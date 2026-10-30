// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/serial_host.hpp"

#include <atomic>
#include <optional>
#include <charconv>
#include <iterator>

#include "core/message.hpp"
#include "remote/net.hpp"

namespace freetvc {

struct SerialHost::Tcp {
    remote::Listener listener;
    remote::Connection connection;
    std::mutex connection_mutex;
    std::atomic<bool> open{false};
    std::atomic<bool> stopping{false};
    std::thread reader;
};

namespace {

std::optional<std::uint16_t> parse_port(std::string_view text) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

SerialHost::SerialHost() = default;

SerialHost::~SerialHost() {
    stop_tcp();
    flush();
}

void SerialHost::stop_tcp() {
    if (!tcp_) {
        return;
    }
    tcp_->stopping = true;
    tcp_->listener.close();
    {
        std::lock_guard lock(tcp_->connection_mutex);
        tcp_->connection.shutdown();
    }
    if (tcp_->reader.joinable()) {
        tcp_->reader.join();
    }
    tcp_.reset();
}

bool SerialHost::set_peer(const std::string& spec, std::string& error) {
    if (spec == "off" || spec == "on") {
        stop_tcp();
        peer_ = spec == "on" ? Peer::Host : Peer::Off;
        port_ = 0;
        return true;
    }
    const bool listen = spec == "listen" || spec.rfind("listen:", 0) == 0;
    if (!listen && spec.rfind("connect:", 0) != 0) {
        error = msg("the serial line is off, on, listen[:PORT] or connect:PORT");
        return false;
    }
    const std::string_view number = spec.size() > (listen ? 7u : 8u) ? std::string_view(spec).substr(listen ? 7 : 8) : std::string_view();
    std::optional<std::uint16_t> port = number.empty() && listen ? std::optional<std::uint16_t>(kDefaultPort) : parse_port(number);
    if (!port) {
        error = msg("'%s' is not a TCP port", std::string(number).c_str());
        return false;
    }
    auto tcp = std::make_unique<Tcp>();
    Tcp* t = tcp.get();
    const auto read_loop = [this](remote::Connection& c) {
        char buffer[4096];
        for (;;) {
            const long n = c.receive(buffer, sizeof buffer);
            if (n <= 0) {
                break;
            }
            std::lock_guard lock(mutex_);
            incoming_.insert(incoming_.end(), buffer, buffer + n);
        }
    };
    if (listen) {
        if (!t->listener.listen(*port, error)) {
            return false;
        }
        t->reader = std::thread([t, read_loop] {
            while (!t->stopping) {
                remote::Connection c = t->listener.accept();
                if (!c.open()) {
                    break;
                }
                {
                    std::lock_guard lock(t->connection_mutex);
                    t->connection = std::move(c);
                }
                t->open = true;
                read_loop(t->connection);
                t->open = false;
                std::lock_guard lock(t->connection_mutex);
                t->connection.close();
            }
        });
    } else {
        t->connection = remote::Connection::connect_loopback(*port, error);
        if (!t->connection.open()) {
            error = msg("cannot connect to 127.0.0.1:%s: %s", std::to_string(*port).c_str(), error.c_str());
            return false;
        }
        t->open = true;
        t->reader = std::thread([t, read_loop] {
            read_loop(t->connection);
            t->open = false;
        });
    }
    stop_tcp();
    tcp_ = std::move(tcp);
    peer_ = listen ? Peer::Listen : Peer::Connect;
    port_ = listen ? tcp_->listener.port() : *port;
    return true;
}

std::string SerialHost::peer() const {
    switch (peer_) {
    case Peer::Off:
        return "off";
    case Peer::Host:
        return "on";
    case Peer::Listen:
        return "listen:" + std::to_string(port_);
    case Peer::Connect:
        return "connect:" + std::to_string(port_);
    }
    return "off";
}

void SerialHost::queue(std::string_view bytes) {
    std::lock_guard lock(mutex_);
    incoming_.insert(incoming_.end(), bytes.begin(), bytes.end());
}

bool SerialHost::queue_file(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = msg("cannot read '%s'", path.c_str());
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    queue(bytes);
    return true;
}

std::size_t SerialHost::queued() const {
    std::lock_guard lock(mutex_);
    return incoming_.size();
}

bool SerialHost::set_capture(const std::string& path, std::string& error) {
    flush();
    if (path.empty()) {
        capture_.close();
        capture_path_.clear();
        return true;
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
        error = msg("cannot write '%s'", path.c_str());
        return false;
    }
    capture_ = std::move(file);
    capture_path_ = path;
    return true;
}

std::string SerialHost::take() {
    std::string out;
    out.swap(kept_);
    return out;
}

void SerialHost::flush() {
    if (capture_.is_open()) {
        capture_.flush();
    }
}

bool SerialHost::connected() const {
    switch (peer_) {
    case Peer::Off:
        return false;
    case Peer::Host:
        return true;
    case Peer::Listen:
    case Peer::Connect:
        return tcp_ && tcp_->open;
    }
    return false;
}

bool SerialHost::take(std::uint8_t& byte) {
    std::lock_guard lock(mutex_);
    if (incoming_.empty()) {
        return false;
    }
    byte = incoming_.front();
    incoming_.pop_front();
    ++received_;
    return true;
}

void SerialHost::send(std::uint8_t byte) {
    ++sent_;
    const char c = static_cast<char>(byte);
    if (kept_.size() >= kKept) {
        kept_.erase(0, kept_.size() - kKept + 1);
    }
    kept_ += c;
    if (capture_.is_open()) {
        capture_.put(c);
    }
    if (tcp_ && tcp_->open) {
        std::lock_guard lock(tcp_->connection_mutex);
        tcp_->connection.send_all(std::string_view(&c, 1));
    }
}

std::string SerialHost::describe() const {
    std::string text;
    switch (peer_) {
    case Peer::Off:
        text = "cable unplugged (SER_CHOUT waits for DSR until STOP)";
        break;
    case Peer::Host:
        text = "cable to the host";
        break;
    case Peer::Listen:
        text = "listening on 127.0.0.1:" + std::to_string(port_) + (connected() ? ", a client is connected" : ", no client yet (cable counts as unplugged)");
        break;
    case Peer::Connect:
        text = std::string("connected to 127.0.0.1:") + std::to_string(port_) + (connected() ? "" : " (closed by the peer)");
        break;
    }
    text += std::string(", flow ") + (rts_flow_ ? "rts" : "none") + ", " + std::to_string(sent_) + " bytes sent, " + std::to_string(received_) +
            " received, " + std::to_string(queued()) + " queued";
    if (!capture_path_.empty()) {
        text += ", capture " + capture_path_.string();
    }
    return text;
}

}  // namespace freetvc
