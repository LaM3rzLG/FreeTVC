// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/dzrp_host.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace freetvc::remote {

namespace {

constexpr std::size_t kMaxPayload = 1 << 20;  // WRITE_BANK_MEM sends at most a 64 K bank

}  // namespace

DzrpHost::DzrpHost(DzrpServer& server) : server_(server) {}

DzrpHost::~DzrpHost() {
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

bool DzrpHost::listen(std::uint16_t port, std::string& error) {
    if (!listener_.listen(port, error)) {
        return false;
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void DzrpHost::accept_loop() {
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
            continue;  // DeZog debugs one program at a time; a second client is closed at once
        }
        if (client_) {
            finished_.push_back(std::move(client_));
        }
        client_ = std::make_unique<Client>();
        client_->connection = std::move(c);
        Client* raw = client_.get();
        client_->reader = std::thread([this, raw] { read_loop(raw); });
    }
}

void DzrpHost::read_loop(Client* client) {
    std::vector<std::uint8_t> pending;
    char buffer[4096];
    bool bad = false;
    while (!bad) {
        const long n = client->connection.receive(buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        pending.insert(pending.end(), buffer, buffer + n);
        // Command: length of the payload, sequence number, command id, payload
        while (pending.size() >= 6) {
            const std::size_t length = pending[0] | (pending[1] << 8) | (pending[2] << 16) |
                                       (static_cast<std::size_t>(pending[3]) << 24);
            if (length > kMaxPayload) {
                bad = true;
                break;
            }
            if (pending.size() < 6 + length) {
                break;
            }
            Message m{client, std::vector<std::uint8_t>(pending.begin() + 4, pending.begin() + 6 + length)};
            pending.erase(pending.begin(), pending.begin() + 6 + static_cast<std::ptrdiff_t>(length));
            {
                std::lock_guard lock(mutex_);
                queue_.push_back(std::move(m));
            }
            wake_.notify_all();
        }
    }
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({client, {}});  // before `closed`, so a new client's commands come after it
    }
    client->closed = true;
    wake_.notify_all();
}

void DzrpHost::service() {
    std::deque<Message> messages;
    {
        std::lock_guard lock(mutex_);
        messages.swap(queue_);
    }
    for (const Message& m : messages) {
        if (m.bytes.empty()) {
            server_.end_session();
            continue;
        }
        const std::string reply = server_.handle(std::span<const std::uint8_t>(m.bytes));
        if (!m.client->closed) {
            m.client->connection.send_all(reply);
        }
    }
    std::lock_guard lock(mutex_);
    const std::string notification = server_.poll();
    if (!notification.empty() && client_ && !client_->closed) {
        client_->connection.send_all(notification);
    }
    for (auto it = finished_.begin(); it != finished_.end();) {
        const Client* c = it->get();
        const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Message& m) { return m.client == c; });
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

void DzrpHost::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, timeout, [&] { return !queue_.empty(); });
}

}  // namespace freetvc::remote
