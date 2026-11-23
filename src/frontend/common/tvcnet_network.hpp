// SPDX-License-Identifier: GPL-3.0-or-later
// TVCnet's WiFi on the host (design/media.md "TVCnet"): an HTTP(S) GET runs curl in a thread and the TVC reads the
// body once it is complete; a TCP connection is a host socket read by its own thread.
#pragma once

#include <memory>
#include <string>

#include "tvcnet/hal.hpp"

namespace freetvc {

class TvcnetNetwork final : public tvcnet::Network {
public:
    struct Job;  // shared with the curl thread
    static constexpr std::uint64_t kMaxBody = 16 * 1024 * 1024;
    static constexpr std::size_t kTcpBuffer = 64 * 1024;  // received, not yet read by the TVC
    static constexpr std::size_t kSendQueue = 4096;       // queued while connecting

    TvcnetNetwork() = default;
    TvcnetNetwork(const TvcnetNetwork&) = delete;
    TvcnetNetwork& operator=(const TvcnetNetwork&) = delete;
    ~TvcnetNetwork() override { close(); }

    // Off: no WiFi, every network command fails as TVCnet would without one
    void set_online(bool on) { online_ = on; }
    bool online() const { return online_; }
    // The last WIFI command's network name; the host's own connection is used either way
    const std::string& ssid() const { return ssid_; }

    bool wifi_connected() const override { return online_; }
    std::array<std::uint8_t, 4> ip() const override;
    bool wifi_join(std::string_view ssid, std::string_view password) override;
    std::uint8_t http_get(std::string_view url) override;
    std::uint8_t tcp_open(std::string_view host, std::uint16_t port) override;
    std::uint8_t tcp_send(const std::uint8_t* data, std::size_t size) override;
    std::uint8_t next(std::vector<std::uint8_t>& out, std::size_t max, std::uint16_t& detail) override;
    std::uint8_t job_state() const override;
    std::int64_t content_length() const override;
    void close() override;

    // One line: the network and the job
    std::string describe() const;

private:
    bool online_ = true;
    std::string ssid_;
    std::shared_ptr<Job> job_;
};

}  // namespace freetvc
