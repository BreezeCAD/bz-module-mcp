#pragma once
// MCP (Model Context Protocol) over JSON-RPC 2.0: initialize, ping,
// tools/list, tools/call. The tools are the host's commands
// ("commands.list"): "part.tree" is offered as the tool "part_tree".
// Transport-free: the HTTP server and tests feed it message bodies.

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace bzmcp {

class Protocol {
public:
    /// `execute`: one host request JSON in, the reply JSON out (bz_module.h).
    Protocol(std::function<std::string(const std::string&)> execute, std::string hostVersion);

    /// A JSON-RPC message (or a batch) in; the reply, or nullopt when it
    /// needs none (notifications). Never throws.
    std::optional<std::string> Handle(const std::string& body);

    /// Installs a commands.list reply and returns the current tool list.
    /// May run on the main thread while the server handles a request.
    nlohmann::json UpdateCatalog(const nlohmann::json& reply);

    /// Tool name for a command: "part.tree" -> "part_tree".
    static std::string ToolName(const std::string& command);

private:
    nlohmann::json HandleOne(const nlohmann::json& message, bool& reply);
    nlohmann::json Initialize(const nlohmann::json& params) const;
    nlohmann::json ListTools();
    nlohmann::json UpdateCatalog(const nlohmann::json& reply, std::optional<std::size_t> expectedRevision);
    nlohmann::json CallTool(const nlohmann::json& params);
    /// The host's reply as {"ok", "result"/"error"}.
    nlohmann::json Host(const std::string& command, const nlohmann::json& args);

    std::function<std::string(const std::string&)> myExecute;
    std::string                                    myHostVersion;
    std::mutex                                     myCatalogMutex;
    std::size_t                                    myCatalogRevision = 0;
    nlohmann::json                                 myTools;
    std::map<std::string, std::string>             myCommands;   ///< tool name -> command
};

} // namespace bzmcp
