// SPDX-License-Identifier: GPL-3.0-or-later
// The MCP tools (inside freetvc_remote only): definitions and handlers over the monitor and the debug API.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "json.hpp"
#include "remote/mcp.hpp"
#include "remote/monitor.hpp"

namespace freetvc::remote::mcp {

using nlohmann::json;

// A tool execution error: reported with isError, so the model can correct its arguments.
struct ToolError {
    std::string message;
};

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    json input_schema;
    json annotations;
    std::function<json(const json& arguments)> run;  // a CallToolResult without resultType
};

// In the order tools/list returns them (deterministic, as the spec asks).
std::vector<Tool> make_tools(Monitor& monitor, const McpOptions& options);

// What initialize and server/discover tell the model about this server.
const char* instructions();

}  // namespace freetvc::remote::mcp
