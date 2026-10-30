// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/mcp_host.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <random>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace freetvc::remote {

namespace {

constexpr std::size_t kMaxHeader = 16 * 1024;
constexpr std::size_t kMaxBody = 4 * 1024 * 1024;

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t");
    const std::size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

const char* reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

std::string response(int status, const std::string& body, const std::string& extra = {}) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    if (!body.empty()) {
        r += "Content-Type: application/json\r\n";
    }
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n";
    return r + body;
}

std::string error_body(int code, const std::string& message) {
    return "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + message + "\"}}";
}

}  // namespace

struct McpHost::Connection {
    remote::Connection socket;
    std::thread thread;
    std::atomic<bool> done{false};
};

McpHost::McpHost(McpServer& server) : server_(server) {}

McpHost::~McpHost() {
    stopping_ = true;
    listener_.close();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    std::vector<std::unique_ptr<Connection>> connections;
    {
        std::lock_guard lock(mutex_);
        for (auto& p : queue_) {
            p->reply.set_value({503, error_body(-32603, "the emulator is shutting down")});
        }
        queue_.clear();
        connections = std::move(connections_);
    }
    for (auto& c : connections) {
        c->socket.shutdown();
        if (c->thread.joinable()) {
            c->thread.join();
        }
    }
}

std::string McpHost::make_token() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i) {
        token.push_back(kHex[rd() & 15]);
    }
    return token;
}

void McpHost::attach_stdio() {
    stdio_attached_ = true;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // one '\n' per message, not "\r\n"
#endif
    // std::getline cannot be interrupted: the thread ends with the process, or at end of file
    std::thread([this] { stdio_loop(); }).detach();
}

bool McpHost::listen_http(std::uint16_t port, std::string token, std::string& error) {
    token_ = std::move(token);
    if (!listener_.listen(port, error)) {
        return false;
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

std::future<McpReply> McpHost::enqueue(std::string body, std::optional<HttpHeaders> headers) {
    auto p = std::make_unique<Pending>();
    p->body = std::move(body);
    p->headers = std::move(headers);
    std::future<McpReply> f = p->reply.get_future();
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(p));
    }
    wake_.notify_all();
    return f;
}

bool McpHost::queue_empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
}

void McpHost::stdio_loop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") != std::string::npos) {
            enqueue(line, std::nullopt);
        }
    }
    stdio_eof_ = true;
    wake_.notify_all();
}

void McpHost::service() {
    std::deque<std::unique_ptr<Pending>> work;
    {
        std::lock_guard lock(mutex_);
        work.swap(queue_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if ((*it)->done) {
                (*it)->thread.join();
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& p : work) {
        McpReply reply = server_.handle(p->body, p->headers ? &*p->headers : nullptr);
        if (!p->headers) {
            if (!reply.body.empty()) {
                // One message per line on stdout, nothing else (stdio transport)
                std::fwrite(reply.body.data(), 1, reply.body.size(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
            }
            p->reply.set_value(std::move(reply));
            continue;
        }
        p->reply.set_value(std::move(reply));
    }
}

void McpHost::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, timeout, [&] { return !queue_.empty() || (stdio_attached_ && stdio_eof_); });
}

void McpHost::accept_loop() {
    while (!stopping_) {
        remote::Connection s = listener_.accept();
        if (!s.open()) {
            if (stopping_) {
                break;
            }
            continue;
        }
        std::lock_guard lock(mutex_);
        auto c = std::make_unique<Connection>();
        c->socket = std::move(s);
        Connection* raw = c.get();
        c->thread = std::thread([this, raw] { serve(raw); });
        connections_.push_back(std::move(c));
    }
}

// One keep-alive connection: requests in order, each answered before the next is read
void McpHost::serve(Connection* c) {
    std::string buffer;
    char chunk[8192];
    const auto fill = [&](std::size_t want) {
        while (buffer.size() < want) {
            const long n = c->socket.receive(chunk, sizeof chunk);
            if (n <= 0) {
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
        return true;
    };
    while (!stopping_) {
        std::size_t end;
        while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (buffer.size() > kMaxHeader || !fill(buffer.size() + 1)) {
                c->done = true;
                return;
            }
        }
        const std::string head = buffer.substr(0, end);
        buffer.erase(0, end + 4);

        std::size_t eol = head.find("\r\n");
        const std::string request_line = head.substr(0, eol);
        HttpHeaders headers;
        while (eol != std::string::npos) {
            const std::size_t next = head.find("\r\n", eol + 2);
            const std::string line = head.substr(eol + 2, next == std::string::npos ? std::string::npos : next - eol - 2);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                headers.values[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            }
            eol = next;
        }
        const std::size_t sp1 = request_line.find(' ');
        const std::size_t sp2 = request_line.find(' ', sp1 + 1);
        const std::string method = request_line.substr(0, sp1);
        std::string path = sp1 == std::string::npos ? std::string() : request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        path = path.substr(0, path.find('?'));

        if (const std::string* te = headers.find("transfer-encoding"); te && lower(*te) != "identity") {
            c->socket.send_all(response(411, error_body(-32600, "send the body with Content-Length")));
            break;
        }
        std::size_t length = 0;
        if (const std::string* cl = headers.find("content-length")) {
            length = static_cast<std::size_t>(std::strtoull(cl->c_str(), nullptr, 10));
        }
        if (length > kMaxBody) {
            c->socket.send_all(response(413, error_body(-32600, "request too large")));
            break;
        }
        if (const std::string* expect = headers.find("expect"); expect && lower(*expect) == "100-continue") {
            c->socket.send_all("HTTP/1.1 100 Continue\r\n\r\n");
        }
        if (!fill(length)) {
            break;
        }
        const std::string body = buffer.substr(0, length);
        buffer.erase(0, length);

        // Checks that need no machine: Origin (DNS rebinding), the token, the endpoint and method
        std::string reply;
        const std::string port = std::to_string(listener_.port());
        const std::string* origin = headers.find("origin");
        const std::string* auth = headers.find("authorization");
        if (origin && *origin != "http://127.0.0.1:" + port && *origin != "http://localhost:" + port &&
            *origin != "http://[::1]:" + port) {
            reply = response(403, error_body(-32600, "Origin not allowed"));
        } else if (path != "/mcp") {
            reply = response(404, error_body(-32600, "the MCP endpoint is /mcp"));
        } else if (method != "POST") {
            reply = response(405, {}, "Allow: POST\r\n");
        } else if (!auth || *auth != "Bearer " + token_) {
            reply = response(401, error_body(-32600, "missing or wrong bearer token"), "WWW-Authenticate: Bearer\r\n");
        } else {
            std::future<McpReply> f = enqueue(body, headers);
            while (f.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
                if (stopping_) {
                    c->done = true;
                    return;
                }
            }
            const McpReply r = f.get();
            reply = response(r.http_status, r.body);
        }
        if (!c->socket.send_all(reply)) {
            break;
        }
        if (const std::string* conn = headers.find("connection"); conn && lower(*conn) == "close") {
            break;
        }
    }
    c->done = true;
}

}  // namespace freetvc::remote
