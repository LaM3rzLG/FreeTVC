// SPDX-License-Identifier: GPL-3.0-or-later
// Thin TCP layer over POSIX sockets and Winsock: loopback listeners and blocking client connections.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace freetvc::remote {

class Connection {
public:
    Connection() = default;
    explicit Connection(std::intptr_t handle) : handle_(handle) {}
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    ~Connection();

    // 127.0.0.1:port; on failure the connection stays closed and `error` says why.
    static Connection connect_loopback(std::uint16_t port, std::string& error);

    bool open() const { return handle_ != kInvalid; }
    bool send_all(std::string_view data);
    // Blocks; 0 = closed by the peer or by shutdown(), negative = error.
    long receive(char* buffer, std::size_t size);
    // Wakes a thread blocked in receive(); the handle stays allocated until close().
    void shutdown();
    void close();

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t handle_ = kInvalid;
};

class Listener {
public:
    Listener() = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener();

    // Binds 127.0.0.1 only; port 0 picks a free one (see port()).
    bool listen(std::uint16_t port, std::string& error);
    // Blocks until a client connects or close() is called (then the result is closed).
    Connection accept();
    void close();
    std::uint16_t port() const { return port_; }

private:
    std::atomic<std::intptr_t> handle_{-1};  // close() runs on another thread than a blocked accept()
    std::uint16_t port_ = 0;
};

}  // namespace freetvc::remote
