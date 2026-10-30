// SPDX-License-Identifier: GPL-3.0-or-later
// The far end of the serial card's cable (design/cards.md "Serial card"): a peer that is the host itself
// (bytes queued by the user, a file sent in, a capture file), or a TCP peer on 127.0.0.1, listening or connecting.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/cards/i8251.hpp"

namespace freetvc {

class SerialHost final : public SerialLine {
public:
    static constexpr std::size_t kKept = 64 * 1024;
    static constexpr std::uint16_t kDefaultPort = 6585;

    SerialHost();
    ~SerialHost() override;
    SerialHost(const SerialHost&) = delete;
    SerialHost& operator=(const SerialHost&) = delete;

    // "off" unplugs the cable; "on" plugs in the host as the peer; "listen[:PORT]" waits for a TCP client on
    // 127.0.0.1 (the cable counts as plugged in while one is connected); "connect:PORT" connects to one. False with
    // `error` set, and the line unchanged.
    bool set_peer(const std::string& spec, std::string& error);
    std::string peer() const;
    // true: the peer sends only while the TVC's RTS is on (the factory cable); false: whenever the receiver is empty
    void set_rts_flow(bool on) { rts_flow_ = on; }
    bool rts_flow() const { return rts_flow_; }
    // Bytes for the TVC, sent after anything already queued (with a host peer; TCP peers send their own)
    void queue(std::string_view bytes);
    bool queue_file(const std::string& path, std::string& error);
    std::size_t queued() const;
    // Everything the TVC sends also goes to this file (appended); empty stops it
    bool set_capture(const std::string& path, std::string& error);
    // What the TVC sent since the last call
    std::string take();
    std::string describe() const;
    void flush();

    bool connected() const override;
    bool waits_for_rts() const override { return rts_flow_; }
    bool take(std::uint8_t& byte) override;
    void send(std::uint8_t byte) override;

private:
    struct Tcp;
    void stop_tcp();

    enum class Peer { Off, Host, Listen, Connect } peer_ = Peer::Off;
    std::uint16_t port_ = 0;
    bool rts_flow_ = true;
    mutable std::mutex mutex_;  // the TCP reader thread fills `incoming_`
    std::deque<std::uint8_t> incoming_;
    std::unique_ptr<Tcp> tcp_;
    std::ofstream capture_;
    std::filesystem::path capture_path_;
    std::string kept_;
    std::uint64_t sent_ = 0, received_ = 0;
};

}  // namespace freetvc
