// SPDX-License-Identifier: GPL-3.0-or-later
// Snapshot fields of the machine and its parts (design/media.md "Snapshots"). Every chunk starts at version 1.
#include "core/snapshot/machine_state.hpp"

#include <optional>

#include "core/machine/machine.hpp"
#include "core/message.hpp"

namespace freetvc {

namespace {

constexpr std::size_t kMaxCart = std::size_t{1} << 20;
constexpr std::size_t kMaxCardRom = std::size_t{1} << 16;

void write_frame(ChunkWriter& w, const Framebuffer& f) { w.packed(f.pixels); }

void read_frame(ChunkReader& r, Framebuffer& f) { r.packed(f.pixels); }

}  // namespace

std::optional<SnapshotNeeds> snapshot_needs(const Snapshot& snapshot, std::string& error) {
    const SnapshotChunk* conf = snapshot.find("CONF");
    if (!conf) {
        error = msg("the snapshot has no CONF chunk");
        return std::nullopt;
    }
    ChunkReader r(conf->body.data);
    SnapshotNeeds n;
    const std::uint8_t model = r.u8();
    n.colour = r.boolean();
    n.audio_rate = r.u32();
    n.sys_crc = r.u32();
    n.ext_crc = r.u32();
    const std::uint8_t type = r.u8();
    n.cart_bank = r.u8();
    n.cart = r.packed_vector(kMaxCart);
    if (r.failed() || model > static_cast<std::uint8_t>(Model::Tvc64kPlus) || type > static_cast<std::uint8_t>(CartType::SdDual)) {
        error = msg("the snapshot's CONF chunk is damaged");
        return std::nullopt;
    }
    n.model = static_cast<Model>(model);
    n.cart_type = static_cast<CartType>(type);
    // SLOT: per slot the card kind and its ROM; snapshots without it have empty slots
    if (const SnapshotChunk* slot = snapshot.find("SLOT")) {
        ChunkReader s(slot->body.data);
        for (CardConfig& card : n.cards) {
            const std::uint8_t kind = s.u8();
            if (kind > static_cast<std::uint8_t>(CardKind::TwoDfx)) {
                error = msg("the snapshot has a card this version does not know");
                return std::nullopt;
            }
            card.kind = static_cast<CardKind>(kind);
            card.rom = s.packed_vector(kMaxCardRom);
        }
        if (s.failed()) {
            error = msg("the snapshot's SLOT chunk is damaged");
            return std::nullopt;
        }
    }
    // SIDC: the SID card's model and clock, then its state
    if (const SnapshotChunk* sid = snapshot.find("SIDC")) {
        ChunkReader s(sid->body.data);
        const std::uint8_t chip = s.u8();
        n.sid.clock_hz = s.u32();
        if (s.failed() || chip == 0 || chip > static_cast<std::uint8_t>(SidModel::Mos8580) || n.sid.clock_hz == 0) {
            error = msg("the snapshot's SIDC chunk is damaged");
            return std::nullopt;
        }
        n.sid.model = static_cast<SidModel>(chip);
    }
    return n;
}

bool snapshot_matches(const MachineConfig& config, const SnapshotNeeds& needs) {
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (config.cards[i].kind != needs.cards[i].kind || config.cards[i].rom != needs.cards[i].rom) {
            return false;
        }
    }
    return config.model == needs.model && config.colour == needs.colour && config.sid == needs.sid && crc32(config.roms.sys) == needs.sys_crc &&
           crc32(config.roms.ext) == needs.ext_crc && config.roms.cart == needs.cart && config.roms.cart_type == needs.cart_type &&
           config.roms.cart_bank == needs.cart_bank;
}

MachineConfig apply_needs(MachineConfig config, const SnapshotNeeds& needs) {
    config.model = needs.model;
    config.colour = needs.colour;
    config.roms.cart = needs.cart;
    config.roms.cart_type = needs.cart_type;
    config.roms.cart_bank = needs.cart_bank;
    config.cards = needs.cards;
    config.sid = needs.sid;
    return config;
}

void Mc6845::save(ChunkWriter& w) const {
    w.bytes(regs_);
    w.u8(address_);
    w.u32(h_);
    w.u32(row_);
    w.u32(ra_);
    w.boolean(in_adjust_);
    w.u32(adjust_lines_);
    w.u16(ma_);
    w.u16(row_start_);
    w.u16(next_row_start_);
    w.boolean(hde_);
    w.boolean(vde_);
    w.u32(hsync_left_);
    w.u32(vsync_left_);
    w.u32(field_);
}

void Mc6845::load(ChunkReader& r) {
    r.bytes(regs_);
    address_ = r.u8();
    h_ = r.u32();
    row_ = r.u32();
    ra_ = r.u32();
    in_adjust_ = r.boolean();
    adjust_lines_ = r.u32();
    ma_ = r.u16();
    row_start_ = r.u16();
    next_row_start_ = r.u16();
    hde_ = r.boolean();
    vde_ = r.boolean();
    hsync_left_ = r.u32();
    vsync_left_ = r.u32();
    field_ = r.u32();
}

void Video::save(ChunkWriter& w) const {
    crtc_.save(w);
    w.u8(border_);
    w.bytes(palette_);
    w.u8(mode_);
    w.u64(next_slot_);
    w.boolean(vblank_);
    w.boolean(last_hsync_);
    w.boolean(last_vsync_);
    w.boolean(last_ma9_);
    w.boolean(frame_pending_);
    w.u32(beam_x_);
    w.u32(beam_y_);
    w.u64(frame_count_);
    write_frame(w, work_);
    write_frame(w, front_);
}

void Video::load(ChunkReader& r) {
    crtc_.load(r);
    border_ = r.u8();
    r.bytes(palette_);
    mode_ = r.u8();
    next_slot_ = r.u64();
    vblank_ = r.boolean();
    last_hsync_ = r.boolean();
    last_vsync_ = r.boolean();
    last_ma9_ = r.boolean();
    frame_pending_ = r.boolean();
    beam_x_ = r.u32();
    beam_y_ = r.u32();
    frame_count_ = r.u64();
    read_frame(r, work_);
    read_frame(r, front_);
}

void Sound::save(ChunkWriter& w) const {
    w.u16(pitch_);
    w.u32(chain_);
    w.u64(next_reload_);
    w.boolean(tone_enabled_);
    w.boolean(irq_enabled_);
    w.u32(amplitude_);
}

void Sound::load(ChunkReader& r) {
    pitch_ = r.u16();
    chain_ = r.u32();
    next_reload_ = r.u64();
    tone_enabled_ = r.boolean();
    irq_enabled_ = r.boolean();
    amplitude_ = r.u32();
}

void AudioSynth::save(ChunkWriter& w) const {
    w.u32(rate_);
    w.u64(base_);
    w.f32(level_);
    w.f64(integral_);
    w.f64(last_input_);
    w.f64(last_output_);
    w.u32(static_cast<std::uint32_t>(deltas_.size()));
    for (const float d : deltas_) {
        w.f32(d);
    }
}

void AudioSynth::load(ChunkReader& r, std::uint64_t tick) {
    const std::uint32_t rate = r.u32();
    const std::uint64_t base = r.u64();
    const float level = r.f32();
    const double integral = r.f64();
    const double last_input = r.f64();
    const double last_output = r.f64();
    const std::uint32_t count = r.u32();
    std::vector<float> deltas;
    for (std::uint32_t i = 0; i < count && i < 4 * kTaps && !r.failed(); ++i) {
        deltas.push_back(r.f32());
    }
    discard_ready();
    if (rate != rate_ || count > 4 * kTaps) {
        // Another output rate: the level carries over, the filter history does not
        clear();
        base_ = sample_position(tick) / kTickRate;
        level_ = level;
        integral_ = level;
        return;
    }
    base_ = base;
    level_ = level;
    integral_ = integral;
    last_input_ = last_input;
    last_output_ = last_output;
    deltas_ = std::move(deltas);
}

void MemoryMap::save(ChunkWriter& w) const {
    w.u8(page_register_);
    w.u8(video_paging_);
    w.u8(static_cast<std::uint8_t>(iomem_slot_));
    w.u8(static_cast<std::uint8_t>(cart_page_));
}

void MemoryMap::load(ChunkReader& r) {
    page_register_ = r.u8();
    video_paging_ = r.u8();
    iomem_slot_ = r.u8() & 3u;
    cart_page_ = r.u8() & 0x3Fu;
    remap();
}

void Machine::save_state(Snapshot& out) const {
    ChunkWriter& conf = out.add("CONF");
    conf.u8(static_cast<std::uint8_t>(config_.model));
    conf.boolean(config_.colour);
    conf.u32(config_.audio_rate);
    conf.u32(crc32(config_.roms.sys));
    conf.u32(crc32(config_.roms.ext));
    conf.u8(static_cast<std::uint8_t>(config_.roms.cart_type));
    conf.u8(static_cast<std::uint8_t>(config_.roms.cart_bank));
    conf.packed(config_.roms.cart);

    ChunkWriter& slot = out.add("SLOT");
    for (const CardConfig& card : config_.cards) {
        slot.u8(static_cast<std::uint8_t>(card.kind));
        slot.packed(card.rom);
    }
    ChunkWriter& cards = out.add("CARD");
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (const Card* card = memory_.cards().card(i)) {
            card->save(cards);
        }
    }

    ChunkWriter& cpu = out.add("CPU ");
    const Z80Registers regs = cpu_.registers();
    for (const std::uint16_t v : {regs.af, regs.bc, regs.de, regs.hl, regs.ix, regs.iy, regs.sp, regs.wz, regs.af2, regs.bc2,
                                  regs.de2, regs.hl2}) {
        cpu.u16(v);
    }
    cpu.u8(regs.i);
    cpu.u8(regs.r);
    cpu.u8(regs.im);
    cpu.boolean(regs.iff1);
    cpu.boolean(regs.iff2);
    cpu.u8(regs.q);
    const Z80Internal in = cpu_.internal();
    cpu.u16(in.pc);
    cpu.u16(in.step);
    cpu.u16(in.addr);
    cpu.u8(in.dlatch);
    cpu.u8(in.opcode);
    cpu.u8(in.hlx_idx);
    cpu.boolean(in.prefix_active);
    cpu.boolean(in.qw);
    cpu.u64(in.pins);
    cpu.u64(in.int_bits);
    cpu.u64(pins_);
    cpu.u64(ticks_);
    cpu.u16(last_fetch_);
    cpu.u16(instruction_pc_);
    cpu.boolean(skip_boundary_);
    cpu.boolean(jumped_);
    cpu.boolean(at_boundary_);
    cpu.boolean(interrupt_acked_);
    cpu.boolean(entered_interrupt_);

    ChunkWriter& bus = out.add("BUS ");
    memory_.save(bus);
    bus.boolean(irq_flipflop_);
    bus.u8(keyboard_row_);
    bus.bytes(keyboard_);
    bus.u8(port05_);
    bus.u8(printer_data_);
    bus.boolean(tape_out_);
    bus.u8(slot_irq_enable_);

    ChunkWriter& lpt = out.add("LPT ");
    lpt.boolean(printer_strobe_);
    lpt.boolean(printer_ack_);
    lpt.u64(printer_ack_due_);

    if (const MultiCart* multicart = memory_.multicart()) {
        multicart->save(out.add("MCRT"));
    }
    if (const SdDual* sddual = memory_.sddual()) {
        sddual->save(out.add("SDDC"));
    }
    if (sid_) {
        ChunkWriter& sid = out.add("SIDC");
        sid.u8(static_cast<std::uint8_t>(config_.sid.model));
        sid.u32(config_.sid.clock_hz);
        sid_->save(sid);
    }
    out.add("RAM ").packed(memory_.ram());
    out.add("VRAM").packed(memory_.vram());
    video_.save(out.add("VID "));
    ChunkWriter& snd = out.add("SND ");
    sound_.save(snd);
    audio_.save(snd);
    snd.u64(sound_.txc_edges());  // after the audio: older snapshots end there
}

bool Machine::load_state(const Snapshot& in, std::string& error) {
    const auto needs = snapshot_needs(in, error);
    if (!needs) {
        return false;
    }
    if (!snapshot_matches(config_, *needs)) {
        error = msg("the snapshot was taken on another board, ROM set or cartridge");
        return false;
    }
    Machine next(*this);
    const auto chunk = [&](const char* tag) -> std::optional<ChunkReader> {
        const SnapshotChunk* c = in.find(tag);
        if (!c) {
            error = msg("the snapshot has no %s chunk", tag);
            return std::nullopt;
        }
        return ChunkReader(c->body.data);
    };
    auto cpu = chunk("CPU ");
    auto bus = chunk("BUS ");
    auto ram = chunk("RAM ");
    auto vram = chunk("VRAM");
    auto vid = chunk("VID ");
    auto snd = chunk("SND ");
    if (!cpu || !bus || !ram || !vram || !vid || !snd) {
        return false;
    }

    Z80Registers regs{};
    for (std::uint16_t* v : {&regs.af, &regs.bc, &regs.de, &regs.hl, &regs.ix, &regs.iy, &regs.sp, &regs.wz, &regs.af2, &regs.bc2,
                             &regs.de2, &regs.hl2}) {
        *v = cpu->u16();
    }
    regs.i = cpu->u8();
    regs.r = cpu->u8();
    regs.im = cpu->u8();
    regs.iff1 = cpu->boolean();
    regs.iff2 = cpu->boolean();
    regs.q = cpu->u8();
    Z80Internal internal{};
    internal.pc = cpu->u16();
    internal.step = cpu->u16();
    internal.addr = cpu->u16();
    internal.dlatch = cpu->u8();
    internal.opcode = cpu->u8();
    internal.hlx_idx = cpu->u8();
    internal.prefix_active = cpu->boolean();
    internal.qw = cpu->boolean();
    internal.pins = cpu->u64();
    internal.int_bits = cpu->u64();
    next.cpu_.set_registers(regs);
    next.cpu_.set_internal(internal);
    next.pins_ = cpu->u64();
    next.ticks_ = cpu->u64();
    next.last_fetch_ = cpu->u16();
    next.instruction_pc_ = cpu->u16();
    next.skip_boundary_ = cpu->boolean();
    next.jumped_ = cpu->boolean();
    next.at_boundary_ = cpu->boolean();
    next.interrupt_acked_ = cpu->boolean();
    next.entered_interrupt_ = cpu->boolean();
    next.stop_pending_ = false;

    next.memory_.load(*bus);
    next.irq_flipflop_ = bus->boolean();
    next.keyboard_row_ = bus->u8();
    bus->bytes(next.keyboard_);
    next.port05_ = bus->u8();
    next.printer_data_ = bus->u8();
    next.tape_out_ = bus->boolean();
    next.slot_irq_enable_ = bus->u8();
    // LPT: the printer port's flip-flops; older snapshots had a printer always ready
    next.printer_strobe_ = true;
    next.printer_ack_ = true;
    next.printer_ack_due_ = Sound::kNever;
    if (const SnapshotChunk* c = in.find("LPT ")) {
        ChunkReader lpt(c->body.data);
        next.printer_strobe_ = lpt.boolean();
        next.printer_ack_ = lpt.boolean();
        next.printer_ack_due_ = lpt.u64();
        if (lpt.failed()) {
            error = msg("the snapshot's LPT chunk is damaged");
            return false;
        }
    }

    // CARD: the cards' state in slot order; older snapshots have no cards to fill
    std::optional<ChunkReader> cards;
    if (const SnapshotChunk* c = in.find("CARD")) {
        cards.emplace(c->body.data);
        for (unsigned i = 0; i < kCardSlots; ++i) {
            if (Card* card = next.memory_.cards().card(i)) {
                card->load(*cards);
            }
        }
        if (cards->failed()) {
            error = msg("the snapshot's CARD chunk is damaged");
            return false;
        }
        next.memory_.cards().refresh_events();
    }
    // MCRT: the MultiCart's register, latch, RAM and chip states; its flash is media
    if (MultiCart* multicart = next.memory_.multicart()) {
        auto mcrt = chunk("MCRT");
        if (!mcrt) {
            return false;
        }
        multicart->load(*mcrt);
        if (mcrt->failed()) {
            error = msg("the snapshot's MCRT chunk is damaged");
            return false;
        }
    }
    // SDDC: the SD Dual's flash, RAM, registers and cards; the cards' images are media
    if (SdDual* sddual = next.memory_.sddual()) {
        auto sddc = chunk("SDDC");
        if (!sddc) {
            return false;
        }
        sddual->load(*sddc);
        if (sddc->failed()) {
            error = msg("the snapshot's SDDC chunk is damaged");
            return false;
        }
    }

    // SIDC: the configuration snapshot_needs() checked, then the chip
    if (next.sid_) {
        auto sidc = chunk("SIDC");
        if (!sidc) {
            return false;
        }
        sidc->u8();
        sidc->u32();
        next.sid_->load(*sidc);
        if (sidc->failed()) {
            error = msg("the snapshot's SIDC chunk is damaged");
            return false;
        }
    }
    ram->packed(next.memory_.ram());
    vram->packed(next.memory_.vram());
    next.video_.load(*vid);
    next.sound_.load(*snd);
    next.audio_.load(*snd, next.ticks_);
    next.sound_.set_txc_edges(snd->at_end() ? 0 : snd->u64());

    for (const auto* r : {&*cpu, &*bus, &*ram, &*vram, &*vid, &*snd}) {
        if (r->failed()) {
            error = msg("the snapshot is damaged (a chunk is too short)");
            return false;
        }
    }
    restore(next);
    return true;
}

}  // namespace freetvc
