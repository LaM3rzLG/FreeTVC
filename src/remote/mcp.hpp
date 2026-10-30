// SPDX-License-Identifier: GPL-3.0-or-later
// MCP server: JSON-RPC for 2026-07-28 (stateless, per-request _meta) and for legacy clients that start with
// `initialize` (2025-11-25 back to 2024-11-05); the TVC tools (design/debug-and-remote.md "MCP server").
// Not thread-safe: the host thread calls handle() between run slices.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "remote/monitor.hpp"

namespace freetvc::remote {

inline constexpr std::uint16_t kMcpPort = 6584;

struct McpOptions {
    std::string machine_name;             // shown by machine_info (the preset)
    bool allow_monitor_command = false;   // the raw text monitor passthrough (opt-in)
};

// What the HTTP transport saw besides the body; header names in lower case.
struct HttpHeaders {
    std::map<std::string, std::string> values;
    const std::string* find(const std::string& name) const {
        const auto it = values.find(name);
        return it == values.end() ? nullptr : &it->second;
    }
};

struct McpReply {
    int http_status = 200;  // 202 for an accepted notification; 4xx for protocol errors
    std::string body;       // one JSON-RPC message, or empty (notifications)
};

// One tools/call as Bench's agent lane shows it (design/ui.md "Bench", 7d).
struct McpCall {
    std::uint64_t number = 0;   // 1, 2, ... since the server started
    std::string tool;
    std::string arguments;      // compact JSON, cut short
    std::string result;         // the first text content, cut short; "[image]" for a screenshot
    bool error = false;
    double milliseconds = 0.0;
    std::uint64_t frame = 0;    // Video::frame_count() when the call came in
};

class McpServer {
public:
    static constexpr std::size_t kCallLog = 256;

    static constexpr const char* kModernVersion = "2026-07-28";

    McpServer(Monitor& monitor, McpOptions options);
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // One JSON-RPC message from stdio (`http` null) or an HTTP POST body.
    McpReply handle(std::string_view message, const HttpHeaders* http = nullptr);

    Monitor& monitor() { return monitor_; }
    // After a preset switch in the host.
    void set_machine_name(std::string name) { options_.machine_name = std::move(name); }
    void set_allow_monitor_command(bool on) { options_.allow_monitor_command = on; }
    std::size_t tool_count() const;
    // The latest kCallLog tool calls, oldest first.
    const std::deque<McpCall>& calls() const { return calls_; }

    struct Impl;

private:
    Monitor& monitor_;
    McpOptions options_;
    std::unique_ptr<Impl> impl_;
    std::deque<McpCall> calls_;
    std::uint64_t call_number_ = 0;
};

}  // namespace freetvc::remote
