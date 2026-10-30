// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/monitor_hub.hpp"

#include <iostream>
#include <utility>

namespace freetvc::remote {

MonitorHub::MonitorHub(Monitor& monitor) : monitor_(monitor) {}

MonitorHub::~MonitorHub() {
    stopping_ = true;
    listener_.close();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    std::vector<std::unique_ptr<Client>> clients;
    {
        std::lock_guard lock(mutex_);
        clients = std::move(clients_);
    }
    for (auto& c : clients) {
        c->connection.shutdown();
        if (c->reader.joinable()) {
            c->reader.join();
        }
    }
}

bool MonitorHub::listen(std::uint16_t port, std::string& error) {
    if (!listener_.listen(port, error)) {
        return false;
    }
    listening_ = true;
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void MonitorHub::stop_listening() {
    if (!listening_) {
        return;
    }
    accept_stop_ = true;
    listener_.close();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    accept_stop_ = false;
    listening_ = false;
    std::lock_guard lock(mutex_);
    for (auto& c : clients_) {
        if (!c->console && !c->local) {
            c->connection.shutdown();  // its reader ends; service() joins it
        }
    }
}

void MonitorHub::attach_console() {
    std::lock_guard lock(mutex_);
    auto client = std::make_unique<Client>();
    client->id = next_id_++;
    client->console = true;
    console_id_ = client->id;
    clients_.push_back(std::move(client));
    console_open_ = true;
    // std::getline cannot be interrupted, so this thread is never joined; it ends with the process
    std::thread([this] { console_loop(); }).detach();
}

int MonitorHub::attach_local() {
    std::lock_guard lock(mutex_);
    auto client = std::make_unique<Client>();
    client->id = next_id_++;
    client->local = true;
    const int id = client->id;
    clients_.push_back(std::move(client));
    return id;
}

std::string MonitorHub::take_local_output() {
    std::lock_guard lock(mutex_);
    return std::exchange(local_output_, std::string());
}

void MonitorHub::start_in_monitor(std::string_view why) {
    broadcast(with_prompt(monitor_.enter(why)));
}

void MonitorHub::add_script(std::vector<std::string> lines) {
    script_ = std::move(lines);
    script_pos_ = 0;
    script_given_ = true;
    if (!monitor_.active()) {
        log(monitor_.enter("script"));
    }
}

bool MonitorHub::interactive() const { return listening_ || console_open_; }

void MonitorHub::accept_loop() {
    while (!stopping_ && !accept_stop_) {
        Connection c = listener_.accept();
        if (!c.open()) {
            if (stopping_ || accept_stop_) {
                break;
            }
            continue;
        }
        std::lock_guard lock(mutex_);
        auto client = std::make_unique<Client>();
        client->id = next_id_++;
        client->connection = std::move(c);
        Client* raw = client.get();
        client->reader = std::thread([this, raw] { read_loop(raw); });
        clients_.push_back(std::move(client));
    }
}

void MonitorHub::read_loop(Client* client) {
    std::string pending;
    char buffer[512];
    for (;;) {
        const long n = client->connection.receive(buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        pending.append(buffer, static_cast<std::size_t>(n));
        std::size_t eol;
        while ((eol = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            push(client->id, std::move(line));
        }
        if (pending.size() > 4096) {
            pending.clear();  // no line is that long
        }
    }
    client->closed = true;
    wake_.notify_all();
}

void MonitorHub::console_loop() {
    std::string line;
    const int id = console_id_;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        push(id, line);
    }
    console_open_ = false;
    wake_.notify_all();
}

void MonitorHub::push(int client, std::string text) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({client, std::move(text)});
    }
    wake_.notify_all();
}

void MonitorHub::send(int client, const std::string& text) {
    if (text.empty()) {
        return;
    }
    std::lock_guard lock(mutex_);
    for (auto& c : clients_) {
        if (c->id != client || c->closed) {
            continue;
        }
        if (c->local) {
            local_output_ += text;
        } else if (c->console) {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
        } else {
            c->connection.send_all(text);
        }
        return;
    }
}

void MonitorHub::broadcast(const std::string& text) {
    if (text.empty()) {
        return;
    }
    bool sent = false;
    {
        std::lock_guard lock(mutex_);
        for (auto& c : clients_) {
            if (c->closed) {
                continue;
            }
            if (c->local) {
                local_output_ += text;
            } else if (c->console) {
                std::fwrite(text.data(), 1, text.size(), stdout);
                std::fflush(stdout);
            } else {
                c->connection.send_all(text);
            }
            sent = true;
        }
    }
    if (!sent) {
        log(text);
    }
}

void MonitorHub::log(const std::string& text) {
    if (log_ && !text.empty()) {
        std::fwrite(text.data(), 1, text.size(), log_);
        std::fflush(log_);
    }
}

std::string MonitorHub::with_prompt(const std::string& text) const {
    return monitor_.active() ? text + monitor_.prompt() : text;
}

void MonitorHub::service() {
    // Script lines wait for the monitor; a run command in the script lets the machine go until the next stop
    while (script_pos_ < script_.size() && monitor_.active() && !monitor_.quit_code()) {
        const std::string& line = script_[script_pos_++];
        std::string out = monitor_.prompt() + line + "\n";
        out += monitor_.execute(line);
        if (!monitor_.active()) {
            script_ran_machine_ = true;
        }
        log(out);
    }
    if (script_given_ && script_done() && !script_finished_) {
        script_finished_ = true;
        if (monitor_.active() && !script_ran_machine_ && !monitor_.quit_code()) {
            monitor_.execute("x");
        }
    }

    std::deque<Line> lines;
    {
        std::lock_guard lock(mutex_);
        lines.swap(queue_);
        // Readers that ended have nothing left to say
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->closed && !(*it)->console) {
                if ((*it)->reader.joinable()) {
                    (*it)->reader.join();
                }
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Line& line : lines) {
        if (monitor_.quit_code()) {
            break;
        }
        if (!monitor_.active()) {
            broadcast(monitor_.enter("stopped by the monitor"));
        }
        send(line.client, with_prompt(monitor_.execute(line.text)));
    }
}

std::uint64_t MonitorHub::run(std::uint64_t max_ticks) {
    if (monitor_.active()) {
        return 0;
    }
    std::string report;
    const std::uint64_t ran = monitor_.run(max_ticks, report);
    broadcast(monitor_.take_trace_lines());
    if (!report.empty()) {
        // A script still running prints its own prompt with the next line
        broadcast(script_done() ? with_prompt(report) : report);
    }
    return ran;
}

void MonitorHub::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, timeout, [&] { return !queue_.empty(); });
}

}  // namespace freetvc::remote
