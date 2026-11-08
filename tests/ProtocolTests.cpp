// The MCP module's protocol layer (src/Protocol), with a fake host.

#include "Protocol.h"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using Json = nlohmann::json;

namespace {

/// A host with two commands; records what it was asked.
struct FakeHost {
    std::string last;
    std::string operator()(const std::string& request) {
        last = request;
        const Json r = Json::parse(request);
        if (r["command"] == "commands.list")
            return Json{{"ok", true},
                        {"result",
                         {{{"name", "commands.list"}, {"description", "list"}, {"inputSchema", {{"type", "object"}}}},
                          {{"name", "part.tree"}, {"description", "tree"}, {"inputSchema", {{"type", "object"}}}, {"changes", false}},
                          {{"name", "part.set_parameter"}, {"description", "set"}, {"inputSchema", {{"type", "object"}}}, {"changes", true}}}}}
                .dump();
        if (r["command"] == "part.tree") return R"({"ok":true,"result":{"features":[]}})";
        return R"({"ok":false,"error":"Нет параметра «x»"})";
    }
};

Json Call(bzmcp::Protocol& p, const std::string& method, Json params = Json::object(), int id = 1) {
    const auto reply = p.Handle(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
    REQUIRE(reply);
    return Json::parse(*reply);
}

} // namespace

TEST_CASE("MCP: initialize, notifications, ping") {
    FakeHost        host;
    bzmcp::Protocol p([&](const std::string& r) { return host(r); }, "0.2.0");

    Json init = Call(p, "initialize", {{"protocolVersion", "2025-03-26"}});
    CHECK(init["result"]["protocolVersion"] == "2025-03-26");
    CHECK(init["result"]["serverInfo"]["name"] == "breezecad");
    CHECK(init["result"]["capabilities"].contains("tools"));
    CHECK(Call(p, "initialize", {{"protocolVersion", "1999-01-01"}})["result"]["protocolVersion"] == "2025-06-18");

    CHECK_FALSE(p.Handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    CHECK(Call(p, "ping")["result"] == Json::object());
    CHECK(Call(p, "nonsense")["error"]["code"] == -32601);
    CHECK(Json::parse(*p.Handle("{oops"))["error"]["code"] == -32700);
}

TEST_CASE("MCP: host commands become tools") {
    FakeHost        host;
    bzmcp::Protocol p([&](const std::string& r) { return host(r); }, "0.2.0");

    const Json tools = Call(p, "tools/list")["result"]["tools"];
    REQUIRE(tools.size() == 2);   // commands.list itself is not a tool
    CHECK(tools[0]["name"] == "part_tree");
    CHECK(tools[0]["annotations"]["readOnlyHint"] == true);
    CHECK(tools[1]["name"] == "part_set_parameter");
    CHECK(tools[1]["annotations"]["readOnlyHint"] == false);

    Json r = Call(p, "tools/call", {{"name", "part_tree"}, {"arguments", {{"a", 1}}}});
    CHECK(r["result"]["isError"] == false);
    CHECK(Json::parse(r["result"]["content"][0]["text"].get<std::string>())["features"].is_array());
    CHECK(Json::parse(host.last) == Json{{"command", "part.tree"}, {"args", {{"a", 1}}}});

    r = Call(p, "tools/call", {{"name", "part_set_parameter"}});
    CHECK(r["result"]["isError"] == true);
    CHECK(r["result"]["content"][0]["text"] == "Нет параметра «x»");
    CHECK(Call(p, "tools/call", {{"name", "no_such"}})["result"]["isError"] == true);
}

TEST_CASE("MCP: a client that calls a tool without listing first") {
    FakeHost        host;
    bzmcp::Protocol p([&](const std::string& r) { return host(r); }, "0.2.0");
    CHECK(Call(p, "tools/call", {{"name", "part_tree"}})["result"]["isError"] == false);
    CHECK(bzmcp::Protocol::ToolName("app.new_part") == "app_new_part");
}
