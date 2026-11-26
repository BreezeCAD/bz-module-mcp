// BreezeCAD module: an MCP server, so Claude (and other MCP clients) can
// read and build parts in the running program.
//
//   client --HTTP POST /mcp--> server thread (this file)
//          --queue--> main_tick on BreezeCAD's main thread --> host->execute()
//
// Listens on 127.0.0.1 only; every request needs the token from
// <settings>/mcp/connection.json. Off by default (module.json): the user
// turns it on in Настройки → Модули.

#include "Connection.h"
#include "Net.h"
#include "Protocol.h"
#include "bz_module.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <string>
#include <thread>

namespace {

const BzHostApi* gHost = nullptr;
constexpr char kSettings[] = "mcp-settings";
constexpr char kCatalogRequest[] = R"({"command":"commands.list","args":{}})";

/// A host-style failed reply: {"ok": false, "error": text}.
std::string Failed(const std::string& text) { return nlohmann::json{{"ok", false}, {"error", text}}.dump(); }

/// One request waiting for the main thread.
struct Job {
    std::string request;
    std::string reply;
    bool        done = false;
};

class Server {
public:
    Server() : myProtocol([this](const std::string& request) { return Execute(request); }, gHost->host_version()) {}

    const bzmcp::Connection& Connection() const { return myConnection; }

    std::size_t RefreshCatalog() {
        const char* reply = nullptr;
        if (gHost->execute(kCatalogRequest, &reply) != BZ_OK || !reply)
            throw std::runtime_error("BreezeCAD не вернул каталог команд");
        return myProtocol.UpdateCatalog(nlohmann::json::parse(reply, nullptr, false)).size();
    }

    bool Start(const bzmcp::Connection& c, std::string& error) {
        myConnection = c;
        myListener   = bzmcp::Socket::Listen(c.port, &error);
        if (!myListener.IsOpen()) return false;
        myThread = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() {
        {
            std::lock_guard lock(myMutex);
            myStopping = true;
            myJobs.clear();
        }
        myDone.notify_all();
        if (myThread.joinable()) myThread.join();
        myListener.Close();
    }

    /// Main thread: runs every queued request through the host.
    void RunJobs() {
        std::deque<std::shared_ptr<Job>> jobs;
        {
            std::lock_guard lock(myMutex);
            jobs.swap(myJobs);
        }
        for (const auto& job : jobs) {
            const char* reply = nullptr;
            const bool  ok    = gHost->execute(job->request.c_str(), &reply) == BZ_OK && reply;
            std::lock_guard lock(myMutex);
            job->reply = ok ? reply : Failed("BreezeCAD не выполнил команду");
            job->done  = true;
        }
        if (!jobs.empty()) myDone.notify_all();
    }

private:
    /// Server thread: hands `request` to the main thread and waits.
    std::string Execute(const std::string& request) {
        auto job     = std::make_shared<Job>();
        job->request = request;
        {
            std::lock_guard lock(myMutex);
            if (myStopping) return Failed("Модуль MCP выключается");
            myJobs.push_back(job);
        }
        gHost->request_main();
        std::unique_lock lock(myMutex);
        // A long rebuild may hold the main thread; a modal dialog too.
        if (!myDone.wait_for(lock, std::chrono::minutes(3), [&] { return job->done || myStopping; }))
            return Failed("BreezeCAD занят больше трёх минут (открыт диалог или идёт долгое перестроение)");
        if (!job->done) return Failed("Модуль MCP выключается");
        return job->reply;
    }

    void Loop() {
        while (true) {
            {
                std::lock_guard lock(myMutex);
                if (myStopping) return;
            }
            bzmcp::Socket client = myListener.Accept(250);
            if (!client.IsOpen()) continue;
            client.SetTimeout(5000);   // a stuck client cannot hold the program's exit for long
            client.WriteAll(Answer(client));
        }
    }

    std::string Answer(const bzmcp::Socket& client) {
        using bzmcp::HttpResponse;
        const auto msg = bzmcp::ReadHttp(client);
        if (!msg) return HttpResponse(400, "Bad Request", "text/plain", "bad request");
        const std::string& line = msg->startLine;
        const std::string  method = line.substr(0, line.find(' '));
        const std::string  path   = line.substr(line.find(' ') + 1, line.rfind(' ') - line.find(' ') - 1);
        if (path != "/mcp" && path.rfind("/mcp?", 0) != 0) return HttpResponse(404, "Not Found", "text/plain", "use /mcp");
        // A web page in a browser sends Origin: it must not drive the CAD.
        if (const std::string origin = msg->Header("origin"); !origin.empty() && origin != "null")
            return HttpResponse(403, "Forbidden", "text/plain", "browsers are not allowed");
        if (msg->Header("authorization") != "Bearer " + myConnection.token)
            return HttpResponse(401, "Unauthorized", "text/plain",
                                "token required: Authorization: Bearer <token from BreezeCAD/mcp/connection.json>");
        if (method == "GET") return HttpResponse(405, "Method Not Allowed", "text/plain", "no event stream");   // no server push
        if (method == "DELETE") return HttpResponse(200, "OK", "", "");                                     // no sessions to end
        if (method != "POST") return HttpResponse(405, "Method Not Allowed", "text/plain", "POST only");
        const auto reply = myProtocol.Handle(msg->body);
        if (!reply) return HttpResponse(202, "Accepted", "", "");
        return HttpResponse(200, "OK", "application/json; charset=utf-8", *reply);
    }

    bzmcp::Protocol                  myProtocol;
    bzmcp::Connection                myConnection;
    bzmcp::Socket                    myListener;
    std::thread                      myThread;
    std::mutex                       myMutex;
    std::condition_variable          myDone;
    std::deque<std::shared_ptr<Job>> myJobs;
    bool                             myStopping = false;
};

std::unique_ptr<Server> gServer;

void Log(int32_t level, const std::string& text) { gHost->log(level, text.c_str()); }

std::filesystem::path Utf8Path(const char* text) {
    const std::string s = text ? text : "";
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

void ShowSettings(const std::string& status = {}) {
    using Json = nlohmann::json;
    Json items = Json::array({
        {{"type", "text"}, {"text", "Адрес MCP-сервера"}},
        {{"type", "mono"}, {"text", gServer->Connection().Url()}}
    });
    if (!status.empty()) items.push_back({{"type", "note"}, {"text", status}});
    items.push_back({{"type", "buttons"}, {"items", Json::array({
        {{"id", "refresh"}, {"label", "Запросить команды"}}
    })}});
    const Json panel = {{"id", kSettings}, {"title", "Настройки MCP"}, {"items", std::move(items)}};
    gHost->show_panel(panel.dump().c_str());
}

void McpAction(const char* id) {
    if (gServer && id && std::string_view(id) == kSettings) ShowSettings();
}

void McpPanelEvent(const char* eventJson) {
    if (!gServer || !eventJson) return;
    const auto event = nlohmann::json::parse(eventJson, nullptr, false);
    if (!event.is_object() || !event.contains("panel") || event["panel"] != kSettings ||
        !event.contains("id") || event["id"] != "refresh") return;
    std::string status;
    try {
        status = "Доступно команд: " + std::to_string(gServer->RefreshCatalog());
    } catch (const std::exception& error) {
        status = "Не удалось обновить команды. " + std::string(error.what());
    }
    ShowSettings(status);
}

int32_t McpInit(const BzHostApi* host) {
    if (!host || host->abi_version != BZ_MODULE_ABI_VERSION || !BZ_HAS(host, execute) || !BZ_HAS(host, config_dir))
        return BZ_EFAIL;   // a host before 0.3: no commands to offer
    gHost = host;
    std::string error;
    const auto  connection = bzmcp::PrepareConnection(bzmcp::ConnectionFile(Utf8Path(host->config_dir())), &error);
    if (!connection) {
        Log(BZ_LOG_ERROR, "MCP: " + error);
        return BZ_EFAIL;
    }
    gServer = std::make_unique<Server>();
    if (!gServer->Start(*connection, error)) {
        Log(BZ_LOG_ERROR, "MCP: " + error + " — поменяйте «port» в mcp/connection.json папки настроек");
        gServer.reset();
        return BZ_EFAIL;
    }
    if (BZ_HAS(host, add_action) && BZ_HAS(host, show_panel))
        host->add_action(R"({"id":"mcp-settings","label":"Настройки MCP","icon":"ui-settings","menu":"settings"})");
    Log(BZ_LOG_INFO, "MCP-сервер слушает " + connection->Url());
    return BZ_OK;
}

void McpShutdown() {
    if (gServer) gServer->Stop();
    gServer.reset();
    if (gHost) Log(BZ_LOG_INFO, "MCP-сервер остановлен");
    gHost = nullptr;
}

void McpMainTick() {
    if (gServer) gServer->RunJobs();
}

const BzModuleApi kApi = {BZ_MODULE_ABI_VERSION, sizeof(BzModuleApi), &McpInit, &McpShutdown, &McpMainTick, &McpAction, &McpPanelEvent};

} // namespace

extern "C" BZ_MODULE_EXPORT const BzModuleApi* bz_module_entry(void) { return &kApi; }
