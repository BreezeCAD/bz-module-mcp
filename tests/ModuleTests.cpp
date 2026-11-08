// The real module, loaded as BreezeCAD loads it (the library and its
// bz_module_entry), with a fake host table in place of the program: it
// serves requests over 127.0.0.1 through the main thread and shuts down
// cleanly. (In BreezeCAD's repository this test went through its
// ModuleManager; here the host is played by the test.)

#include "Net.h"
#include "bz_module.h"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// --- the fake program ---------------------------------------------------------

std::atomic<bool> gWantTick{false};
std::string       gConfigDir;
std::string       gReply;

const char* HostVersion() { return "0.2.5"; }
void        HostLog(int32_t, const char* message) { MESSAGE("журнал: " << (message ? message : "")); }
int32_t     NoAssembly(const char**, const char**) { return BZ_ENODATA; }
int32_t     NoCount(int32_t*) { return BZ_ENODATA; }
int32_t     NoComponent(int32_t, BzComponentInfo*) { return BZ_ENODATA; }
int32_t     NoBounds(uint64_t, double*, double*) { return BZ_ENODATA; }
void        RequestMain() { gWantTick = true; }
const char* ConfigDir() { return gConfigDir.c_str(); }

/// Two commands, as in the protocol tests.
int32_t Execute(const char* request, const char** reply) {
    const Json r = Json::parse(request);
    if (r["command"] == "commands.list")
        gReply = Json{{"ok", true},
                      {"result",
                       {{{"name", "commands.list"}, {"description", "list"}, {"inputSchema", {{"type", "object"}}}},
                        {{"name", "part.set_parameter"}, {"description", "set"}, {"inputSchema", {{"type", "object"}}}, {"changes", true}}}}}
                     .dump();
    else
        gReply = R"({"ok":false,"error":"нет такой команды"})";
    *reply = gReply.c_str();
    return BZ_OK;
}

BzHostApi FakeHost() {
    BzHostApi h;
    std::memset(&h, 0, sizeof h);
    h.abi_version      = BZ_MODULE_ABI_VERSION;
    h.struct_size      = sizeof h;
    h.host_version     = HostVersion;
    h.log              = HostLog;
    h.assembly_name    = NoAssembly;
    h.component_count  = NoCount;
    h.component_at     = NoComponent;
    h.component_bounds = NoBounds;
    h.request_main     = RequestMain;
    h.execute          = Execute;
    h.config_dir       = ConfigDir;
    return h;
}

const BzModuleApi* LoadModule(const char* path) {
#if defined(_WIN32)
    void* lib = reinterpret_cast<void*>(LoadLibraryA(path));
    if (!lib) return nullptr;
    auto entry = reinterpret_cast<BzModuleEntryFn>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), BZ_MODULE_ENTRY_NAME)));
#else
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) return nullptr;
    auto entry = reinterpret_cast<BzModuleEntryFn>(dlsym(lib, BZ_MODULE_ENTRY_NAME));
#endif
    return entry ? entry() : nullptr;   // the library stays loaded until the process ends
}

std::string Post(std::uint16_t port, const std::string& token, const std::string& body) {
    const bzmcp::Socket s = bzmcp::Socket::Connect(port);
    if (!s.IsOpen()) return "no connection";
    s.SetTimeout(20000);
    s.WriteAll("POST /mcp HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token + "\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body);
    const auto reply = bzmcp::ReadHttp(s);
    return reply ? reply->startLine + "\n" + reply->body : "no reply";
}

} // namespace

TEST_CASE("MCP module: serves requests through the main thread and shuts down cleanly") {
    const fs::path root = fs::temp_directory_path() / "bz_mcp_module";
    fs::remove_all(root);
    // A port of its own, so a BreezeCAD running next to the tests does not matter.
    fs::create_directories(root / "config" / "mcp");
    std::ofstream(root / "config" / "mcp" / "connection.json") << R"({"port": 47399, "token": "test-token-1234567890"})";
    const std::u8string config = (root / "config").generic_u8string();
    gConfigDir = std::string(config.begin(), config.end());

    const BzModuleApi* module = LoadModule(BZ_MCP_MODULE);
    REQUIRE(module);
    REQUIRE(module->abi_version == BZ_MODULE_ABI_VERSION);
    REQUIRE(BZ_HAS(module, main_tick));
    static const BzHostApi host = FakeHost();
    REQUIRE(module->init(&host) == BZ_OK);

    // Requests wait for the main thread: this test plays it, ticking when asked.
    const auto ask = [&](const std::string& token, const std::string& body) {
        auto reply = std::async(std::launch::async, [=] { return Post(47399, token, body); });
        while (reply.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
            if (gWantTick.exchange(false)) module->main_tick();
        return reply.get();
    };
    CHECK(ask("wrong", R"({"jsonrpc":"2.0","id":1,"method":"ping"})").rfind("HTTP/1.1 401", 0) == 0);
    const std::string list = ask("test-token-1234567890", R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    CHECK(list.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(list.find("part_set_parameter") != std::string::npos);
    const std::string note = ask("test-token-1234567890", R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK(note.rfind("HTTP/1.1 202", 0) == 0);

    const auto t0 = std::chrono::steady_clock::now();
    module->shutdown();   // the server thread stops and is joined
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    CHECK(Post(47399, "test-token-1234567890", "{}") == "no connection");
    fs::remove_all(root);
}

TEST_CASE("MCP package: the module, the bridge, module.json and README inside one folder") {
    // The .bzmod is a ZIP: its central directory lists the names; no unzip needed.
    std::ifstream f(BZ_MCP_PACKAGE, std::ios::binary);
    REQUIRE(f);
    const std::string zip((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto has = [&](const std::string& name) { return zip.find(name) != std::string::npos; };
    const std::string lib    = fs::path(BZ_MCP_MODULE).filename().string();
    const std::string bridge = fs::path(BZ_MCP_BRIDGE).filename().string();
    CHECK(has("mcp/module.json"));
    CHECK(has("mcp/README.md"));
    CHECK(has("mcp/" + lib));
    CHECK(has("mcp/" + bridge));
}
