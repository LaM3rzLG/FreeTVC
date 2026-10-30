// SPDX-License-Identifier: GPL-3.0-or-later
// Debug core checks on hand-assembled programs in all-RAM mode (port 02h = B0h), no ROMs needed.
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "debug/z80_decode.hpp"

namespace {

using freetvc::Bank;
using freetvc::Machine;
using freetvc::debug::Checkpoint;
using freetvc::debug::Debugger;
using freetvc::debug::Flow;
using freetvc::debug::Why;

constexpr std::uint64_t kBudget = 2 * 62800;  // one frame in half-T ticks

int g_failures = 0;

void check(bool condition, const char* what, unsigned long long got, unsigned long long expected, int line) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d %s: got %llu (0x%llX), expected %llu (0x%llX)\n", line, what, got, got, expected, expected);
    }
}

#define CHECK_EQ(got, expected)                                                                                      \
    do {                                                                                                             \
        const auto got_ = (got);                                                                                     \
        const auto expected_ = (expected);                                                                           \
        check(got_ == expected_, #got, static_cast<unsigned long long>(got_), static_cast<unsigned long long>(expected_), \
              __LINE__);                                                                                             \
    } while (false)

std::unique_ptr<Machine> ram_machine(std::uint16_t address, std::initializer_list<std::uint8_t> program) {
    freetvc::MachineConfig config;
    config.audio_rate = 0;
    auto m = std::make_unique<Machine>(config);
    m->io_write(0x02, 0xB0);  // U0 U1 U2 U3
    const std::vector<std::uint8_t> bytes(program);
    m->load(address, bytes);
    freetvc::Z80Registers r = m->cpu().registers();
    r.sp = 0xA000;
    m->cpu().set_registers(r);
    m->jump(address);
    return m;
}

Checkpoint watch(std::uint8_t access, std::uint16_t start, std::uint16_t end = 0) {
    Checkpoint c;
    c.access = access;
    c.start = start;
    c.end = end ? end : start;
    return c;
}

void exec_checkpoint() {
    // 8000 LD A,5 / INC A / INC A / JR $
    auto m = ram_machine(0x8000, {0x3E, 0x05, 0x3C, 0x3C, 0x18, 0xFE});
    Debugger dbg(*m);
    const unsigned id = dbg.add(watch(freetvc::kAccessExec, 0x8003));
    auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8003u);
    CHECK_EQ(dbg.registers().af >> 8, 6u);
    CHECK_EQ(stop.hits.size(), 1u);
    CHECK_EQ(stop.hits[0].checkpoint, id);
    CHECK_EQ(stop.hits[0].value, 0x3Cu);
    CHECK_EQ(stop.hits[0].where.bank, Bank::U2);
    CHECK_EQ(stop.hits[0].where.offset, 0x0003u);
    CHECK_EQ(m->at_boundary(), true);
    // Resuming does not stop on the same instruction again
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Budget);
    CHECK_EQ(dbg.find(id)->hits, 1u);
    // Neither does a run that starts there after a jump
    m->jump(0x8003);
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Budget);
    CHECK_EQ(dbg.find(id)->hits, 1u);
    // Bank qualifier
    Checkpoint other = *dbg.find(id);
    other.bank = Bank::U1;
    dbg.update(other);
    m->jump(0x8000);
    CHECK_EQ(dbg.run(kBudget).why, Why::Budget);
    other.bank = Bank::U2;
    dbg.update(other);
    m->jump(0x8000);
    CHECK_EQ(dbg.run(kBudget).why, Why::Checkpoint);
}

void memory_watchpoints() {
    // 8000 LD HL,9000h / LD (HL),AAh / NOP / LD A,(HL) / JR $
    auto m = ram_machine(0x8000, {0x21, 0x00, 0x90, 0x36, 0xAA, 0x00, 0x7E, 0x18, 0xFE});
    Debugger dbg(*m);
    dbg.add(watch(freetvc::kAccessWrite, 0x9000));
    auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8005u);  // after the writing instruction
    CHECK_EQ(stop.hits.size(), 1u);
    CHECK_EQ(stop.hits[0].pc, 0x8003u);
    CHECK_EQ(stop.hits[0].value, 0xAAu);
    CHECK_EQ(stop.hits[0].access, freetvc::kAccessWrite);
    CHECK_EQ(stop.hits[0].where.bank, Bank::U2);
    CHECK_EQ(stop.hits[0].where.offset, 0x1000u);
    CHECK_EQ(m->peek(0x9000), 0xAAu);

    // Reads: an operand byte counts, an opcode fetch does not
    dbg.clear_checkpoints();
    dbg.add(watch(freetvc::kAccessRead, 0x8005));  // the NOP, fetched only
    dbg.add(watch(freetvc::kAccessRead, 0x9000));
    m->jump(0x8000);
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8007u);
    CHECK_EQ(stop.hits.size(), 1u);
    CHECK_EQ(stop.hits[0].address, 0x9000u);
    CHECK_EQ(stop.hits[0].pc, 0x8006u);
    dbg.clear_checkpoints();
    dbg.add(watch(freetvc::kAccessRead, 0x8001));  // operand of LD HL,nn
    m->jump(0x8000);
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8003u);
}

void port_watchpoints() {
    // 8000 LD A,12h / OUT (00h),A / IN A,(59h) / JR $
    auto m = ram_machine(0x8000, {0x3E, 0x12, 0xD3, 0x00, 0xDB, 0x59, 0x18, 0xFE});
    Debugger dbg(*m);
    dbg.add(watch(freetvc::kAccessOut, 0x00));
    dbg.add(watch(freetvc::kAccessIn, 0x59));
    auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8004u);
    CHECK_EQ(stop.hits[0].address, 0x00u);
    CHECK_EQ(stop.hits[0].value, 0x12u);
    CHECK_EQ(stop.hits[0].where.bank, Bank::Open);
    CHECK_EQ(m->video().border(), 1u);  // 12h: b1 set = blue
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8006u);
    CHECK_EQ(stop.hits[0].access, freetvc::kAccessIn);
    CHECK_EQ(stop.hits[0].value, dbg.registers().af >> 8);
}

void checkpoint_options() {
    // 8000 LD A,(HL) / INC L / JR 8000h; HL = 9000h, 9000h-9003h = 11 22 55 44
    auto m = ram_machine(0x8000, {0x7E, 0x2C, 0x18, 0xFC});
    m->load(0x9000, std::vector<std::uint8_t>{0x11, 0x22, 0x55, 0x44});
    freetvc::Z80Registers r = m->cpu().registers();
    r.hl = 0x9000;
    m->cpu().set_registers(r);
    Debugger dbg(*m);

    Checkpoint logged = watch(freetvc::kAccessRead, 0x9000, 0x9003);
    logged.stop = false;
    dbg.add(logged);
    Checkpoint conditional = watch(freetvc::kAccessRead, 0x9000, 0x90FF);
    conditional.condition = [](const Machine&, const freetvc::debug::Hit& hit) { return hit.value == 0x55; };
    const unsigned cond_id = dbg.add(conditional);
    auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.hits.size(), 1u);
    CHECK_EQ(stop.hits[0].address, 0x9002u);
    CHECK_EQ(dbg.hit_log().size(), 4u);  // three logged reads + the stopping one
    CHECK_EQ(dbg.find(cond_id)->hits, 1u);

    // Ignore count, then a temporary checkpoint
    dbg.clear_checkpoints();
    Checkpoint ignored = watch(freetvc::kAccessExec, 0x8001);
    ignored.ignore = 2;
    const unsigned ign_id = dbg.add(ignored);
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(dbg.find(ign_id)->hits, 3u);
    CHECK_EQ(dbg.registers().hl, 0x9005u);  // stopped at the third pass, two ignored
    dbg.clear_checkpoints();
    Checkpoint temp = watch(freetvc::kAccessExec, 0x8002);
    temp.temporary = true;
    const unsigned temp_id = dbg.add(temp);
    CHECK_EQ(dbg.run(kBudget).why, Why::Checkpoint);
    CHECK_EQ(dbg.find(temp_id) == nullptr, true);
    CHECK_EQ(dbg.run(kBudget).why, Why::Budget);
}

void stepping() {
    // 8000 CALL 8010h / NOP / JR $ ; 8010 INC A / CALL 8020h / RET ; 8020 INC A / RET
    auto m = ram_machine(0x8000, {0xCD, 0x10, 0x80, 0x00, 0x18, 0xFE});
    m->load(0x8010, std::vector<std::uint8_t>{0x3C, 0xCD, 0x20, 0x80, 0xC9});
    m->load(0x8020, std::vector<std::uint8_t>{0x3C, 0xC9});
    Debugger dbg(*m);
    auto stop = dbg.step_over(kBudget);
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x8003u);
    CHECK_EQ(stop.run.instructions_run, 6u);
    CHECK_EQ(dbg.registers().sp, 0xA000u);

    m->jump(0x8000);
    stop = dbg.step(1, kBudget);
    CHECK_EQ(stop.pc, 0x8010u);
    stop = dbg.step(2, kBudget);
    CHECK_EQ(stop.pc, 0x8020u);
    stop = dbg.step_out(kBudget);  // out of 8020h back into 8010h's frame
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x8014u);
    stop = dbg.step_out(kBudget);
    CHECK_EQ(stop.pc, 0x8003u);
    CHECK_EQ(dbg.registers().sp, 0xA000u);

    // A checkpoint inside a stepped-over call stops it
    m->jump(0x8000);
    dbg.add(watch(freetvc::kAccessExec, 0x8020));
    stop = dbg.step_over(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8020u);
    dbg.clear_checkpoints();

    // run_to
    m->jump(0x8000);
    stop = dbg.run_to(0x8014, std::nullopt, kBudget);
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x8014u);
    CHECK_EQ(stop.run.instructions_run, 5u);  // CALL, INC A, CALL, INC A, RET
}

void sliced_steps_and_offsets() {
    // The stepping program; step over in 50-tick slices keeps its target across run() calls
    auto m = ram_machine(0x8000, {0xCD, 0x10, 0x80, 0x00, 0x18, 0xFE});
    m->load(0x8010, std::vector<std::uint8_t>{0x3C, 0xCD, 0x20, 0x80, 0xC9});
    m->load(0x8020, std::vector<std::uint8_t>{0x3C, 0xC9});
    Debugger dbg(*m);
    CHECK_EQ(dbg.start_step_over(), true);
    CHECK_EQ(dbg.stepping(), true);
    freetvc::debug::Stop stop;
    int slices = 0;
    do {
        stop = dbg.run(50);
        ++slices;
    } while (stop.why == Why::Budget && slices < 1000);
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x8003u);
    CHECK_EQ(slices > 2, true);
    CHECK_EQ(dbg.stepping(), false);
    CHECK_EQ(dbg.start_step_over(), false);  // NOP: a plain step

    // A bank-offset checkpoint: U2 offset 0020h wherever U2 is paged (page 2 here, so 8020h)
    m->jump(0x8000);
    Checkpoint offset = watch(freetvc::kAccessExec, 0x0020);
    offset.bank = Bank::U2;
    offset.offsets = true;
    dbg.add(offset);
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x8020u);
    CHECK_EQ(stop.hits[0].where.offset, 0x0020u);
    // A checkpoint hit ends a pending step
    m->jump(0x8000);
    dbg.start_run_to(0x8004, std::nullopt);
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(dbg.stepping(), false);
}

void step_over_recursion() {
    // 8000 LD B,3 / CALL 8010h / HALT ; 8010 DJNZ +3 / RET ; ... 8013 CALL 8010h / RET
    auto m = ram_machine(0x8000, {0x06, 0x03, 0xCD, 0x10, 0x80, 0x76});
    m->load(0x8010, std::vector<std::uint8_t>{0x10, 0x01, 0xC9, 0xCD, 0x10, 0x80, 0xC9});
    Debugger dbg(*m);
    dbg.step(1, kBudget);
    auto stop = dbg.step(2, kBudget);  // CALL, DJNZ taken: now at 8013h, the nested CALL
    CHECK_EQ(stop.pc, 0x8013u);
    stop = dbg.step_over(kBudget);  // returns to 8016h only at this stack level
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x8016u);
    CHECK_EQ(dbg.registers().sp, 0xA000u - 2);
}

void step_over_specials() {
    // RST 30h + function byte: 0030h mimics INFUNC (EX (SP),HL / INC HL / EX (SP),HL / RET)
    auto m = ram_machine(0x8000, {0xF7, 0x07, 0x00, 0x18, 0xFE});
    m->load(0x0030, std::vector<std::uint8_t>{0xE3, 0x23, 0xE3, 0xC9});
    Debugger dbg(*m);
    auto stop = dbg.step_over(kBudget);
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x8002u);

    // LDIR runs to completion
    m->load(0x8000, std::vector<std::uint8_t>{0x21, 0x00, 0x90, 0x11, 0x00, 0x91, 0x01, 0x10, 0x00, 0xED, 0xB0, 0x18, 0xFE});
    m->jump(0x8000);
    dbg.step(3, kBudget);
    CHECK_EQ(m->pc(), 0x8009u);
    stop = dbg.step_over(kBudget);
    CHECK_EQ(stop.pc, 0x800Bu);
    CHECK_EQ(dbg.registers().bc, 0u);
    CHECK_EQ(stop.run.instructions_run, 16u);
}

void halt_and_interrupt() {
    // 0038 OUT (07h),A / EI / RET. 8000 IM 1 / pitch F00h, sound IRQ on / IN A,(5Bh) (INSTART) / EI / HALT / INC A / JR $
    auto m = ram_machine(0x8000, {0xED, 0x56, 0x3E, 0x00, 0xD3, 0x04, 0x3E, 0x2F, 0xD3, 0x05, 0xDB, 0x5B, 0xFB, 0x76, 0x3C,
                                  0x18, 0xFE});
    m->load(0x0038, std::vector<std::uint8_t>{0xD3, 0x07, 0xFB, 0xC9});
    Debugger dbg(*m);
    dbg.run_to(0x800D, std::nullopt, kBudget);
    CHECK_EQ(m->pc(), 0x800Du);
    Checkpoint after = watch(freetvc::kAccessExec, 0x800E);
    after.stop = false;
    const unsigned after_id = dbg.add(after);
    dbg.set_trace_capacity(64);
    const auto stop = dbg.step_over(kBudget);
    CHECK_EQ(stop.why, Why::Step);
    CHECK_EQ(stop.pc, 0x800Eu);
    CHECK_EQ(dbg.registers().sp, 0xA000u);
    CHECK_EQ(dbg.find(after_id)->hits, 1u);  // not once per refetch while halted
    const auto entries = dbg.trace().last(64);
    CHECK_EQ(entries.size(), 4u);  // 0038h, 003Ah, 003Bh, 800Eh: the halted refetches are not instructions
    if (entries.size() == 4) {
        CHECK_EQ(entries[0].regs.pc, 0x0038u);
        CHECK_EQ(entries[0].interrupt, true);
        CHECK_EQ(entries[1].interrupt, false);
        CHECK_EQ(entries[3].regs.pc, 0x800Eu);
    }
}

void event_checkpoints() {
    // The HALT program of halt_and_interrupt, run straight through: an IRQ checkpoint stops before 0038h
    auto m = ram_machine(0x8000, {0xED, 0x56, 0x3E, 0x00, 0xD3, 0x04, 0x3E, 0x2F, 0xD3, 0x05, 0xDB, 0x5B, 0xFB, 0x76, 0x3C,
                                  0x18, 0xFE});
    m->load(0x0038, std::vector<std::uint8_t>{0xD3, 0x07, 0xFB, 0xC9});
    Debugger dbg(*m);
    dbg.add(watch(freetvc::kAccessIrq, 0x0000, 0xFFFF));
    auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x0038u);
    CHECK_EQ(stop.hits[0].access, freetvc::kAccessIrq);
    CHECK_EQ(stop.hits[0].where.bank, Bank::U0);
    CHECK_EQ(m->entered_interrupt(), true);

    // Raster row 100: stops in that row, once per frame
    dbg.clear_checkpoints();
    dbg.add(watch(freetvc::kAccessLine, 100));
    stop = dbg.run(2 * kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(m->video().beam_line(), 100u);
    CHECK_EQ(stop.hits[0].address, 100u);
    const std::uint64_t frame = m->frame_count();
    stop = dbg.run(2 * kBudget);  // the CRTC is not programmed here, so the beam free-runs: frames are longer than 314 rows
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(m->frame_count(), frame + 1);
    CHECK_EQ(m->video().beam_line(), 100u);

    // Break at reset: an exec checkpoint at 0000h stops before the first instruction; a jump there does not
    dbg.clear_checkpoints();
    dbg.add(watch(freetvc::kAccessExec, 0x0000));
    m->reset();
    stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.pc, 0x0000u);
    CHECK_EQ(stop.run.instructions_run, 0u);
    m->power_on();
    CHECK_EQ(dbg.run(kBudget).why, Why::Checkpoint);
    m->io_write(0x02, 0xB0);
    m->load(0x0000, std::vector<std::uint8_t>{0xC3, 0x10, 0x00});  // JP 0010h; 0010h: JR $
    m->load(0x0010, std::vector<std::uint8_t>{0x18, 0xFE});
    m->jump(0x0000);
    CHECK_EQ(dbg.run(kBudget).why, Why::Budget);
}

void registers_and_boundaries() {
    // 8000 LD HL,1234h (10 T) / JR $ ; 8010 INC A / JR $
    auto m = ram_machine(0x8000, {0x21, 0x34, 0x12, 0x18, 0xFE});
    m->load(0x8010, std::vector<std::uint8_t>{0x3C, 0x18, 0xFE});
    Debugger dbg(*m);
    auto stop = dbg.run(6);  // 3 T: inside LD HL,nn
    CHECK_EQ(stop.why, Why::Budget);
    CHECK_EQ(m->at_boundary(), false);
    CHECK_EQ(m->pc(), 0x8000u);
    CHECK_EQ(dbg.set_registers(dbg.registers()), false);
    stop = dbg.step(1, kBudget);
    CHECK_EQ(stop.pc, 0x8003u);
    CHECK_EQ(dbg.registers().hl, 0x1234u);

    freetvc::Z80Registers r = dbg.registers();
    r.pc = 0x8010;
    r.af = 0x4100;
    CHECK_EQ(dbg.set_registers(r), true);
    CHECK_EQ(m->pc(), 0x8010u);
    stop = dbg.step(1, kBudget);
    CHECK_EQ(stop.pc, 0x8011u);
    CHECK_EQ(dbg.registers().af >> 8, 0x42u);
}

void trace_ring() {
    // 8000 LD A,1 / INC A / INC A / JR $
    auto m = ram_machine(0x8000, {0x3E, 0x01, 0x3C, 0x3C, 0x18, 0xFE});
    Debugger dbg(*m);
    dbg.set_trace_capacity(8);
    dbg.step(3, kBudget);
    const auto entries = dbg.trace().last(16);
    CHECK_EQ(entries.size(), 4u);  // the restart fetch at 8000h is recorded, the JR not yet
    if (entries.size() == 4) {
        CHECK_EQ(entries[0].regs.pc, 0x8000u);
        CHECK_EQ(entries[1].regs.pc, 0x8002u);
        CHECK_EQ(entries[1].regs.af >> 8, 1u);
        CHECK_EQ(entries[2].regs.af >> 8, 2u);
        CHECK_EQ(entries[3].regs.pc, 0x8004u);
        CHECK_EQ(entries[3].bytes[0], 0x18u);
        CHECK_EQ(entries[2].where.bank, Bank::U2);
        CHECK_EQ(entries[2].ticks > entries[1].ticks, true);
    }
    dbg.step(20, kBudget);
    CHECK_EQ(dbg.trace().size(), 8u);
    dbg.set_trace_capacity(0);
    CHECK_EQ(dbg.every_instruction, false);
}

void saved_states() {
    // 8000 INC (HL) / JR 8000h; HL = 9000h
    auto m = ram_machine(0x8000, {0x34, 0x18, 0xFD});
    freetvc::Z80Registers r = m->cpu().registers();
    r.hl = 0x9000;
    m->cpu().set_registers(r);
    Debugger dbg(*m);
    dbg.run(kBudget / 3);
    dbg.save_state("a");
    const std::uint64_t ticks = m->ticks();
    const std::uint8_t counter = m->peek(0x9000);
    dbg.run(kBudget * 2 + 2000);  // 23 T per pass: not a multiple of 256 passes
    const std::uint8_t later = m->peek(0x9000);
    const std::uint64_t later_ticks = m->ticks();
    const std::vector<std::uint8_t> later_frame = m->framebuffer().pixels;
    CHECK_EQ(later != counter, true);
    CHECK_EQ(dbg.load_state("a"), true);
    CHECK_EQ(m->ticks(), ticks);
    CHECK_EQ(m->peek(0x9000), counter);
    CHECK_EQ(m->debug_hooks() == &dbg, true);
    CHECK_EQ(m->memory().locate(0x8000).bank, Bank::U2);  // page table rebuilt in the copy
    dbg.run(kBudget * 2 + 2000);
    CHECK_EQ(m->peek(0x9000), later);
    CHECK_EQ(m->ticks(), later_ticks);
    CHECK_EQ(m->framebuffer().pixels == later_frame, true);
    CHECK_EQ(dbg.state_names().size(), 1u);
    CHECK_EQ(dbg.delete_state("a"), true);
    CHECK_EQ(dbg.load_state("a"), false);
}

void banks() {
    freetvc::MemoryMap map;
    map.configure(freetvc::Model::Tvc64kPlus, {});
    map.reset();
    CHECK_EQ(map.locate(0x0000).bank, Bank::Sys);
    CHECK_EQ(map.locate(0x8123).bank, Bank::Vid0);
    CHECK_EQ(map.locate(0x8123).offset, 0x0123u);
    CHECK_EQ(map.locate(0xC000).bank, Bank::Cart);
    map.set_page_register(0xC4);  // page 1 VID, page 3 IOMEM
    map.set_video_paging(0x02);   // page 1 = VID bank 2
    map.set_iomem_slot(1);
    CHECK_EQ(map.locate(0x4000).bank, Bank::Vid2);
    CHECK_EQ(map.locate(0xC010).bank, Bank::Card1);
    CHECK_EQ(map.locate(0xC010).offset, 0x0010u);
    CHECK_EQ(map.locate(0xE001).bank, Bank::Ext);
    CHECK_EQ(map.locate(0xE001).offset, 0x0001u);
    CHECK_EQ(map.bank_bytes(Bank::Ext).size(), 0x2000u);
    CHECK_EQ(map.bank_bytes(Bank::Vid3).size(), 0x4000u);
    CHECK_EQ(map.bank_bytes(Bank::Card0).size(), 0u);

    freetvc::MemoryMap small;
    small.configure(freetvc::Model::Tvc32k, {});
    small.set_page_register(0x20 | 0x80);  // page 2 U2, page 3 U3: absent on the 32k
    CHECK_EQ(small.locate(0x8000).bank, Bank::Open);
    CHECK_EQ(small.locate(0xC000).bank, Bank::Open);
    CHECK_EQ(small.bank_bytes(Bank::U2).size(), 0u);
    CHECK_EQ(small.bank_bytes(Bank::Vid1).size(), 0u);
    CHECK_EQ(std::string_view(freetvc::bank_name(Bank::Vid2)) == "VID2", true);
}

void decoder_flows() {
    using freetvc::debug::decode;
    const auto d = [](std::initializer_list<std::uint8_t> b) {
        const std::vector<std::uint8_t> v(b);
        return decode(v);
    };
    CHECK_EQ(d({0xCD}).flow, Flow::Call);
    CHECK_EQ(d({0xCD}).length, 3u);
    CHECK_EQ(d({0xC4}).conditional, true);
    CHECK_EQ(d({0xF7}).flow, Flow::Call);
    CHECK_EQ(d({0xF7}).length, 1u);
    CHECK_EQ(d({0xC9}).flow, Flow::Return);
    CHECK_EQ(d({0xD8}).conditional, true);
    CHECK_EQ(d({0xED, 0x4D}).flow, Flow::Return);
    CHECK_EQ(d({0xED, 0x45}).flow, Flow::Return);
    CHECK_EQ(d({0x10}).flow, Flow::Branch);
    CHECK_EQ(d({0x18}).flow, Flow::Jump);
    CHECK_EQ(d({0x38}).length, 2u);
    CHECK_EQ(d({0xC2}).length, 3u);
    CHECK_EQ(d({0xE9}).flow, Flow::Jump);
    CHECK_EQ(d({0xDD, 0xE9}).flow, Flow::Jump);
    CHECK_EQ(d({0xDD, 0xE9}).length, 2u);
    CHECK_EQ(d({0xED, 0xB0}).flow, Flow::Repeat);
    CHECK_EQ(d({0xED, 0xBB}).flow, Flow::Repeat);
    CHECK_EQ(d({0xED, 0xA0}).flow, Flow::Next);
    CHECK_EQ(d({0x76}).flow, Flow::Halt);
    CHECK_EQ(d({0xFD, 0x36}).length, 4u);
    CHECK_EQ(d({0xDD, 0xCB, 0x05, 0x46}).length, 4u);
    CHECK_EQ(d({0xED, 0x43}).length, 4u);
    CHECK_EQ(d({0xDD, 0xFD, 0x21}).length, 5u);
    CHECK_EQ(d({0xFD, 0xED, 0xB0}).flow, Flow::Repeat);
    CHECK_EQ(d({0xFD, 0xED, 0xB0}).length, 3u);
}

// Every instruction that always falls through: decode() length = PC advance in z80.h
void decoder_lengths_vs_cpu() {
    auto base = ram_machine(0x8000, {});
    freetvc::Z80Registers r = base->cpu().registers();
    r.hl = r.ix = r.iy = 0x9000;
    r.de = 0x9100;
    r.bc = 0x0001;  // OUT (C) goes to port 01h (printer data)
    base->cpu().set_registers(r);
    Machine m(*base);
    int checked = 0;
    std::vector<std::array<std::uint8_t, 4>> cases;
    for (unsigned op = 0; op < 256; ++op) {
        const auto b = static_cast<std::uint8_t>(op);
        cases.push_back({b, 0, 0, 0});
        cases.push_back({0xCB, b, 0, 0});
        cases.push_back({0xED, b, 0, 0});
        cases.push_back({0xDD, b, 0, 0});
        cases.push_back({0xFD, b, 0, 0});
        cases.push_back({0xDD, 0xCB, 0, b});
        cases.push_back({0xFD, 0xCB, 0, b});
    }
    for (const auto& bytes : cases) {
        const freetvc::debug::Decoded d = freetvc::debug::decode(bytes);
        if (d.flow != Flow::Next) {
            continue;
        }
        m.restore(*base);
        m.load(0x8000, bytes);
        m.jump(0x8000);
        const freetvc::StopInfo info = m.run({.max_ticks = 2000, .max_instructions = 1});
        const unsigned advance = static_cast<std::uint16_t>(m.pc() - 0x8000);
        if (info.reason != freetvc::StopReason::InstructionCount || advance != d.length) {
            ++g_failures;
            std::printf("FAIL decode %02X %02X %02X %02X: length %u, CPU advanced %u\n", bytes[0], bytes[1], bytes[2], bytes[3],
                        d.length, advance);
        }
        ++checked;
    }
    std::printf("decoder: %d fall-through instructions checked against z80.h\n", checked);
}

void heat_map() {
    using freetvc::debug::HeatMap;
    // 8000 LD A,(9000h) / LD (9001h),A / JR $
    auto m = ram_machine(0x8000, {0x3A, 0x00, 0x90, 0x32, 0x01, 0x90, 0x18, 0xFE});
    Debugger dbg(*m);
    const unsigned id = dbg.add(watch(freetvc::kAccessWrite, 0x9001));
    dbg.set_heat(true);
    const auto at = [&](std::uint16_t offset, HeatMap::Kind kind) { return dbg.heat().at({Bank::U2, offset}, kind); };
    auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.hits.size(), 1u);
    CHECK_EQ(stop.hits[0].checkpoint, id);
    CHECK_EQ(at(0x0000, HeatMap::Exec), 1u);
    CHECK_EQ(at(0x0001, HeatMap::Exec), 1u);  // operand bytes are code, not data reads
    CHECK_EQ(at(0x0001, HeatMap::Read), 0u);
    CHECK_EQ(at(0x1000, HeatMap::Read), 1u);
    CHECK_EQ(at(0x1000, HeatMap::Write), 0u);
    CHECK_EQ(at(0x1001, HeatMap::Write), 1u);
    CHECK_EQ(at(0x1002, HeatMap::Read) + at(0x1002, HeatMap::Write), 0u);
    // Heat flags every address, but only checkpoints stop
    CHECK_EQ(dbg.memory_flags[0x1234] & freetvc::kAccessRead, freetvc::kAccessRead);
    stop = dbg.run(kBudget * 2);
    CHECK_EQ(stop.why, Why::Budget);
    CHECK_EQ(at(0x0006, HeatMap::Exec), static_cast<std::uint32_t>(m->frame_count() + 1));
    dbg.set_heat(false);
    CHECK_EQ(dbg.memory_flags[0x1234], 0u);
    CHECK_EQ(dbg.memory_flags[0x9001], freetvc::kAccessWrite);
}

void beam_events() {
    using freetvc::debug::BeamEvent;
    // 8000 LD A,5 / OUT (00h),A / IN A,(58h) / OUT (60h),A / JR $
    auto m = ram_machine(0x8000, {0x3E, 0x05, 0xD3, 0x00, 0xDB, 0x58, 0xD3, 0x60, 0x18, 0xFE});
    Debugger dbg(*m);
    const unsigned id = dbg.add(watch(freetvc::kAccessOut, 0x60));
    dbg.set_events(true);
    const auto stop = dbg.run(kBudget);
    CHECK_EQ(stop.why, Why::Checkpoint);
    CHECK_EQ(stop.hits.size(), 1u);
    CHECK_EQ(stop.hits[0].checkpoint, id);
    const auto& events = dbg.events().frame(m->frame_count());
    CHECK_EQ(events.size(), 3u);
    if (events.size() == 3) {
        CHECK_EQ(events[0].kind, BeamEvent::Out);
        CHECK_EQ(events[0].port, 0x00u);
        CHECK_EQ(events[0].value, 0x05u);
        CHECK_EQ(events[0].pc, 0x8002u);
        CHECK_EQ(events[1].kind, BeamEvent::In);
        CHECK_EQ(events[1].port, 0x58u);
        CHECK_EQ(events[1].pc, 0x8004u);
        CHECK_EQ(events[2].port, 0x60u);
        CHECK_EQ(events[2].line, m->video().beam_line());  // stopped right after the access
    }
    // The I/O log has the same events with their time, oldest first
    CHECK_EQ(dbg.io_log().size(), 3u);
    if (dbg.io_log().size() == 3) {
        CHECK_EQ(dbg.io_log().at(0).event.port, 0x00u);
        CHECK_EQ(dbg.io_log().at(1).event.kind, BeamEvent::In);
        CHECK_EQ(dbg.io_log().at(2).event.port, 0x60u);
        CHECK_EQ(dbg.io_log().at(2).frame, m->frame_count());
        CHECK_EQ(dbg.io_log().at(1).ticks < dbg.io_log().at(2).ticks, true);
        CHECK_EQ(dbg.io_log().at(2).ticks <= m->ticks(), true);
    }
    // Events flag every port, but only checkpoints stop
    CHECK_EQ(dbg.port_flags[0x58] & freetvc::kAccessIn, freetvc::kAccessIn);
    dbg.set_events(false);
    CHECK_EQ(dbg.port_flags[0x58], 0u);
    CHECK_EQ(dbg.port_flags[0x60], freetvc::kAccessOut);
    CHECK_EQ(dbg.events().frame(m->frame_count()).size(), 0u);
    CHECK_EQ(dbg.io_log().size(), 3u);  // kept while off
    dbg.clear_io_log();
    CHECK_EQ(dbg.io_log().size(), 0u);
}

void profile_calls() {
    // 8000 CALL 8010 / CALL 8010 / JR $; 8010 CALL 8020 / RET; 8020 NOP / RET
    std::vector<std::uint8_t> code(0x22, 0x00);
    const std::uint8_t main_part[] = {0xCD, 0x10, 0x80, 0xCD, 0x10, 0x80, 0x18, 0xFE};
    std::copy(std::begin(main_part), std::end(main_part), code.begin());
    code[0x10] = 0xCD;
    code[0x11] = 0x20;
    code[0x12] = 0x80;
    code[0x13] = 0xC9;
    code[0x21] = 0xC9;
    auto m = ram_machine(0x8000, {});
    m->load(0x8000, code);
    Debugger dbg(*m);
    dbg.set_profile(true);
    const auto stop = dbg.run_to(0x8006, std::nullopt, kBudget);
    CHECK_EQ(stop.why, Why::Step);
    const auto& p = dbg.profile();
    // T-states: CALL 17, RET 10, NOP 4; ticks are half-T
    CHECK_EQ(p.instructions(), 10u);
    CHECK_EQ(p.total_ticks(), 2u * (4 * 17 + 4 * 10 + 2 * 4));
    CHECK_EQ(p.count({Bank::U2, 0x0020}), 2u);
    CHECK_EQ(p.ticks({Bank::U2, 0x0020}), 2u * 2 * 4);
    CHECK_EQ(p.depth(), 0u);
    const auto find = [&](std::uint16_t offset) -> const freetvc::debug::RoutineStats* {
        for (const auto& r : p.routines()) {
            if (r.entry.bank == Bank::U2 && r.entry.offset == offset) {
                return &r;
            }
        }
        return nullptr;
    };
    const auto* outer = find(0x0010);
    const auto* inner = find(0x0020);
    CHECK_EQ(outer != nullptr && inner != nullptr, true);
    if (outer && inner) {
        CHECK_EQ(outer->calls, 2u);
        CHECK_EQ(outer->exclusive, 2u * 2 * (17 + 10));
        CHECK_EQ(outer->inclusive, 2u * 2 * (17 + 4 + 10 + 10));
        CHECK_EQ(inner->calls, 2u);
        CHECK_EQ(inner->exclusive, 2u * 2 * (4 + 10));
        CHECK_EQ(inner->inclusive, inner->exclusive);
    }
    CHECK_EQ(p.routines().front().entry.bank, Bank::Open);  // outside any call: the two CALLs at 8000 and 8003
    CHECK_EQ(p.routines().front().exclusive, 2u * 2 * 17);
    // Off keeps the counts; clear forgets them
    dbg.set_profile(false);
    dbg.run(1000);
    CHECK_EQ(p.instructions(), 10u);
    dbg.clear_profile();
    CHECK_EQ(p.instructions(), 0u);
    CHECK_EQ(p.routines().size(), 0u);
}

}  // namespace

int main() {
    exec_checkpoint();
    memory_watchpoints();
    port_watchpoints();
    checkpoint_options();
    stepping();
    step_over_recursion();
    sliced_steps_and_offsets();
    step_over_specials();
    halt_and_interrupt();
    event_checkpoints();
    registers_and_boundaries();
    trace_ring();
    saved_states();
    banks();
    decoder_flows();
    decoder_lengths_vs_cpu();
    heat_map();
    beam_events();
    profile_calls();
    if (g_failures == 0) {
        std::printf("debug_test: all checks passed\n");
        return 0;
    }
    std::printf("debug_test: %d check(s) failed\n", g_failures);
    return 1;
}
