// SPDX-License-Identifier: GPL-3.0-or-later
// MCP transports: stdio (newline-delimited JSON) and a minimal HTTP/1.1 server for POST /mcp (JSON replies only,
// 127.0.0.1, Origin check, bearer token). Reader threads queue messages; the host thread answers them in service().
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "remote/mcp.hpp"
#include "remote/net.hpp"

namespace freetvc::remote {

class McpHost {
public:
    explicit McpHost(McpServer& server);
    ~McpHost();
    McpHost(const McpHost&) = delete;
    McpHost& operator=(const McpHost&) = delete;

    // Requests from stdin, replies on stdout; nothing else may write to stdout.
    void attach_stdio();
    // http://127.0.0.1:port/mcp; every request needs "Authorization: Bearer <token>".
    bool listen_http(std::uint16_t port, std::string token, std::string& error);
    std::uint16_t http_port() const { return listener_.port(); }

    // Host thread: answers every queued message.
    void service();
    void wait(std::chrono::milliseconds timeout);
    // stdin reached end of file (an MCP stdio server should then exit).
    bool stdio_closed() const { return stdio_attached_ && stdio_eof_ && queue_empty(); }

    // A random token for --mcp-http without --mcp-token.
    static std::string make_token();

private:
    struct Pending {
        std::string body;
        std::optional<HttpHeaders> headers;  // empty = stdio
        std::promise<McpReply> reply;
    };
    struct Connection;

    void stdio_loop();
    void accept_loop();
    void serve(Connection* c);
    bool queue_empty() const;
    std::future<McpReply> enqueue(std::string body, std::optional<HttpHeaders> headers);

    McpServer& server_;
    Listener listener_;
    std::thread acceptor_;
    std::string token_;
    std::atomic<bool> stopping_{false};
    bool stdio_attached_ = false;
    std::atomic<bool> stdio_eof_{false};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::unique_ptr<Pending>> queue_;
    std::vector<std::unique_ptr<Connection>> connections_;
};

}  // namespace freetvc::remote
