// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/serial_card.hpp"

#include <algorithm>

#include "core/audio/sound.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

void SerialCard::power_on() {
    usart_.reset(0);
    next_event_ = UINT64_MAX;
}

// The bus RESET line reaches the 8251A's RESET pin (tvcser_4)
void SerialCard::reset(std::uint64_t) {
    usart_.reset(0);
    next_event_ = UINT64_MAX;
}

void SerialCard::catch_up(const CardBus& bus) {
    if (bus.sound) {
        usart_.advance(bus.sound->txc_edges(), bus.serial);
    }
}

// A guess at the tick of the next finished character from the pitch now, or of the next look at the line while the
// receiver listens (once a bit); run_events() checks the clock count itself
void SerialCard::schedule(const CardBus& bus) {
    next_event_ = UINT64_MAX;
    if (!bus.sound || bus.sound->pitch() == Sound::kStopPitch) {
        return;
    }
    const std::uint64_t now = bus.sound->txc_edges();
    std::uint64_t due = usart_.next_event();
    if (usart_.listening(bus.serial)) {
        due = std::min(due, now + usart_.bit_clocks());
    }
    if (due == I8251::kNever) {
        return;
    }
    const std::uint64_t per_edge = 4 * static_cast<std::uint64_t>(0x1000 - bus.sound->pitch());  // two reloads, in ticks
    next_event_ = bus.tick + (due > now ? due - now : 1) * per_edge;
}

std::uint8_t SerialCard::io_read(unsigned port, const CardBus& bus) {
    catch_up(bus);
    const std::uint8_t value = (port & 1) ? usart_.read_status(bus.serial && bus.serial->connected()) : usart_.read_data();
    catch_up(bus);  // a read data register lets the next character start
    schedule(bus);
    return value;
}

void SerialCard::io_write(unsigned port, std::uint8_t value, const CardBus& bus) {
    catch_up(bus);
    const std::uint64_t edges = bus.sound ? bus.sound->txc_edges() : 0;
    if (port & 1) {
        usart_.write_control(value, edges, bus.serial);
    } else {
        usart_.write_data(value, edges, bus.serial);
    }
    schedule(bus);
}

void SerialCard::run_events(const CardBus& bus) {
    catch_up(bus);
    schedule(bus);
}

void SerialCard::save(ChunkWriter& w) const {
    usart_.save(w);
    w.u64(next_event_);
}

void SerialCard::load(ChunkReader& r) {
    usart_.load(r);
    next_event_ = r.u64();
}

}  // namespace freetvc
