// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/mcp.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include "core/version.hpp"
#include "remote/mcp_tools.hpp"

namespace freetvc::remote {

using mcp::json;

namespace {

constexpr const char* kMetaVersion = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kMetaCapabilities = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kMetaServerInfo = "io.modelcontextprotocol/serverInfo";
// Legacy revisions served after an `initialize` (both protocol eras); newest first
constexpr std::array<const char*, 4> kLegacyVersions{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kHeaderMismatch = -32020;
constexpr int kUnsupportedVersion = -32022;
constexpr std::int64_t kListTtlMs = 3'600'000;

json server_info() { return {{"name", "freeTVC"}, {"title", "freeTVC - Videoton TV Computer emulator"}, {"version", kVersion}}; }

json supported_versions() {
    json v = json::array({McpServer::kModernVersion});
    for (const char* legacy : kLegacyVersions) {
        v.push_back(legacy);
    }
    return v;
}

McpReply error(const json& id, int code, const std::string& message, int status, const json& data = nullptr) {
    json e{{"code", code}, {"message", message}};
    if (!data.is_null()) {
        e["data"] = data;
    }
    json reply{{"jsonrpc", "2.0"}, {"error", e}};
    if (!id.is_null()) {
        reply["id"] = id;
    }
    return {status, reply.dump()};
}

McpReply result(const json& id, json body) { return {200, json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(body)}}.dump()}; }

// "=?base64?...?=" header values (Streamable HTTP "Value Encoding")
std::string decode_header_value(const std::string& value) {
    if (value.size() < 11 || value.rfind("=?base64?", 0) != 0 || value.substr(value.size() - 2) != "?=") {
        return value;
    }
    const std::string_view b64 = std::string_view(value).substr(9, value.size() - 11);
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : b64) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

struct McpServer::Impl {
    std::vector<mcp::Tool> tools;

    json list() const {
        json list = json::array();
        for (const mcp::Tool& t : tools) {
            json tool{{"name", t.name}, {"title", t.title}, {"description", t.description}, {"inputSchema", t.input_schema}};
            if (!t.annotations.is_null()) {
                tool["annotations"] = t.annotations;
            }
            list.push_back(std::move(tool));
        }
        return list;
    }

    // nullptr = unknown tool
    const mcp::Tool* find(const std::string& name) const {
        const auto it = std::find_if(tools.begin(), tools.end(), [&](const mcp::Tool& t) { return t.name == name; });
        return it == tools.end() ? nullptr : &*it;
    }

    static json call(const mcp::Tool& tool, const json& arguments) {
        try {
            return tool.run(arguments);
        } catch (const mcp::ToolError& e) {
            return {{"content", json::array({{{"type", "text"}, {"text", e.message}}})}, {"isError", true}};
        } catch (const std::exception& e) {
            return {{"content", json::array({{{"type", "text"}, {"text", std::string("bad arguments: ") + e.what()}}})},
                    {"isError", true}};
        }
    }

    // The call, kept in the server's call log for Bench's agent lane
    static json call_logged(McpServer& server, const mcp::Tool& tool, const json& arguments) {
        constexpr std::size_t kShort = 160;
        McpCall entry;
        entry.number = ++server.call_number_;
        entry.tool = tool.name;
        entry.arguments = arguments.dump(-1, ' ', false, json::error_handler_t::replace).substr(0, kShort);
        entry.frame = server.monitor_.debugger().machine().frame_count();
        const auto start = std::chrono::steady_clock::now();
        json body = call(tool, arguments);
        entry.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        entry.error = body.value("isError", false);
        if (body.contains("content") && body["content"].is_array() && !body["content"].empty()) {
            const json& first = body["content"][0];
            entry.result = first.value("type", "") == "text" ? first.value("text", "").substr(0, kShort) : "[" + first.value("type", "") + "]";
        }
        server.calls_.push_back(std::move(entry));
        if (server.calls_.size() > kCallLog) {
            server.calls_.pop_front();
        }
        return body;
    }
};

McpServer::McpServer(Monitor& monitor, McpOptions options)
    : monitor_(monitor), options_(std::move(options)), impl_(std::make_unique<Impl>()) {
    impl_->tools = mcp::make_tools(monitor_, options_);
}

McpServer::~McpServer() = default;

std::size_t McpServer::tool_count() const { return impl_->tools.size(); }

McpReply McpServer::handle(std::string_view message, const HttpHeaders* http) {
    json msg;
    try {
        msg = json::parse(message);
    } catch (const json::exception&) {
        return error(nullptr, kParseError, "Parse error", 400);
    }
    if (msg.is_object() && !msg.contains("method") && (msg.contains("result") || msg.contains("error"))) {
        return {202, {}};  // a client's response: this server sends no requests, so there is nothing to match
    }
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method") || !msg["method"].is_string()) {
        return error(msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr), kInvalidRequest, "Invalid Request", 400);
    }
    const std::string method = msg["method"];
    if (!msg.contains("id")) {
        return {202, {}};  // notifications (initialized, cancelled): nothing to answer
    }
    const json id = msg["id"];
    if (!id.is_string() && !id.is_number_integer()) {
        return error(nullptr, kInvalidRequest, "Invalid Request: the id must be a string or an integer", 400);
    }
    const json params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();
    const bool modern = params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains(kMetaVersion);

    if (!modern) {
        // Legacy: `initialize` opens the handshake; later requests carry no _meta and are served the same way
        if (method == "initialize") {
            const std::string requested = params.value("protocolVersion", "");
            const bool known = std::find(kLegacyVersions.begin(), kLegacyVersions.end(), requested) != kLegacyVersions.end();
            return result(id, {{"protocolVersion", known ? requested : std::string(kLegacyVersions[0])},
                               {"capabilities", {{"tools", {{"listChanged", false}}}}},
                               {"serverInfo", server_info()},
                               {"instructions", mcp::instructions()}});
        }
        if (method == "ping") {
            return result(id, json::object());
        }
        if (method == "tools/list") {
            return result(id, {{"tools", impl_->list()}});
        }
        if (method == "tools/call") {
            const mcp::Tool* tool = impl_->find(params.value("name", ""));
            if (!tool) {
                return error(id, kInvalidParams, "Unknown tool: " + params.value("name", ""), 200);
            }
            return result(id, Impl::call_logged(*this, *tool, params.value("arguments", json::object())));
        }
        return error(id, kMethodNotFound, "Method not found: " + method, http ? 404 : 200);
    }

    const json& meta = params["_meta"];
    if (!meta[kMetaVersion].is_string()) {
        return error(id, kInvalidParams, std::string("Invalid params: ") + kMetaVersion + " must be a string", 400);
    }
    const std::string version = meta[kMetaVersion];
    if (http) {
        const std::string* header_version = http->find("mcp-protocol-version");
        const std::string* header_method = http->find("mcp-method");
        if (!header_version || *header_version != version) {
            return error(id, kHeaderMismatch, "Header mismatch: MCP-Protocol-Version must equal _meta " + std::string(kMetaVersion), 400);
        }
        if (!header_method || *header_method != method) {
            return error(id, kHeaderMismatch, "Header mismatch: Mcp-Method must equal the request method", 400);
        }
        if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
            const std::string* name = http->find("mcp-name");
            const std::string body_name = params.value(method == "resources/read" ? "uri" : "name", "");
            if (!name || decode_header_value(*name) != body_name) {
                return error(id, kHeaderMismatch, "Header mismatch: Mcp-Name must equal params." +
                                                      std::string(method == "resources/read" ? "uri" : "name"), 400);
            }
        }
    }
    if (version != kModernVersion) {
        return error(id, kUnsupportedVersion, "Unsupported protocol version", 400,
                     {{"supported", supported_versions()}, {"requested", version}});
    }
    if (!meta.contains(kMetaCapabilities) || !meta[kMetaCapabilities].is_object()) {
        return error(id, kInvalidParams, std::string("Invalid params: _meta needs ") + kMetaCapabilities, 400);
    }
    const json result_meta{{kMetaServerInfo, server_info()}};
    // Caching hints are required on these results; the tool set never changes while the program runs
    if (method == "server/discover") {
        return result(id, {{"resultType", "complete"},
                           {"supportedVersions", supported_versions()},
                           {"capabilities", {{"tools", {{"listChanged", false}}}}},
                           {"instructions", mcp::instructions()},
                           {"ttlMs", kListTtlMs},
                           {"cacheScope", "public"},
                           {"_meta", result_meta}});
    }
    if (method == "tools/list") {
        return result(id, {{"resultType", "complete"}, {"tools", impl_->list()}, {"ttlMs", kListTtlMs}, {"cacheScope", "public"},
                           {"_meta", result_meta}});
    }
    if (method == "tools/call") {
        const mcp::Tool* tool = impl_->find(params.value("name", ""));
        if (!tool) {
            return error(id, kInvalidParams, "Unknown tool: " + params.value("name", ""), 200);
        }
        json body = Impl::call_logged(*this, *tool, params.value("arguments", json::object()));
        body["resultType"] = "complete";
        body["_meta"] = result_meta;
        return result(id, std::move(body));
    }
    return error(id, kMethodNotFound, "Method not found: " + method, 404);
}

}  // namespace freetvc::remote
