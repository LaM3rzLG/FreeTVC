// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/net.hpp"

#include <cerrno>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace freetvc::remote {

namespace {

#ifdef _WIN32
using Native = SOCKET;
constexpr Native kNone = INVALID_SOCKET;

void startup() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}

void close_native(Native s) { closesocket(s); }
void shutdown_native(Native s) { ::shutdown(s, SD_BOTH); }
std::string last_error() { return "Winsock error " + std::to_string(WSAGetLastError()); }
#else
using Native = int;
constexpr Native kNone = -1;

void startup() {}
void close_native(Native s) { ::close(s); }
void shutdown_native(Native s) { ::shutdown(s, SHUT_RDWR); }
std::string last_error() { return "socket error " + std::to_string(errno); }
#endif

Native native(std::intptr_t handle) { return handle == -1 ? kNone : static_cast<Native>(handle); }
std::intptr_t wrap(Native s) { return s == kNone ? -1 : static_cast<std::intptr_t>(s); }

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

}  // namespace

Connection::Connection(Connection&& other) noexcept : handle_(std::exchange(other.handle_, kInvalid)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalid);
    }
    return *this;
}

Connection::~Connection() { close(); }

Connection Connection::connect_loopback(std::uint16_t port, std::string& error) {
    startup();
    const Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNone) {
        error = last_error();
        return {};
    }
    const sockaddr_in addr = loopback(port);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        error = last_error();
        close_native(s);
        return {};
    }
    return Connection(wrap(s));
}

bool Connection::send_all(std::string_view data) {
    while (!data.empty() && open()) {
#ifdef _WIN32
        const int n = ::send(native(handle_), data.data(), static_cast<int>(data.size()), 0);
#else
        const auto n = ::send(native(handle_), data.data(), data.size(), MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return data.empty();
}

long Connection::receive(char* buffer, std::size_t size) {
    if (!open()) {
        return 0;
    }
#ifdef _WIN32
    return ::recv(native(handle_), buffer, static_cast<int>(size), 0);
#else
    return static_cast<long>(::recv(native(handle_), buffer, size, 0));
#endif
}

void Connection::shutdown() {
    if (open()) {
        shutdown_native(native(handle_));
    }
}

void Connection::close() {
    if (open()) {
        close_native(native(handle_));
        handle_ = kInvalid;
    }
}

Listener::~Listener() { close(); }

bool Listener::listen(std::uint16_t port, std::string& error) {
    startup();
    close();
    const Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNone) {
        error = last_error();
        return false;
    }
#ifndef _WIN32
    const int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
    sockaddr_in addr = loopback(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 4) != 0) {
        error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (" + last_error() + ")";
        close_native(s);
        return false;
    }
    socklen_t length = sizeof addr;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &length);
    port_ = ntohs(addr.sin_port);
    handle_ = wrap(s);
    return true;
}

Connection Listener::accept() {
    const std::intptr_t handle = handle_.load();
    if (handle == -1) {
        return {};
    }
    const Native s = ::accept(native(handle), nullptr, nullptr);
    return Connection(wrap(s));
}

void Listener::close() {
    const std::intptr_t handle = handle_.exchange(-1);
    if (handle != -1) {
        shutdown_native(native(handle));
        close_native(native(handle));
    }
}

}  // namespace freetvc::remote
