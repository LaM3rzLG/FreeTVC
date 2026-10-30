// SPDX-License-Identifier: GPL-3.0-or-later
// The GDB stub over TCP: one client at a time on 127.0.0.1. The reader thread only queues the bytes; the host thread
// hands them to GdbServer in service().
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "remote/gdb.hpp"
#include "remote/net.hpp"

namespace freetvc::remote {

class GdbHost {
public:
    explicit GdbHost(GdbServer& server);
    ~GdbHost();
    GdbHost(const GdbHost&) = delete;
    GdbHost& operator=(const GdbHost&) = delete;

    // 127.0.0.1:port (0 = any free port, see port()).
    bool listen(std::uint16_t port, std::string& error);
    std::uint16_t port() const { return listener_.port(); }

    // Host thread: feeds every queued chunk to the server, then sends a stop reply if a continue has stopped.
    void service();
    void wait(std::chrono::milliseconds timeout);

private:
    struct Client {
        Connection connection;
        std::thread reader;
        std::atomic<bool> closed{false};
    };
    enum class Event { Connected, Bytes, Lost };
    struct Message {
        Client* client = nullptr;
        Event event = Event::Bytes;
        std::string bytes;
    };

    void accept_loop();
    void read_loop(Client* client);
    // Queues `out` one packet (with its ack) per entry.
    void queue_packets(const std::string& out);

    GdbServer& server_;
    // Host thread only: one entry goes out per service(), because DeZog's gdb remote reads one packet per TCP read
    std::deque<std::string> outbox_;
    Client* outbox_client_ = nullptr;
    Listener listener_;
    std::thread acceptor_;
    std::atomic<bool> stopping_{false};

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Message> queue_;
    std::unique_ptr<Client> client_;
    std::vector<std::unique_ptr<Client>> finished_;  // closed clients whose reader thread still needs a join
};

}  // namespace freetvc::remote
