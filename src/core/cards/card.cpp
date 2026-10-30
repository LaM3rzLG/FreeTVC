// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/card.hpp"

#include <algorithm>

#include "core/cards/card256.hpp"
#include "core/cards/gamecard.hpp"
#include "core/cards/hbf.hpp"
#include "core/cards/multichip.hpp"
#include "core/cards/nanosd.hpp"
#include "core/cards/serial_card.hpp"

namespace freetvc {

const char* card_kind_name(CardKind kind) {
    switch (kind) {
    case CardKind::Multichip:
        return "multichip";
    case CardKind::NanoSd:
        return "nanosd";
    case CardKind::Serial:
        return "serial";
    case CardKind::Hbf:
        return "hbf";
    case CardKind::GameCard:
        return "gamecard";
    case CardKind::Card256:
        return "card256";
    case CardKind::None:
        break;
    }
    return "none";
}

CardSlots::CardSlots(const CardSlots& other) { *this = other; }

CardSlots& CardSlots::operator=(const CardSlots& other) {
    if (this != &other) {
        for (unsigned i = 0; i < kCardSlots; ++i) {
            cards_[i] = other.cards_[i] ? other.cards_[i]->clone() : nullptr;
        }
        next_event_ = other.next_event_;
    }
    return *this;
}

void CardSlots::configure(const std::array<CardConfig, kCardSlots>& configs) {
    for (unsigned i = 0; i < kCardSlots; ++i) {
        switch (configs[i].kind) {
        case CardKind::Hbf:
            cards_[i] = std::make_unique<HbfCard>(configs[i].rom);
            break;
        case CardKind::GameCard:
            cards_[i] = std::make_unique<GameCard>(configs[i].rom);
            break;
        case CardKind::Card256:
            cards_[i] = std::make_unique<Card256>(configs[i].rom);
            break;
        case CardKind::Multichip:
            cards_[i] = std::make_unique<MultichipCard>(configs[i].rom);
            break;
        case CardKind::NanoSd:
            cards_[i] = std::make_unique<NanoSd>(configs[i].rom);
            break;
        case CardKind::Serial:
            cards_[i] = std::make_unique<SerialCard>();
            break;
        case CardKind::None:
            cards_[i].reset();
            break;
        }
    }
    refresh_events();
}

std::uint8_t CardSlots::ids() const {
    unsigned v = 0xFF;
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (cards_[i]) {
            v = (v & ~(3u << (2 * i))) | ((cards_[i]->id() & 3u) << (2 * i));
        }
    }
    return static_cast<std::uint8_t>(v);
}

void CardSlots::power_on() {
    for (auto& c : cards_) {
        if (c) {
            c->power_on();
        }
    }
    refresh_events();
}

void CardSlots::reset(std::uint64_t tick) {
    for (auto& c : cards_) {
        if (c) {
            c->reset(tick);
        }
    }
    refresh_events();
}

std::uint8_t CardSlots::io_read(unsigned slot, unsigned port, const CardBus& bus) {
    Card* c = card(slot);
    if (!c) {
        return 0xFF;
    }
    const std::uint8_t value = c->io_read(port & 15, bus);
    refresh_events();
    return value;
}

void CardSlots::io_write(unsigned slot, unsigned port, std::uint8_t value, const CardBus& bus) {
    if (Card* c = card(slot)) {
        c->io_write(port & 15, value, bus);
        refresh_events();
    }
}

void CardSlots::run_events(const CardBus& bus) {
    for (auto& c : cards_) {
        if (c && c->next_event() <= bus.tick) {
            c->run_events(bus);
        }
    }
    refresh_events();
}

void CardSlots::refresh_events() {
    next_event_ = UINT64_MAX;
    for (const auto& c : cards_) {
        if (c) {
            next_event_ = std::min(next_event_, c->next_event());
        }
    }
}

std::uint8_t CardSlots::mem_read(unsigned slot, std::uint16_t offset) const {
    const Card* c = card(slot);
    return c ? c->mem_read(offset) : 0xFF;
}

void CardSlots::mem_write(unsigned slot, std::uint16_t offset, std::uint8_t value) {
    if (Card* c = card(slot)) {
        c->mem_write(offset, value);
    }
}

void CardSlots::advance_audio(std::uint64_t tick, AudioSynth& out) {
    for (auto& c : cards_) {
        if (c) {
            c->advance_audio(tick, out);
        }
    }
    refresh_events();
}

std::uint8_t* CardSlots::phantom_page(unsigned unit) {
    for (auto& c : cards_) {
        if (c) {
            if (std::uint8_t* page = c->phantom_page(unit)) {
                return page;
            }
        }
    }
    return nullptr;
}

void CardSlots::border_write(std::uint8_t value) {
    for (auto& c : cards_) {
        if (c) {
            c->border_write(value);
        }
    }
}

VideoOverlay* CardSlots::overlay() {
    for (auto& c : cards_) {
        if (c) {
            if (VideoOverlay* o = c->overlay()) {
                return o;
            }
        }
    }
    return nullptr;
}

std::uint8_t CardSlots::irq_lines() const {
    unsigned lines = 0;
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (cards_[i] && cards_[i]->irq()) {
            lines |= 1u << i;
        }
    }
    return static_cast<std::uint8_t>(lines);
}

void CardSlots::clear_interrupt(unsigned slot, std::uint8_t value) {
    if (Card* c = card(slot)) {
        c->clear_interrupt(value);
    }
}

}  // namespace freetvc
