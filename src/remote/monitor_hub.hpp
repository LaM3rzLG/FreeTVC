// SPDX-License-Identifier: GPL-3.0-or-later
// Where monitor commands come from and where output goes: TCP clients, the console, --moncommands scripts.
// Network and console threads only read lines into a queue; the host thread drains it between run slices.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "remote/monitor.hpp"
#include "remote/net.hpp"

namespace freetvc::remote {

inline constexpr std::uint16_t kMonitorPort = 6580;

class MonitorHub {
public:
    explicit MonitorHub(Monitor& monitor);
    ~MonitorHub();
    MonitorHub(const MonitorHub&) = delete;
    MonitorHub& operator=(const MonitorHub&) = delete;

    // Text monitor on 127.0.0.1:port (0 = any free port, see port()).
    bool listen(std::uint16_t port, std::string& error);
    std::uint16_t port() const { return listener_.port(); }
    // Closes the listener and its TCP clients (the console and local clients stay); listen() may follow.
    void stop_listening();
    bool listening() const { return listening_; }
    // Commands from stdin, output to stdout, with a prompt.
    void attach_console();
    // Enters the monitor now and tells every client (headless --monitor starts there).
    void start_in_monitor(std::string_view why);
    // Runs first, in the monitor. The machine then runs, unless a script line ran it or a stop is waiting.
    void add_script(std::vector<std::string> lines);
    // Output with no client to send it to (stop reports in the GUI without a console, script output).
    void set_log(std::FILE* log) { log_ = log; }
    // An in-process client (Bench's console): its lines are queued like a TCP client's, and what is sent to it collects
    // for take_local_output().
    int attach_local();
    void submit_local(int client, std::string line) { push(client, std::move(line)); }
    std::string take_local_output();

    // Host thread, between run slices: runs the script and every queued line. A line from a client while the
    // machine runs enters the monitor first (VICE).
    void service();
    // Host thread: runs the machine through the monitor; stop reports and trace hits go to every client.
    std::uint64_t run(std::uint64_t max_ticks);
    // Host thread, while the monitor holds the machine: waits for input.
    void wait(std::chrono::milliseconds timeout);

    std::optional<int> quit_code() const { return monitor_.quit_code(); }
    // A listener is open or the console has not reached end of file.
    bool interactive() const;
    bool script_done() const { return script_pos_ >= script_.size(); }

private:
    struct Client {
        int id = 0;
        Connection connection;  // closed for the console
        bool console = false;
        bool local = false;
        std::thread reader;
        std::atomic<bool> closed{false};
    };
    struct Line {
        int client = 0;
        std::string text;
    };

    void accept_loop();
    void read_loop(Client* client);
    void console_loop();
    void push(int client, std::string text);
    void send(int client, const std::string& text);
    void broadcast(const std::string& text);
    void log(const std::string& text);
    std::string with_prompt(const std::string& text) const;

    Monitor& monitor_;
    Listener listener_;
    std::thread acceptor_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> accept_stop_{false};
    std::atomic<bool> console_open_{false};
    bool listening_ = false;
    int console_id_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Line> queue_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::string local_output_;
    int next_id_ = 1;

    std::vector<std::string> script_;
    std::size_t script_pos_ = 0;
    bool script_given_ = false;
    bool script_finished_ = false;
    bool script_ran_machine_ = false;
    std::FILE* log_ = stdout;
};

}  // namespace freetvc::remote
