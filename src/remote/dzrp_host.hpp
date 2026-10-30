// SPDX-License-Identifier: GPL-3.0-or-later
// DZRP over TCP: one client at a time on 127.0.0.1. The reader thread only cuts the stream into messages; the host
// thread answers them in service().
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

#include "remote/dzrp.hpp"
#include "remote/net.hpp"

namespace freetvc::remote {

class DzrpHost {
public:
    explicit DzrpHost(DzrpServer& server);
    ~DzrpHost();
    DzrpHost(const DzrpHost&) = delete;
    DzrpHost& operator=(const DzrpHost&) = delete;

    // 127.0.0.1:port (0 = any free port, see port()).
    bool listen(std::uint16_t port, std::string& error);
    std::uint16_t port() const { return listener_.port(); }

    // Host thread: answers every queued command, then sends NTF_PAUSE if a continued program has stopped.
    void service();
    void wait(std::chrono::milliseconds timeout);

private:
    struct Client {
        Connection connection;
        std::thread reader;
        std::atomic<bool> closed{false};
    };
    struct Message {
        Client* client = nullptr;
        std::vector<std::uint8_t> bytes;  // empty: the client is gone
    };

    void accept_loop();
    void read_loop(Client* client);

    DzrpServer& server_;
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
