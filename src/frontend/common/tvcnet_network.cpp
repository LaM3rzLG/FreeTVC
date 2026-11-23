// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/tvcnet_network.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "frontend/common/rom_fetch.hpp"
#include "remote/net.hpp"
#include "tvcnet/codes.hpp"

namespace fs = std::filesystem;
namespace st = tvcnet::status;

namespace freetvc {

struct TvcnetNetwork::Job {
    std::string url;          // GET: the URL; TCP: host:port
    bool tcp = false;
    mutable std::mutex mutex;
    bool finished = false;    // GET: the body is in; TCP: the peer closed
    bool connected = false;
    bool cancelled = false;
    std::uint8_t error = 0;
    std::uint16_t http = 0;
    std::vector<std::uint8_t> body;
    std::size_t pos = 0;
    std::string outgoing;     // TCP bytes sent while still connecting
    remote::Connection connection;
};

namespace {

std::atomic<unsigned> job_counter{0};

// curl's exit code and the HTTP status into a status code
std::uint8_t error_of(std::optional<int> exit, unsigned http) {
    if (!exit) {
        return st::kErrNoWifi;
    }
    switch (*exit) {
    case 0:
        return http >= 400 ? st::kErrHttp : 0;
    case 6:
        return st::kErrDns;
    case 7:
        return st::kErrConnect;
    case 1:
    case 3:
        return st::kErrParam;
    default:
        return st::kErrLost;
    }
}

void fetch(const std::shared_ptr<TvcnetNetwork::Job>& job, const fs::path& curl) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    const std::string stem = "freetvc-tvcnet-" + std::to_string(job_counter++);
    const fs::path body = dir / (stem + ".body");
    const fs::path log = dir / (stem + ".log");
    const auto exit = run_program(curl,
                                  {"-sS", "-L", "--proto", "=http,https", "--proto-redir", "=http,https", "--max-time", "60",
                                   "--max-filesize", std::to_string(TvcnetNetwork::kMaxBody), "-o", body.string(), "-w",
                                   "\n%{http_code}", job->url},
                                  log);
    unsigned http = 0;
    {
        std::ifstream in(log, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::size_t line = text.find_last_of('\n');
        const std::string last = line == std::string::npos ? text : text.substr(line + 1);
        if (last.size() == 3 && std::all_of(last.begin(), last.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            http = static_cast<unsigned>(std::stoul(last));
        }
    }
    std::vector<std::uint8_t> bytes;
    if (std::ifstream in{body, std::ios::binary}) {
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    fs::remove(body, ec);
    fs::remove(log, ec);
    std::lock_guard lock(job->mutex);
    job->error = error_of(exit, http);
    job->http = static_cast<std::uint16_t>(http);
    job->body = std::move(bytes);
    job->finished = true;
}

void stream(const std::shared_ptr<TvcnetNetwork::Job>& job, const std::string& host, std::uint16_t port) {
    std::string error;
    bool dns_failed = false;
    remote::Connection connection = remote::Connection::connect_host(host, port, error, dns_failed);
    {
        std::lock_guard lock(job->mutex);
        if (job->cancelled) {
            return;
        }
        if (!connection.open()) {
            job->error = dns_failed ? st::kErrDns : st::kErrConnect;
            return;
        }
        job->connection = std::move(connection);
        job->connected = true;
        if (!job->outgoing.empty() && !job->connection.send_all(job->outgoing)) {
            job->error = st::kErrLost;
            return;
        }
        job->outgoing.clear();
    }
    char buffer[4096];
    for (;;) {
        for (;;) {
            std::unique_lock lock(job->mutex);
            if (job->cancelled) {
                return;
            }
            if (job->body.size() - job->pos < TvcnetNetwork::kTcpBuffer) {
                break;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const long n = job->connection.receive(buffer, sizeof buffer);
        std::lock_guard lock(job->mutex);
        if (job->cancelled) {
            return;
        }
        if (n <= 0) {
            job->finished = true;
            return;
        }
        if (job->pos > TvcnetNetwork::kTcpBuffer) {
            job->body.erase(job->body.begin(), job->body.begin() + static_cast<std::ptrdiff_t>(job->pos));
            job->pos = 0;
        }
        job->body.insert(job->body.end(), buffer, buffer + n);
    }
}

}  // namespace

void TvcnetNetwork::close() {
    if (job_ && job_->tcp) {
        std::lock_guard lock(job_->mutex);
        job_->cancelled = true;
        job_->connection.shutdown();
    }
    job_.reset();
}

std::uint8_t TvcnetNetwork::tcp_open(std::string_view host, std::uint16_t port) {
    close();
    if (host.empty() || host.size() > 253) {
        return st::kErrParam;
    }
    if (!online_) {
        return st::kErrNoWifi;
    }
    auto job = std::make_shared<Job>();
    job->tcp = true;
    job->url = std::string(host) + ":" + std::to_string(port);
    job_ = job;
    std::thread([job, name = std::string(host), port] { stream(job, name, port); }).detach();
    return st::kOk;
}

std::uint8_t TvcnetNetwork::tcp_send(const std::uint8_t* data, std::size_t size) {
    if (!job_ || !job_->tcp) {
        return st::kNone;
    }
    std::lock_guard lock(job_->mutex);
    if (job_->error) {
        return job_->error;
    }
    if (job_->finished) {
        return st::kErrLost;
    }
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    if (job_->connected) {
        return job_->connection.send_all(bytes) ? st::kOk : st::kErrLost;
    }
    if (job_->outgoing.size() + size > kSendQueue) {
        return st::kBusy;
    }
    job_->outgoing.append(bytes);
    return st::kOk;
}

std::array<std::uint8_t, 4> TvcnetNetwork::ip() const {
    if (!online_) {
        return {};
    }
    return {127, 0, 0, 1};
}

bool TvcnetNetwork::wifi_join(std::string_view ssid, std::string_view) {
    if (ssid.empty() || ssid.size() > 32) {
        return false;
    }
    ssid_.assign(ssid);
    return true;
}

std::uint8_t TvcnetNetwork::http_get(std::string_view url) {
    close();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return st::kErrParam;
    }
    if (!online_) {
        return st::kErrNoWifi;
    }
    static const CurlCheck curl = find_curl();
    if (!curl.ok()) {
        return st::kErrNoWifi;
    }
    auto job = std::make_shared<Job>();
    job->url.assign(url);
    job_ = job;
    std::thread([job, program = curl.program] { fetch(job, program); }).detach();
    return st::kOk;
}

std::uint8_t TvcnetNetwork::next(std::vector<std::uint8_t>& out, std::size_t max, std::uint16_t& detail) {
    out.clear();
    if (!job_) {
        return st::kNone;
    }
    std::shared_ptr<Job> job = job_;
    std::lock_guard lock(job->mutex);
    if (job->tcp && !job->error) {
        const std::size_t n = std::min(max, job->body.size() - job->pos);
        out.assign(job->body.begin() + static_cast<std::ptrdiff_t>(job->pos),
                   job->body.begin() + static_cast<std::ptrdiff_t>(job->pos + n));
        job->pos += n;
        if (job->pos == job->body.size() && job->finished) {
            job_.reset();
            return st::kEnd;
        }
        return n ? st::kData : st::kBusy;
    }
    if (!job->finished && !job->error) {
        return st::kBusy;
    }
    if (job->error) {
        detail = job->http;
        const std::uint8_t error = job->error;
        job_.reset();
        return error;
    }
    const std::size_t n = std::min(max, job->body.size() - job->pos);
    out.assign(job->body.begin() + static_cast<std::ptrdiff_t>(job->pos),
               job->body.begin() + static_cast<std::ptrdiff_t>(job->pos + n));
    job->pos += n;
    if (job->pos == job->body.size()) {
        job_.reset();
        return st::kEnd;
    }
    return st::kData;
}

std::uint8_t TvcnetNetwork::job_state() const {
    if (!job_) {
        return st::kNone;
    }
    std::lock_guard lock(job_->mutex);
    if (job_->error) {
        return job_->error;
    }
    if (job_->pos < job_->body.size()) {
        return st::kData;
    }
    if (job_->finished) {
        return st::kEnd;
    }
    return job_->tcp && job_->connected ? st::kOk : st::kBusy;
}

std::int64_t TvcnetNetwork::content_length() const {
    if (!job_) {
        return -1;
    }
    std::lock_guard lock(job_->mutex);
    return !job_->tcp && job_->finished && !job_->error ? static_cast<std::int64_t>(job_->body.size()) : -1;
}

std::string TvcnetNetwork::describe() const {
    std::string text = online_ ? "network on" : "network off";
    if (!ssid_.empty()) {
        text += " (WiFi name " + ssid_ + ")";
    }
    if (job_) {
        std::lock_guard lock(job_->mutex);
        if (job_->tcp) {
            text += job_->error         ? ", TCP failed: "
                    : job_->finished    ? ", TCP closed by the peer: "
                    : job_->connected   ? ", TCP connected: "
                                        : ", TCP connecting: ";
            text += job_->url + " (" + std::to_string(job_->body.size() - job_->pos) + " bytes waiting)";
            return text;
        }
        text += job_->finished ? (job_->error ? ", job failed: " : ", job done: ") : ", job running: ";
        text += job_->url;
        if (job_->finished && !job_->error) {
            text += " (" + std::to_string(job_->pos) + " of " + std::to_string(job_->body.size()) + " bytes read)";
        }
    }
    return text;
}

}  // namespace freetvc
