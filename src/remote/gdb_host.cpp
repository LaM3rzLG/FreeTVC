// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/gdb_host.hpp"

#include <algorithm>
#include <utility>

namespace freetvc::remote {

GdbHost::GdbHost(GdbServer& server) : server_(server) {}

GdbHost::~GdbHost() {
    stopping_ = true;
    listener_.close();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    std::vector<std::unique_ptr<Client>> clients;
    {
        std::lock_guard lock(mutex_);  // a reader takes it on its way out, so join outside
        clients = std::move(finished_);
        if (client_) {
            clients.push_back(std::move(client_));
        }
    }
    for (auto& c : clients) {
        c->connection.shutdown();
        if (c->reader.joinable()) {
            c->reader.join();
        }
    }
}

bool GdbHost::listen(std::uint16_t port, std::string& error) {
    if (!listener_.listen(port, error)) {
        return false;
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void GdbHost::accept_loop() {
    while (!stopping_) {
        Connection c = listener_.accept();
        if (!c.open()) {
            if (stopping_) {
                break;
            }
            continue;
        }
        std::lock_guard lock(mutex_);
        if (client_ && !client_->closed) {
            continue;  // one debugger at a time; a second client is closed at once
        }
        if (client_) {
            finished_.push_back(std::move(client_));
        }
        client_ = std::make_unique<Client>();
        client_->connection = std::move(c);
        Client* raw = client_.get();
        queue_.push_back({raw, Event::Connected, {}});
        client_->reader = std::thread([this, raw] { read_loop(raw); });
        wake_.notify_all();
    }
}

void GdbHost::read_loop(Client* client) {
    char buffer[4096];
    for (;;) {
        const long n = client->connection.receive(buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        {
            std::lock_guard lock(mutex_);
            queue_.push_back({client, Event::Bytes, std::string(buffer, static_cast<std::size_t>(n))});
        }
        wake_.notify_all();
    }
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({client, Event::Lost, {}});  // before `closed`, so a new client's bytes come after it
    }
    client->closed = true;
    wake_.notify_all();
}

void GdbHost::service() {
    std::deque<Message> messages;
    {
        std::lock_guard lock(mutex_);
        messages.swap(queue_);
    }
    for (const Message& m : messages) {
        if (m.event == Event::Connected) {
            server_.begin_session();
            outbox_.clear();
            outbox_client_ = m.client;
        } else if (m.event == Event::Lost) {
            server_.end_session();
            outbox_.clear();
            outbox_client_ = nullptr;
        } else {
            queue_packets(server_.feed(m.bytes));
        }
    }
    queue_packets(server_.poll());
    std::lock_guard lock(mutex_);
    if (!outbox_.empty()) {
        if (outbox_client_ && !outbox_client_->closed) {
            outbox_client_->connection.send_all(outbox_.front());
        }
        outbox_.pop_front();
    }
    for (auto it = finished_.begin(); it != finished_.end();) {
        const Client* c = it->get();
        const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Message& q) { return q.client == c; });
        if (c->closed && !queued) {
            if ((*it)->reader.joinable()) {
                (*it)->reader.join();
            }
            it = finished_.erase(it);
        } else {
            ++it;
        }
    }
}

void GdbHost::queue_packets(const std::string& out) {
    std::string unit;
    for (std::size_t i = 0; i < out.size(); ++i) {
        unit.push_back(out[i]);
        if (out[i] == '#' && i + 2 < out.size()) {
            unit.append(out, i + 1, 2);  // the checksum ends the packet
            i += 2;
            outbox_.push_back(std::move(unit));
            unit.clear();
        }
    }
    if (!unit.empty()) {
        outbox_.push_back(std::move(unit));  // a lone ack, as the reply to a continue
    }
}

void GdbHost::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, timeout, [&] { return !queue_.empty() || !outbox_.empty(); });
}

}  // namespace freetvc::remote
