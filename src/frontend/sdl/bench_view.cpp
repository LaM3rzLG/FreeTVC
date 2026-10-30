// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/bench_view.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

#include <SDL3/SDL.h>

#include "debug/expression.hpp"
#include "frontend/sdl/deck.hpp"
#include "frontend/sdl/text.hpp"
#include "imgui_internal.h"

namespace freetvc::sdl {

namespace {

constexpr ImGuiWindowFlags kFixedFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
constexpr const char* kScreen = "###screen";
constexpr const char* kListing = "###listing";
constexpr const char* kCpu = "###cpu";
constexpr const char* kMemory = "###memory";
constexpr const char* kMachine = "###machine";
constexpr const char* kConsole = "###console";
constexpr const char* kAgent = "###agent";
constexpr const char* kCheckpoints = "###checkpoints";
constexpr const char* kWatch = "###watch";
constexpr const char* kHistory = "###history";
constexpr const char* kIoLog = "###iolog";
constexpr const char* kVideo = "###video";
constexpr const char* kProfiler = "###profiler";
constexpr const char* kSource = "###source";
constexpr std::size_t kHotInstructions = 500;
constexpr double kTStatesPerSecond = 3125000.0;
constexpr const char* kModeNames[] = {N_("2-colour"), N_("4-colour"), N_("16-colour"), N_("16-colour")};
constexpr double kDeleteAllSeconds = 3.0;  // the second click of "Delete all" must come within this
constexpr unsigned kWatchBytes = 8;
constexpr unsigned kWatchText = 32;
constexpr std::size_t kConsoleLines = 4000;
// Tabs that join a node behind its first panel: appearing does not bring them to the front
constexpr ImGuiWindowFlags kSecondaryTab = ImGuiWindowFlags_NoFocusOnAppearing;
constexpr std::uint32_t kHeatFrames = 50;  // a byte's tint fades out in about a second
constexpr float kPagesWidth = 76.0f;
constexpr unsigned kWheelLines = 3;
// Listing columns, in character widths of the data font
constexpr float kGutter = 4.0f;
constexpr float kBytesColumn = 10.0f;
constexpr float kTextColumn = 22.0f;
constexpr float kCommentColumn = 44.0f;

std::string hex(unsigned value, int digits) {
    char text[12];
    std::snprintf(text, sizeof text, "%0*X", digits, value);
    return text;
}

ImVec4 vec(Colour c, float alpha = 1.0f) {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f * alpha};
}

// A fixed window over a rectangle of the viewport, with no padding
bool begin_fixed(const char* id, ImVec2 min, ImVec2 size) {
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    const bool open = ImGui::Begin(id, nullptr, kFixedFlags);
    ImGui::PopStyleVar(3);
    return open;
}

// Dense data panels (Listing, Memory, Console): panel and case text, or putty and ink with panel.light
struct PanelColours {
    Colour background;
    Colour text;
    bool light = false;
    Colour role(const Skin& skin, Role r) const { return light ? skin.role(r).on_putty : skin.role(r).on_case; }
};

PanelColours panel_colours(const Skin& skin) {
    return skin.panel_light ? PanelColours{skin.putty, skin.ink, true} : PanelColours{skin.panel, skin.label, false};
}

// The panel's text and control colours; push after Begin(), which draws the dock tab with the case colours. Returns the count to pop.
int push_panel(const Skin& skin) {
    const PanelColours panel = panel_colours(skin);
    const std::pair<ImGuiCol, ImVec4> colours[] = {
        {ImGuiCol_Text, vec(panel.text)},
        {ImGuiCol_TextDisabled, vec(panel.text, 0.55f)},
        {ImGuiCol_FrameBg, vec(panel.text, 0.08f)},
        {ImGuiCol_FrameBgHovered, vec(panel.text, 0.14f)},
        {ImGuiCol_FrameBgActive, vec(panel.text, 0.18f)},
        {ImGuiCol_Button, vec(panel.text, 0.10f)},
        {ImGuiCol_ButtonHovered, vec(panel.text, 0.16f)},
        {ImGuiCol_ButtonActive, vec(panel.text, 0.22f)},
        {ImGuiCol_Header, vec(panel.text, 0.12f)},
        {ImGuiCol_HeaderHovered, vec(panel.text, 0.18f)},
        {ImGuiCol_HeaderActive, vec(panel.text, 0.24f)},
        {ImGuiCol_PopupBg, vec(panel.background)},
        {ImGuiCol_CheckMark, vec(panel.text)},
        {ImGuiCol_TextSelectedBg, vec(panel.text, 0.25f)},
        {ImGuiCol_InputTextCursor, vec(panel.text)},
    };
    for (const auto& [index, colour] : colours) {
        ImGui::PushStyleColor(index, colour);
    }
    return static_cast<int>(std::size(colours));
}

// What a port access does (tvc-hardware.md §4; core/machine/machine.cpp io_write / io_read)
std::string port_name(const debug::BeamEvent& e) {
    const unsigned p = e.port;
    if (p & 0x80) {
        return tr("unused");
    }
    if (e.kind == debug::BeamEvent::Out) {
        switch (p) {
        case 0x00: return tr("border");
        case 0x01: return tr("printer data");
        case 0x02: return tr("paging");
        case 0x03: return tr("IOMEM slot, keyboard row");
        case 0x04: return tr("pitch low");
        case 0x05: return tr("sound control, pitch high");
        case 0x06: return tr("video mode, amplitude");
        case 0x07: return tr("IRQ acknowledge");
        default: break;
        }
        if (p >= 0x0C && p <= 0x0F) {
            return tr("video paging");
        }
        if (p >= 0x10 && p < 0x50) {
            return std::string(tr("slot")) + " " + std::to_string((p >> 4) - 1);
        }
        if (p >= 0x50 && p < 0x58) {
            return tr("tape out");
        }
        if (p >= 0x58 && p < 0x60) {
            return std::string(tr("slot IRQ enable")) + " " + std::to_string(p & 3);
        }
        if (p >= 0x60 && p < 0x70) {
            return std::string(tr("palette")) + " " + std::to_string(p & 3);
        }
        return (p & 1) ? "CRTC R" + std::to_string(e.crtc_register) : std::string(tr("CRTC select"));
    }
    if (p >= 0x50 && p < 0x58) {
        return tr("tape in");
    }
    if (p >= 0x58 && p < 0x60) {
        switch (p & 3) {
        case 0: return tr("keyboard");
        case 1: return tr("status");
        case 2: return tr("card IDs");
        default: return tr("INSTART");
        }
    }
    if (p >= 0x70) {
        return (p & 1) ? "CRTC R" + std::to_string(e.crtc_register) : std::string(tr("unused"));
    }
    return p >= 0x10 && p < 0x50 ? std::string(tr("slot")) + " " + std::to_string((p >> 4) - 1) : std::string(tr("unused"));
}

std::string describe_event(const debug::BeamEvent& e) {
    char text[128];
    if (e.kind == debug::BeamEvent::Irq) {
        std::snprintf(text, sizeof text, tr("line %u col %u: interrupt, handler at %04X"), e.line, e.column, e.pc);
    } else {
        std::snprintf(text, sizeof text, "%s %u %s %u: %s %02Xh %s %02Xh (%s), %s %04X", tr("line"), e.line, tr("col"), e.column,
                      e.kind == debug::BeamEvent::Out ? "OUT" : "IN", e.port, e.kind == debug::BeamEvent::Out ? "=" : "->",
                      e.value, port_name(e).c_str(), tr("from"), e.pc);
    }
    return text;
}

bool writable(Bank bank) { return (bank >= Bank::U0 && bank <= Bank::U3) || (bank >= Bank::Vid0 && bank <= Bank::Vid3); }

// 0: no exec checkpoint covers the address; 1: an enabled one; 2: only disabled ones
int checkpoint_state(const debug::Debugger& debugger, std::uint16_t address) {
    const BankAddress where = debugger.machine().memory().locate(address);
    int state = 0;
    for (const debug::Checkpoint& c : debugger.checkpoints()) {
        if ((c.access & kAccessExec) == 0) {
            continue;
        }
        const std::uint16_t at = c.bank && c.offsets ? where.offset : address;
        if ((c.bank && *c.bank != where.bank) || at < c.start || at > c.end) {
            continue;
        }
        if (c.enabled) {
            return 1;
        }
        state = 2;
    }
    return state;
}

// The add form's kinds: monitor words and the hint for the place field
struct AddKind {
    const char* label;
    const char* words;
    const char* hint;
};
constexpr AddKind kAddKinds[] = {
    {N_("exec"), "exec", N_("[bank:]start [end]")},
    {N_("load"), "load", N_("[bank:]start [end]")},
    {N_("store"), "store", N_("[bank:]start [end]")},
    {N_("load + store"), "load store", N_("[bank:]start [end]")},
    {N_("in"), "in", N_("port [port]")},
    {N_("out"), "out", N_("port [port]")},
    {N_("in + out"), "in out", N_("port [port]")},
    {N_("irq"), "irq", N_("handler address, empty: any")},
    {N_("line"), "line", N_("#row [#row]")},
};

constexpr std::pair<std::uint8_t, const char*> kAccessNames[] = {
    {kAccessExec, "exec"}, {kAccessRead, "load"}, {kAccessWrite, "store"}, {kAccessIn, "in"},
    {kAccessOut, "out"},   {kAccessIrq, "irq"},   {kAccessLine, "line"},
};

std::string access_names(std::uint8_t access) {
    std::string out;
    for (const auto& [bit, name] : kAccessNames) {
        if (access & bit) {
            out += (out.empty() ? "" : " ") + std::string(name);
        }
    }
    return out;
}

// The colour meaning of a checkpoint's first kind (design/ui.md "Colour meaning")
Colour access_colour(const Skin& skin, const PanelColours& panel, std::uint8_t access) {
    if (access & kAccessExec) {
        return panel.role(skin, Role::Exec);
    }
    if (access & kAccessRead) {
        return panel.role(skin, Role::Read);
    }
    if (access & kAccessWrite) {
        return panel.role(skin, Role::Write);
    }
    if (access & (kAccessIn | kAccessOut)) {
        return panel.role(skin, Role::Io);
    }
    return access & kAccessIrq ? panel.role(skin, Role::Notice) : panel.text;
}

bool any_handler(const debug::Checkpoint& c) { return (c.access & kAccessIrq) && c.start == 0 && c.end == 0xFFFF; }

std::string checkpoint_place(const debug::Checkpoint& c) {
    if (any_handler(c)) {
        return tr("any handler");
    }
    const bool rows = (c.access & kAccessLine) != 0;
    const bool ports = (c.access & (kAccessIn | kAccessOut)) != 0;
    const auto number = [&](unsigned v) { return rows ? "#" + std::to_string(v) : hex(v, ports ? 2 : 4); };
    std::string out = c.bank && c.offsets ? std::string(bank_name(*c.bank)) + ":" : std::string();
    out += number(c.start);
    if (c.end != c.start) {
        out += "-" + number(c.end);
    }
    if (c.bank && !c.offsets) {
        out += std::string(" ") + tr("in") + " " + bank_name(*c.bank);
    }
    return out;
}

std::string hit_text(const debug::Hit& h) {
    char text[96];
    switch (h.access) {
    case kAccessExec:
        std::snprintf(text, sizeof text, tr("exec at %04X"), h.address);
        break;
    case kAccessRead:
    case kAccessWrite:
        std::snprintf(text, sizeof text, tr("%s %04X = %02X from PC %04X"), h.access == kAccessRead ? "load" : "store", h.address,
                      h.value, h.pc);
        break;
    case kAccessIn:
    case kAccessOut:
        std::snprintf(text, sizeof text, tr("%s %02Xh = %02X from PC %04X"), h.access == kAccessIn ? "in" : "out", h.address, h.value,
                      h.pc);
        break;
    case kAccessIrq:
        std::snprintf(text, sizeof text, tr("irq, handler at %04X"), h.address);
        break;
    default:
        std::snprintf(text, sizeof text, tr("line #%u, PC %04X"), h.address, h.pc);
        break;
    }
    return text;
}

// Where `offset` of `bank` is paged in the CPU's view now
std::optional<std::uint16_t> cpu_address(const Machine& machine, Bank bank, unsigned offset) {
    for (unsigned base = 0; base < 0x10000; base += 0x2000) {
        const BankAddress where = machine.memory().locate(static_cast<std::uint16_t>(base));
        if (where.bank == bank && offset >= where.offset && offset < where.offset + 0x2000u) {
            return static_cast<std::uint16_t>(base + offset - where.offset);
        }
    }
    return std::nullopt;
}

// "C3F1 SYS:03F1 label" where the bank is paged now, else "SYS:03F1"
std::string place_name(const Machine& machine, const debug::SymbolTable& symbols, BankAddress where) {
    std::string name = std::string(bank_name(where.bank)) + ":" + hex(where.offset, 4);
    if (const auto cpu = cpu_address(machine, where.bank, where.offset)) {
        name = hex(*cpu, 4) + " " + name;
        if (const debug::Symbol* symbol = symbols.at(*cpu, where)) {
            name += " " + symbol->name;
        }
    }
    return name;
}

// A share as text over a bar in the role colour
void share_cell(double share, Colour colour) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + width * static_cast<float>(std::clamp(share, 0.0, 1.0)), p.y + height},
                                              with_alpha(colour, 0.35f));
    ImGui::Text("%5.1f%%", share * 100.0);
}

std::string trimmed(const char* text) {
    std::string_view v(text);
    const std::size_t first = v.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(v.substr(first, v.find_last_not_of(" \t") - first + 1));
}

// The registers an instruction changed, as "A=05 F=44 HL=0B01" (R, WZ and Q left out)
std::string register_changes(const Z80Registers& before, const Z80Registers& after, bool interrupt_follows) {
    std::string out;
    const auto add = [&](const char* name, unsigned a, unsigned b, int digits) {
        if (a != b) {
            out += (out.empty() ? "" : " ") + std::string(name) + "=" + hex(b, digits);
        }
    };
    add("A", before.af >> 8, after.af >> 8, 2);
    add("F", before.af & 0xFF, after.af & 0xFF, 2);
    add("BC", before.bc, after.bc, 4);
    add("DE", before.de, after.de, 4);
    add("HL", before.hl, after.hl, 4);
    add("IX", before.ix, after.ix, 4);
    add("IY", before.iy, after.iy, 4);
    if (!interrupt_follows) {
        add("SP", before.sp, after.sp, 4);  // an interrupt's own push is not the instruction's
    }
    add("AF'", before.af2, after.af2, 4);
    add("BC'", before.bc2, after.bc2, 4);
    add("DE'", before.de2, after.de2, 4);
    add("HL'", before.hl2, after.hl2, 4);
    add("I", before.i, after.i, 2);
    add("IM", before.im, after.im, 1);
    if (!interrupt_follows) {
        add("IFF", before.iff1, after.iff1, 1);
    }
    return out;
}

std::string lower_text(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// "70 71 60-63" as a port set; false on a word that is not hex or a range
bool parse_ports(const char* text, std::array<bool, 256>& ports, bool& any) {
    ports.fill(false);
    any = false;
    std::string_view rest(text);
    while (!rest.empty()) {
        const std::size_t start = rest.find_first_not_of(" ,");
        if (start == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(start);
        const std::size_t end = std::min(rest.find_first_of(" ,"), rest.size());
        const std::string word(rest.substr(0, end));
        rest.remove_prefix(end);
        const std::size_t dash = word.find('-');
        char* stop = nullptr;
        const unsigned long low = std::strtoul(word.c_str(), &stop, 16);
        if (stop == word.c_str()) {
            return false;
        }
        unsigned long high = low;
        if (dash != std::string::npos) {
            const char* tail = word.c_str() + dash + 1;
            high = std::strtoul(tail, &stop, 16);
            if (stop == tail) {
                return false;
            }
        }
        if (*stop != 0 || low > 0xFF || high > 0xFF || high < low) {
            return false;
        }
        for (unsigned long p = low; p <= high; ++p) {
            ports[p] = true;
        }
        any = true;
    }
    return true;
}

// A data panel's table colours: the panel shows through, rows and lines in its text colour. Returns the count to pop.
int push_table_colours(const PanelColours& panel) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(panel.background, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, vec(panel.text, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, vec(panel.text, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, vec(panel.text, 0.08f));
    return 4;
}

// A watch's value now, and the memory it reads (Bytes, Text) or names (Value)
struct WatchResult {
    std::string text;
    bool error = false;
    std::optional<Bank> bank;
    std::optional<unsigned> address;
    unsigned length = 1;
};

constexpr const char* kWatchFormats[] = {N_("value"), N_("bytes"), N_("text")};

WatchResult failed(std::string message) {
    WatchResult r;
    r.text = std::move(message);
    r.error = true;
    return r;
}

WatchResult evaluate_watch(const std::string& text, unsigned format, const Machine& machine, const debug::ParseOptions& options) {
    WatchResult r;
    if (format == 0) {
        const auto [e, error] = debug::Expression::parse(text, options);
        if (!e) {
            return failed(error.message);
        }
        const std::int64_t v = e->evaluate({machine});
        const auto u = static_cast<std::uint64_t>(v);
        const int digits = v < 0 || u > 0xFFFF ? 8 : u > 0xFF ? 4 : 2;
        r.text = hex(static_cast<unsigned>(u & 0xFFFFFFFFu), digits) + "h  " + std::to_string(v);
        if (v >= 0x20 && v < 0x7F) {
            r.text += "  '" + std::string(1, static_cast<char>(v)) + "'";
        }
        if (v >= 0 && v <= 0xFFFF) {
            r.address = static_cast<unsigned>(v);
        }
        return r;
    }
    const auto [spec, error] = debug::parse_address(text, options);
    if (!spec) {
        return failed(error.message);
    }
    const std::span<const std::uint8_t> bytes = spec->bank ? machine.memory().bank_bytes(*spec->bank) : std::span<const std::uint8_t>{};
    if (spec->bank && bytes.empty()) {
        return failed(tr("this machine has no such bank"));
    }
    const unsigned size = spec->bank ? static_cast<unsigned>(bytes.size()) : 0x10000u;
    const auto base = static_cast<unsigned>(spec->value.evaluate({machine}) & 0xFFFF) % size;
    const auto byte = [&](unsigned i) {
        const unsigned at = (base + i) % size;
        return spec->bank ? bytes[at] : machine.peek(static_cast<std::uint16_t>(at));
    };
    r.bank = spec->bank;
    r.address = base;
    if (format == 1) {
        for (unsigned i = 0; i < kWatchBytes; ++i) {
            r.text += (i ? " " : "") + hex(byte(i), 2);
        }
        r.length = kWatchBytes;
        return r;
    }
    r.text = "\"";
    unsigned i = 0;
    for (; i < kWatchText && byte(i) != 0; ++i) {
        const std::uint8_t c = byte(i);
        r.text += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
    }
    r.text += i == kWatchText ? "\"..." : "\"";
    r.length = std::max(1u, i);
    return r;
}

}  // namespace

BenchActions BenchView::draw(const PlayStatus& status, remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts,
                             PlayActions& play) {
    BenchActions actions;
    track_stops(monitor);
    const Machine& machine = monitor.debugger().machine();
    const bool held = monitor.active();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = status.scale;
    const float bar_h = skin.strip_height * scale;
    const float groove = skin.groove_width * scale;
    const float pages_w = kPagesWidth * scale;
    const float top = viewport->Pos.y + bar_h + groove;
    const float bottom = viewport->Pos.y + viewport->Size.y - strip_space(skin, scale) - status.bottom_panel;

    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddRectFilled(viewport->Pos, viewport->Pos + viewport->Size, to_imgui(skin.frame));
    background->AddRectFilled({viewport->Pos.x, top - groove}, {viewport->Pos.x + viewport->Size.x, top}, to_imgui(skin.groove));
    background->AddRectFilled({viewport->Pos.x + pages_w, top}, {viewport->Pos.x + pages_w + groove, bottom}, to_imgui(skin.groove));

    draw_run_bar(status, monitor, skin, fonts, viewport->Pos, {viewport->Size.x, bar_h}, play, actions);
    draw_pages(machine, skin, fonts, scale, {viewport->Pos.x, top}, {pages_w, bottom - top});

    // Dock space; the default arrangement only where the saved layout has none
    const ImVec2 dock_min{viewport->Pos.x + pages_w + groove, top};
    const ImVec2 dock_size{viewport->Pos.x + viewport->Size.x - status.side_panel - dock_min.x, bottom - top};
    ImGui::SetNextWindowPos(dock_min);
    ImGui::SetNextWindowSize(dock_size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##bench", nullptr, kFixedFlags | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    const ImGuiID dock = ImGui::GetID("bench.dock");
    if (!layout_built_) {
        layout_built_ = true;
        if (!ImGui::DockBuilderGetNode(dock)) {
            ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock, dock_size);
            ImGuiID left = 0;
            ImGuiID right = 0;
            ImGui::DockBuilderSplitNode(dock, ImGuiDir_Right, 0.30f, &right, &left);
            ImGuiID upper = 0;
            ImGuiID lower = 0;
            ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, &lower, &upper);
            ImGuiID right_upper = 0;
            ImGuiID right_lower = 0;
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, &right_lower, &right_upper);
            ImGuiID right_middle = 0;
            ImGuiID right_bottom = 0;
            ImGui::DockBuilderSplitNode(right_lower, ImGuiDir_Down, 0.40f, &right_bottom, &right_middle);
            ImGui::DockBuilderDockWindow(kScreen, upper);
            ImGui::DockBuilderDockWindow(kVideo, upper);
            ImGui::DockBuilderDockWindow(kListing, lower);
            ImGui::DockBuilderDockWindow(kSource, lower);
            ImGui::DockBuilderDockWindow(kMemory, lower);
            ImGui::DockBuilderDockWindow(kCheckpoints, lower);
            ImGui::DockBuilderDockWindow(kHistory, lower);
            ImGui::DockBuilderDockWindow(kIoLog, lower);
            ImGui::DockBuilderDockWindow(kProfiler, lower);
            ImGui::DockBuilderDockWindow(kConsole, lower);
            ImGui::DockBuilderDockWindow(kCpu, right_upper);
            ImGui::DockBuilderDockWindow(kMachine, right_middle);
            ImGui::DockBuilderDockWindow(kAgent, right_middle);
            ImGui::DockBuilderDockWindow(kWatch, right_bottom);
            ImGui::DockBuilderFinish(dock);
        }
    }
    ImGui::DockSpace(dock, {0.0f, 0.0f}, ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::End();

    draw_screen_panel(status, monitor, skin, fonts, actions);
    draw_video(monitor, skin, fonts, scale);
    draw_listing(monitor, skin, fonts, scale, actions);
    draw_source(monitor, skin, fonts, scale, actions);
    draw_memory(monitor, skin, fonts, scale, actions);
    draw_checkpoints(monitor, skin, fonts, actions);
    draw_history(monitor, skin, fonts);
    draw_io_log(monitor, skin, fonts, actions);
    draw_profiler(monitor, skin, fonts, actions);
    draw_cpu(monitor, skin, fonts, scale, actions);
    draw_machine(monitor, skin, fonts, scale);
    draw_console(monitor, skin, fonts, actions);
    draw_agent(skin, fonts);
    draw_watch(monitor, skin, fonts, actions);
    draw_strip(status, skin, fonts, play);

    // Bench keys, while the TVC does not own the keyboard (design/ui.md "Bench")
    if (!keys_to_tvc_ && !ImGui::GetIO().WantTextInput && !play.menu_open) {
        actions.run |= held && ImGui::IsKeyPressed(ImGuiKey_F5, false);
        const bool shift = ImGui::GetIO().KeyShift;
        const bool lines = !monitor.sources().empty();
        actions.step |= held && !shift && ImGui::IsKeyPressed(ImGuiKey_F6, false);
        actions.over |= held && !shift && ImGui::IsKeyPressed(ImGuiKey_F7, false);
        actions.line_step |= held && shift && lines && ImGui::IsKeyPressed(ImGuiKey_F6, false);
        actions.line_next |= held && shift && lines && ImGui::IsKeyPressed(ImGuiKey_F7, false);
        actions.out |= held && ImGui::IsKeyPressed(ImGuiKey_F8, false);
        if (cursor_ && held && ImGui::IsKeyPressed(ImGuiKey_F4, false)) {
            actions.run_to = cursor_;
        }
        if (cursor_ && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
            actions.toggle_checkpoint = cursor_;
        }
    }
    actions.keys_to_tvc = keys_to_tvc_;
    return actions;
}

BenchView::MachineState BenchView::machine_state(const Machine& machine) {
    MachineState s;
    s.page = machine.memory().page_register();
    s.video_paging = machine.memory().video_paging();
    s.iomem = machine.memory().iomem_slot();
    s.border = machine.video().border();
    for (unsigned i = 0; i < 4; ++i) {
        s.palette[i] = machine.video().palette(i);
    }
    s.mode = machine.video().mode();
    s.crtc = machine.video().crtc().registers();
    s.pitch = machine.sound().pitch();
    s.tone = machine.sound().tone_enabled();
    s.amplitude = machine.sound().amplitude();
    s.sound_irq = machine.sound().irq_enabled();
    return s;
}

void BenchView::track_stops(remote::Monitor& monitor) {
    const Machine& machine = monitor.debugger().machine();
    const bool held = monitor.active();
    if (held && (!was_held_ || machine.ticks() != held_ticks_)) {
        held_ticks_ = machine.ticks();
        previous_ = shown_;
        have_previous_ = have_shown_;
        shown_ = monitor.debugger().registers();
        previous_machine_ = shown_machine_;
        shown_machine_ = machine_state(machine);
        have_shown_ = true;
        follow_ = true;
        reason_.clear();
        stopped_by_.clear();
        if (monitor.stop_count() != stops_seen_) {
            stops_seen_ = monitor.stop_count();
            const auto& stop = monitor.last_stop();
            if (stop && stop->why == debug::Why::Checkpoint && !stop->hits.empty()) {
                char text[48];
                std::snprintf(text, sizeof text, tr("checkpoint #%u"), stop->hits.front().checkpoint);
                reason_ = text;
                for (const debug::Hit& hit : stop->hits) {
                    stopped_by_.push_back(hit.checkpoint);
                }
            }
        }
        ++stop_serial_;
        update_watches(monitor);
    }
    was_held_ = held;
}

void BenchView::draw_run_bar(const PlayStatus& status, remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts,
                             ImVec2 min, ImVec2 size, PlayActions& play, BenchActions& actions) {
    const bool held = monitor.active();
    const float ui_px = skin.font_ui_size * status.scale;
    const float data_px = skin.font_data_size * status.scale;
    if (!begin_fixed("##runbar", min, size)) {
        ImGui::End();
        return;
    }
    DeckRow bar(skin, size.y);
    ImGui::Dummy({ImGui::GetStyle().FramePadding.x, size.y});
    const auto tip = [](const char* text) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", text);
        }
    };
    if (held) {
        actions.run |= bar.button("##run", tr("Run"), fonts.strong, ui_px, skin.label);
        tip(tr("Leave the monitor and run (F5)"));
    } else {
        actions.pause |= bar.button("##pause", tr("Pause"), fonts.strong, ui_px, skin.label);
        tip(tr("Stop at the next instruction (Pause)"));
    }
    bar.divider();
    actions.step |= bar.button("##step", tr("Step"), fonts.ui, ui_px, skin.label, held);
    tip(tr("Step into one instruction (F6)"));
    actions.over |= bar.button("##over", tr("Over"), fonts.ui, ui_px, skin.label, held);
    tip(tr("Step over CALL, RST, block repeats and HALT (F7)"));
    actions.out |= bar.button("##out", tr("Out"), fonts.ui, ui_px, skin.label, held);
    tip(tr("Run until a return leaves this stack frame (F8)"));
    if (bar.button("##to", tr("To cursor"), fonts.ui, ui_px, skin.label, held && cursor_.has_value())) {
        actions.run_to = cursor_;
    }
    tip(tr("Run to the listing's cursor line (F4)"));
    bar.divider();
    if (bar.button("##back", tr("Back 5 s"), fonts.ui, ui_px, skin.label)) {
        actions.commands.push_back("rewind 5");
    }
    tip(tr("Rewind 5 seconds of machine time; press again to go further (hold F10 to step back a second at a time)"));
    play.reset |= bar.button("##reset", tr("Reset"), fonts.ui, ui_px, skin.label);
    bar.divider();

    if (held) {
        char text[96];
        std::snprintf(text, sizeof text, tr("stopped at %s"), hex(monitor.debugger().machine().pc(), 4).c_str());
        std::string state = text;
        if (!reason_.empty()) {
            state += ": " + reason_;
        }
        bar.text(state.c_str(), fonts.data, data_px, skin.role(Role::Notice).on_case);
    } else {
        char text[32];
        if (status.speed_percent < 0.0) {
            std::snprintf(text, sizeof text, "%s", tr("running"));
        } else {
            std::snprintf(text, sizeof text, "%3.0f %%", status.speed_percent);
        }
        bar.text(text, fonts.data, data_px, skin.label);
    }

    const char* back = trc("mode", "Play");
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float back_w = fonts.strong->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, back).x + pad * 2.0f;
    actions.play |= bar.button("##play", back, fonts.strong, ui_px, skin.label, true, std::max(1.0f, size.x - back_w - pad));
    tip(tr("Back to Play (F12)"));
    ImGui::End();
}

void BenchView::draw_pages(const Machine& machine, const Skin& skin, const SkinFonts& fonts, float scale, ImVec2 min,
                           ImVec2 size) {
    if (!begin_fixed("##pages", min, size)) {
        ImGui::End();
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float ui_px = skin.font_ui_size * scale;
    const float data_px = skin.font_data_size * scale;
    const float pad = 8.0f * scale;
    const float page_h = size.y / 4.0f;
    const std::uint16_t pc = machine.pc();
    for (unsigned page = 0; page < 4; ++page) {
        const auto base = static_cast<std::uint16_t>(page * 0x4000);
        const Bank low = machine.memory().locate(base).bank;
        const Bank high = machine.memory().locate(static_cast<std::uint16_t>(base + 0x2000)).bank;
        const unsigned parts = low == high ? 1 : 2;
        for (unsigned part = 0; part < parts; ++part) {
            const auto start = static_cast<std::uint16_t>(base + part * 0x2000);
            const unsigned length = parts == 1 ? 0x4000 : 0x2000;
            const Bank bank = part == 0 ? low : high;
            const ImVec2 p0{min.x, min.y + page_h * static_cast<float>(page) + page_h * static_cast<float>(part) / static_cast<float>(parts)};
            const ImVec2 extent{size.x, page_h / static_cast<float>(parts)};
            ImGui::SetCursorScreenPos(p0);
            const std::string id = "##page" + std::to_string(start);
            if (ImGui::InvisibleButton(id.c_str(), extent)) {
                go_to(start);
                memory_bank_.reset();
                memory_top_ = start;
                edit_.reset();
            }
            if (ImGui::IsItemHovered()) {
                draw->AddRectFilled(p0, p0 + extent, to_imgui(skin.key));
                char tip[64];
                std::snprintf(tip, sizeof tip, tr("%s at %s: click to show it in Listing and Memory"), bank_name(bank),
                              hex(start, 4).c_str());
                ImGui::SetTooltip("%s", tip);
            }
            if (pc >= start && pc < start + length) {
                draw->AddRectFilled(p0, {p0.x + 3.0f * scale, p0.y + extent.y}, to_imgui(skin.role(Role::Exec).on_case));
            }
            const bool video = bank >= Bank::Vid0 && bank <= Bank::Vid3;
            const Colour colour = bank == Bank::Open ? skin.muted : video ? skin.role(Role::Vram).on_case : skin.label;
            draw->AddText(fonts.data, data_px * 0.85f, {p0.x + pad, p0.y + pad * 0.6f}, to_imgui(skin.muted), hex(start, 4).c_str());
            draw->AddText(fonts.strong, ui_px, {p0.x + pad, p0.y + pad * 0.6f + data_px}, to_imgui(colour), bank_name(bank));
            if (part > 0) {
                draw->AddLine(p0, {p0.x + extent.x, p0.y}, to_imgui(skin.rib));
            }
        }
        if (page > 0) {
            const float y = min.y + page_h * static_cast<float>(page);
            draw->AddRectFilled({min.x, y - scale}, {min.x + size.x, y + scale}, to_imgui(skin.groove));
        }
    }
    ImGui::End();
}

void BenchView::draw_screen_panel(const PlayStatus& status, remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts,
                                  BenchActions& actions) {
    const Machine& machine = monitor.debugger().machine();
    const bool held = monitor.active();
    if (focus_screen_ > 0) {
        ImGui::SetNextWindowFocus();
    }
    const std::string title = std::string(keys_to_tvc_ ? tr("Screen: keys go to the TVC") : tr("Screen")) + kScreen;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f * status.scale, 4.0f * status.scale});
    const bool open = ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    keys_to_tvc_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    focus_screen_ = std::max(0, focus_screen_ - 1);  // panels appearing later take focus for a frame
    actions.events = open;
    if (open && status.screen != ImTextureID_Invalid) {
        const debug::Debugger& debugger = monitor.debugger();
        const std::uint64_t frame = machine.frame_count();
        const auto& shown = frame > 0 ? debugger.events().frame(frame - 1) : debugger.events().frame(~std::uint64_t{0});
        const auto& progress = debugger.events().frame(frame);
        const auto wanted = [&](const debug::BeamEvent& e) {
            return e.kind == debug::BeamEvent::Out ? show_out_ : e.kind == debug::BeamEvent::In ? show_in_ : show_irq_;
        };

        // Marker toggles: the role colour when on, muted when off
        const float ui_px = skin.font_ui_size * status.scale;
        DeckRow toggles(skin, ImGui::GetTextLineHeight() + 4.0f * status.scale);
        ImGui::Dummy({0.0f, 0.0f});
        const Colour io = skin.role(Role::Io).on_case;
        const Colour irq = skin.role(Role::Notice).on_case;
        show_out_ ^= toggles.button("##out", "OUT", fonts.data, ui_px, show_out_ ? io : skin.muted);
        show_in_ ^= toggles.button("##in", "IN", fonts.data, ui_px, show_in_ ? io : skin.muted);
        show_irq_ ^= toggles.button("##irq", "IRQ", fonts.data, ui_px, show_irq_ ? irq : skin.muted);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Marks: port accesses and interrupt entries at the beam position where they happened"));
        }
        const ImVec2 count_at{ImGui::GetItemRectMax().x + 8.0f * status.scale,
                              ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - ui_px) / 2.0f};

        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x > 1.0f && size.y > 1.0f) {
            ImGui::InvisibleButton("##picture", size);
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ScreenRect rect = draw_screen(draw, status.screen, status.full_raster, min, size, skin.label);

            // Event marks: the frame on screen filled, the frame in progress hollow (design/ui.md "Screen")
            const float r = 2.5f * status.scale;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            std::string tip;
            std::optional<std::uint16_t> clicked_pc;
            unsigned outside = 0;
            unsigned marked = 0;
            const auto mark_all = [&](const std::vector<debug::BeamEvent>& events, bool filled) {
                for (const debug::BeamEvent& e : events) {
                    if (!wanted(e)) {
                        continue;
                    }
                    const float x = static_cast<float>(e.column * 8 + 4);
                    const float y = static_cast<float>(e.line) + 0.5f;
                    if (x < static_cast<float>(rect.area.x) || x >= static_cast<float>(rect.area.x + rect.area.width) ||
                        y < static_cast<float>(rect.area.y) || y >= static_cast<float>(rect.area.y + rect.area.height)) {
                        ++outside;
                        continue;
                    }
                    ++marked;
                    const ImVec2 p = rect.at(x, y);
                    const ImU32 colour = to_imgui(e.kind == debug::BeamEvent::Irq ? irq : io);
                    if (e.kind == debug::BeamEvent::Irq) {
                        const ImVec2 a{p.x, p.y - r * 1.4f}, b{p.x + r * 1.4f, p.y}, c{p.x, p.y + r * 1.4f}, d{p.x - r * 1.4f, p.y};
                        filled ? draw->AddQuadFilled(a, b, c, d, colour) : draw->AddQuad(a, b, c, d, colour);
                    } else {
                        filled ? draw->AddRectFilled({p.x - r, p.y - r}, {p.x + r, p.y + r}, colour)
                               : draw->AddRect({p.x - r, p.y - r}, {p.x + r, p.y + r}, colour);
                    }
                    if (hovered && std::abs(mouse.x - p.x) <= r * 2.0f && std::abs(mouse.y - p.y) <= r * 2.0f) {
                        if (std::count(tip.begin(), tip.end(), '\n') < 12) {
                            tip += describe_event(e) + "\n";
                        }
                        clicked_pc = e.pc;
                    }
                }
            };
            mark_all(shown, true);
            if (held) {
                mark_all(progress, false);
            }
            if (!tip.empty()) {
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (clicked && clicked_pc) {
                go_to(*clicked_pc);
            }
            char count[96];
            std::snprintf(count, sizeof count, tr("%u marked, %u outside the picture"), marked, outside);
            if (debugger.events().dropped(frame > 0 ? frame - 1 : 0) > 0) {
                std::snprintf(count, sizeof count, tr("%u marked, %u outside the picture, more than %u in the frame"), marked,
                              outside, static_cast<unsigned>(debug::EventLog::kMaxPerFrame));
            }
            draw->AddText(fonts.data, ui_px, count_at, to_imgui(skin.muted), count);

            if (held) {
                // Beam position: framebuffer row, and 8 pixels per character (core/video/video.cpp)
                const float row = static_cast<float>(machine.video().beam_line());
                const float x = static_cast<float>(machine.video().beam_column() * 8);
                const ImU32 red = to_imgui(skin.role(Role::Exec).on_case);
                if (row >= static_cast<float>(rect.area.y) && row < static_cast<float>(rect.area.y + rect.area.height)) {
                    const ImVec2 at = rect.at(x, row + 0.5f);
                    draw->AddLine({rect.min.x, at.y}, {rect.min.x + rect.size.x, at.y}, red, std::max(1.0f, status.scale));
                    if (at.x >= rect.min.x && at.x <= rect.min.x + rect.size.x) {
                        const float tick = 5.0f * status.scale;
                        draw->AddLine({at.x, at.y - tick}, {at.x, at.y + tick}, red, 2.0f * status.scale);
                    }
                } else {
                    char text[64];
                    std::snprintf(text, sizeof text, tr("beam on line %u, outside this area"), machine.video().beam_line());
                    draw->AddText({rect.min.x + 4.0f, rect.min.y + 4.0f}, red, text);
                }
            }
        }
    }
    ImGui::End();
}

void BenchView::build_rows(remote::Monitor& monitor, std::uint16_t top, std::size_t count) {
    const Machine& machine = monitor.debugger().machine();
    const debug::DisasmOptions options = debug::tvc_disasm_options(monitor.symbols(), machine);
    rows_.clear();
    std::uint16_t address = top;
    while (rows_.size() < count) {
        if (const debug::Symbol* symbol = monitor.symbols().at(address, machine)) {
            rows_.push_back({address, true, symbol->name + ":", {}});
        }
        Row row{address, false, {}, debug::disassemble(machine, address, options)};
        for (unsigned i = 0; i < row.disassembly.length && i < 4; ++i) {
            row.text += hex(machine.peek(static_cast<std::uint16_t>(address + i)), 2) + " ";
        }
        address = static_cast<std::uint16_t>(address + row.disassembly.length);
        rows_.push_back(std::move(row));
    }
}

std::uint16_t BenchView::back_up(const Machine& machine, std::uint16_t address, unsigned count) const {
    const unsigned span = count * 4 + 4;
    std::vector<std::uint16_t> starts;
    for (unsigned back = std::min<unsigned>(span, address); back >= 1; --back) {
        starts.clear();
        unsigned at = address - back;
        while (at < address) {
            starts.push_back(static_cast<std::uint16_t>(at));
            at += debug::disassemble(machine, static_cast<std::uint16_t>(at)).length;
        }
        if (at == address) {
            return starts[starts.size() > count ? starts.size() - count : 0];
        }
    }
    return static_cast<std::uint16_t>(address - std::min<unsigned>(count, address));
}

void BenchView::go_to(std::uint16_t address) {
    top_ = address;
    cursor_ = address;
    follow_ = false;
}

void BenchView::draw_listing(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale,
                             BenchActions& actions) {
    const Machine& machine = monitor.debugger().machine();
    const PanelColours panel = panel_colours(skin);
    const std::string title = std::string(tr("Listing")) + kListing;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);

    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 26.0f);
    if (ImGui::InputTextWithHint("##goto", tr("Go to: address or label"), goto_text_, sizeof goto_text_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        const debug::ParseOptions options{16, &monitor.symbols()};
        const auto [spec, error] = debug::parse_address(goto_text_, options);
        if (!spec) {
            goto_error_ = error.message;
        } else if (spec->bank) {
            goto_error_ = tr("the listing takes a CPU address");
        } else {
            go_to(static_cast<std::uint16_t>(spec->value.evaluate({machine})));
            goto_error_.clear();
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Follow PC"), &follow_);
    if (!goto_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(vec(panel.role(skin, Role::Exec)), "%s", goto_error_.c_str());
    }

    const float data_px = skin.font_data_size * scale;
    const float row_h = std::round(data_px * 1.35f);
    const float cw = fonts.data->CalcTextSizeA(data_px, FLT_MAX, 0.0f, "0").x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const auto count = static_cast<std::size_t>(std::max(1.0f, std::floor(size.y / row_h)));
    const std::uint16_t pc = machine.pc();

    build_rows(monitor, top_, count);
    if (follow_) {
        const std::size_t settled = count > 3 ? count - 2 : count;
        const bool visible = std::any_of(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(std::min(settled, rows_.size())),
                                         [&](const Row& r) { return !r.label && r.address == pc; });
        if (!visible) {
            top_ = back_up(machine, pc, static_cast<unsigned>(count / 3));
            build_rows(monitor, top_, count);
        }
    }

    if (size.x > 1.0f && size.y > 1.0f) {
        ImGui::InvisibleButton("##rows", size);
        if (ImGui::IsItemClicked()) {
            const auto index = static_cast<std::size_t>((ImGui::GetIO().MousePos.y - min.y) / row_h);
            if (index < rows_.size()) {
                if (ImGui::GetIO().MousePos.x - min.x < cw * kGutter) {
                    actions.toggle_checkpoint = rows_[index].address;
                } else {
                    cursor_ = rows_[index].address;
                }
            }
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            follow_ = false;
            if (ImGui::GetIO().MouseWheel > 0.0f) {
                top_ = back_up(machine, top_, kWheelLines);
            } else {
                for (unsigned i = 0; i < kWheelLines; ++i) {
                    top_ = static_cast<std::uint16_t>(top_ + debug::disassemble(machine, top_).length);
                }
            }
        }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, min + size, true);
    const ImU32 ink = to_imgui(panel.text);
    const ImU32 faint = with_alpha(panel.text, 0.6f);
    const ImU32 red = to_imgui(panel.role(skin, Role::Exec));
    const float text_dy = (row_h - data_px) / 2.0f;
    for (std::size_t i = 0; i < rows_.size() && i < count; ++i) {
        const Row& row = rows_[i];
        const float y = min.y + static_cast<float>(i) * row_h;
        const auto at = [&](float column) { return ImVec2{min.x + cw * column, y + text_dy}; };
        if (row.label) {
            draw->AddText(fonts.data, data_px, at(kGutter), ink, row.text.c_str());
            continue;
        }
        if (row.address == pc) {
            draw->AddRectFilled({min.x, y}, {min.x + size.x, y + row_h}, with_alpha(panel.role(skin, Role::Exec), 0.16f));
            const float mid = y + row_h / 2.0f;
            const float half = row_h * 0.28f;
            draw->AddTriangleFilled({min.x + cw * 2.0f, mid - half}, {min.x + cw * 2.0f, mid + half},
                                    {min.x + cw * 2.0f + half * 1.6f, mid}, red);
        }
        if (cursor_ == row.address) {
            draw->AddRectFilled({min.x, y}, {min.x + size.x, y + row_h}, with_alpha(panel.text, 0.09f));
            draw->AddRect({min.x, y}, {min.x + size.x, y + row_h}, with_alpha(panel.text, 0.35f));
        }
        const int checkpoint = checkpoint_state(monitor.debugger(), row.address);
        if (checkpoint != 0) {
            const ImVec2 centre{min.x + cw * 1.0f, y + row_h / 2.0f};
            if (checkpoint == 1) {
                draw->AddCircleFilled(centre, row_h * 0.26f, red);
            } else {
                draw->AddCircle(centre, row_h * 0.24f, red, 0, std::max(1.0f, scale));
            }
        }
        draw->AddText(fonts.data, data_px, at(kGutter), ink, hex(row.address, 4).c_str());
        draw->AddText(fonts.data, data_px, at(kBytesColumn), faint, row.text.c_str());
        draw->AddText(fonts.data, data_px, at(kTextColumn), ink, row.disassembly.text.c_str());
        if (!row.disassembly.comment.empty()) {
            const std::string comment = "; " + row.disassembly.comment;
            draw->AddText(fonts.data, data_px, at(kCommentColumn), faint, comment.c_str());
        }
        // The profile's share of T-states at this instruction, right-aligned
        if (const debug::Profile& profile = monitor.debugger().profile(); profile.total_ticks() > 0) {
            const std::uint64_t ticks = profile.ticks(machine.memory().locate(row.address));
            if (ticks > 0) {
                char share[16];
                const double percent = 100.0 * static_cast<double>(ticks) / static_cast<double>(profile.total_ticks());
                std::snprintf(share, sizeof share, percent < 0.05 ? "<0.1%%" : "%.1f%%", percent);
                const float w = fonts.data->CalcTextSizeA(data_px, FLT_MAX, 0.0f, share).x;
                draw->AddText(fonts.data, data_px, {min.x + size.x - w - cw, y + text_dy}, to_imgui(panel.role(skin, Role::Notice)), share);
            }
        }
    }
    draw->PopClipRect();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

void BenchView::draw_memory(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale,
                            BenchActions& actions) {
    const Machine& machine = monitor.debugger().machine();
    const PanelColours panel = panel_colours(skin);
    const auto bank_size = [&](std::optional<Bank> bank) {
        return bank ? static_cast<unsigned>(machine.memory().bank_bytes(*bank).size()) : 0x10000u;
    };
    // A layout saved before the panel existed has no place for it: start as a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Memory")) + kMemory;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    actions.heat = open;
    if (!open) {
        edit_.reset();
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    if (memory_bank_ && bank_size(memory_bank_) == 0) {
        memory_bank_.reset();  // the new preset has no such bank
    }
    const unsigned total = bank_size(memory_bank_);
    const auto select = [&](std::optional<Bank> bank, unsigned top) {
        memory_bank_ = bank;
        memory_top_ = top;
        edit_.reset();
        memory_error_.clear();
    };

    ImGui::SetNextItemWidth(ImGui::CalcTextSize("CARD0").x + ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##view", memory_bank_ ? bank_name(*memory_bank_) : tr("CPU"))) {
        if (ImGui::Selectable(tr("CPU"), !memory_bank_)) {
            select(std::nullopt, 0);
        }
        for (std::size_t b = 0; b < kBankCount; ++b) {
            const auto bank = static_cast<Bank>(b);
            if (bank != Bank::Open && bank_size(bank) > 0 && ImGui::Selectable(bank_name(bank), memory_bank_ == bank)) {
                select(bank, 0);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("CPU: what is paged in now; a bank: its bytes wherever it is paged"));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 26.0f);
    if (ImGui::InputTextWithHint("##memgoto", tr("Go to: [bank:]address"), memory_goto_, sizeof memory_goto_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        const debug::ParseOptions options{16, &monitor.symbols()};
        const auto [spec, error] = debug::parse_address(memory_goto_, options);
        const auto bank = spec && spec->bank ? spec->bank : memory_bank_;
        if (!spec) {
            memory_error_ = error.message;
        } else if (bank_size(bank) == 0) {
            memory_error_ = tr("this machine has no such bank");
        } else {
            select(bank, std::min(static_cast<unsigned>(spec->value.evaluate({machine}) & 0xFFFF), bank_size(bank) - 1));
        }
    }
    if (!memory_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(vec(panel.role(skin, Role::Exec)), "%s", memory_error_.c_str());
    }

    // Columns in data-font characters: bank tag (CPU view), address, hex in groups of 8, ASCII
    const float data_px = skin.font_data_size * scale;
    const float row_h = std::round(data_px * 1.35f);
    const float cw = fonts.data->CalcTextSizeA(data_px, FLT_MAX, 0.0f, "0").x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const float tag = memory_bank_ ? 0.0f : 6.0f;
    const float hex0 = tag + 6.0f;
    const auto hex_column = [&](unsigned c) { return hex0 + static_cast<float>(c * 3 + c / 8); };
    unsigned per_row = 8;
    for (const unsigned n : {32u, 16u}) {
        if ((hex_column(n) + 1.0f + static_cast<float>(n)) * cw <= size.x) {
            per_row = n;
            break;
        }
    }
    const float ascii0 = hex_column(per_row) + 1.0f;
    const auto rows = static_cast<unsigned>(std::max(1.0f, std::floor(size.y / row_h)));
    const unsigned max_top = total > rows * per_row ? total - rows * per_row : 0;
    memory_top_ = std::min(memory_top_ - memory_top_ % per_row, max_top - max_top % per_row);

    const auto target = [&](unsigned address) {
        return memory_bank_ ? BankAddress{*memory_bank_, static_cast<std::uint16_t>(address)}
                            : machine.memory().locate(static_cast<std::uint16_t>(address));
    };
    const auto start_edit = [&](unsigned address) {
        const Bank bank = target(address).bank;
        if (writable(bank)) {
            edit_ = address;
            edit_high_.reset();
            memory_error_.clear();
        } else {
            edit_.reset();
            char text[64];
            std::snprintf(text, sizeof text, tr("%s is read-only"), bank_name(bank));
            memory_error_ = text;
        }
    };

    if (size.x > 1.0f && size.y > 1.0f) {
        ImGui::InvisibleButton("##bytes", size);
        if (ImGui::IsItemClicked()) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const auto row = static_cast<unsigned>((mouse.y - min.y) / row_h);
            const float x = (mouse.x - min.x) / cw;
            std::optional<unsigned> column;
            for (unsigned c = 0; c < per_row; ++c) {
                if ((x >= hex_column(c) && x < hex_column(c) + 2.5f) || (x >= ascii0 + static_cast<float>(c) && x < ascii0 + static_cast<float>(c) + 1.0f)) {
                    column = c;
                }
            }
            if (column && memory_top_ + row * per_row + *column < total) {
                start_edit(memory_top_ + row * per_row + *column);
            } else {
                edit_.reset();
            }
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            const unsigned step = kWheelLines * per_row;
            memory_top_ = ImGui::GetIO().MouseWheel > 0.0f ? (memory_top_ > step ? memory_top_ - step : 0)
                                                            : std::min(memory_top_ + step, max_top);
        }
    }
    if (edit_ && ImGui::IsWindowFocused()) {
        memory_keys(machine, per_row, actions);
        if (edit_ && !writable(target(*edit_).bank)) {
            start_edit(*edit_);
        }
        if (edit_ && *edit_ < memory_top_) {
            memory_top_ = *edit_ - *edit_ % per_row;
        } else if (edit_ && *edit_ >= memory_top_ + rows * per_row) {
            memory_top_ = *edit_ - *edit_ % per_row - (rows - 1) * per_row;
        }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, min + size, true);
    const ImU32 ink = to_imgui(panel.text);
    const ImU32 faint = with_alpha(panel.text, 0.6f);
    const float text_dy = (row_h - data_px) / 2.0f;
    const auto now = static_cast<std::uint32_t>(machine.frame_count() + 1);
    const debug::HeatMap& heat = monitor.debugger().heat();
    const bool heat_on = monitor.debugger().heat_enabled();
    const std::span<const std::uint8_t> bank_bytes =
        memory_bank_ ? machine.memory().bank_bytes(*memory_bank_) : std::span<const std::uint8_t>{};
    for (unsigned r = 0; r < rows; ++r) {
        const unsigned row_start = memory_top_ + r * per_row;
        if (row_start >= total) {
            break;
        }
        const float y = min.y + static_cast<float>(r) * row_h;
        const auto at = [&](float column) { return ImVec2{min.x + cw * column, y + text_dy}; };
        if (!memory_bank_) {
            draw->AddText(fonts.data, data_px, at(0.0f), faint, bank_name(target(row_start).bank));
        }
        draw->AddText(fonts.data, data_px, at(tag), ink, hex(row_start, 4).c_str());
        std::string ascii;
        for (unsigned c = 0; c < per_row; ++c) {
            const unsigned address = row_start + c;
            const std::uint8_t value = memory_bank_ ? bank_bytes[address] : machine.peek(static_cast<std::uint16_t>(address));
            const ImVec2 cell0{min.x + cw * (hex_column(c) - 0.25f), y};
            const ImVec2 cell1{min.x + cw * (hex_column(c) + 2.25f), y + row_h};
            if (heat_on) {
                // The latest access kind tints the byte, fading over kHeatFrames
                const BankAddress where = target(address);
                std::uint32_t best = 0;
                Role role = Role::Exec;
                const std::pair<debug::HeatMap::Kind, Role> kinds[] = {
                    {debug::HeatMap::Exec, Role::Exec}, {debug::HeatMap::Read, Role::Read}, {debug::HeatMap::Write, Role::Write}};
                for (const auto& [kind, kind_role] : kinds) {
                    const std::uint32_t stamp = heat.at(where, kind);
                    if (stamp > best) {
                        best = stamp;
                        role = kind_role;
                    }
                }
                if (best != 0 && now - best < kHeatFrames) {
                    const float fade = 1.0f - static_cast<float>(now - best) / static_cast<float>(kHeatFrames);
                    draw->AddRectFilled(cell0, cell1, with_alpha(panel.role(skin, role), 0.55f * fade));
                }
            }
            std::string text = hex(value, 2);
            if (edit_ == address) {
                draw->AddRect(cell0, cell1, ink, 0.0f, std::max(1.0f, scale));
                const ImVec2 a0{min.x + cw * (ascii0 + static_cast<float>(c)), y};
                draw->AddRect(a0, a0 + ImVec2{cw, row_h}, ink);
                if (edit_high_) {
                    text = hex(*edit_high_, 1) + "_";
                }
            }
            draw->AddText(fonts.data, data_px, at(hex_column(c)), ink, text.c_str());
            ascii.push_back(value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '.');
        }
        draw->AddText(fonts.data, data_px, at(ascii0), faint, ascii.c_str());
    }
    draw->PopClipRect();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

void BenchView::memory_keys(const Machine& machine, unsigned per_row, BenchActions& actions) {
    const unsigned total = memory_bank_ ? static_cast<unsigned>(machine.memory().bank_bytes(*memory_bank_).size()) : 0x10000u;
    std::optional<unsigned> nibble;
    for (unsigned i = 0; i < 10; ++i) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + i)) || ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i))) {
            nibble = i;
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i))) {
            nibble = 10 + i;
        }
    }
    const auto move = [&](long delta) {
        const long to = static_cast<long>(*edit_) + delta;
        if (to >= 0 && to < static_cast<long>(total)) {
            edit_ = static_cast<unsigned>(to);
        }
        edit_high_.reset();
    };
    if (nibble && !edit_high_) {
        edit_high_ = nibble;
    } else if (nibble) {
        // One byte per monitor `>` line, as typed into the monitor (ROM banks never reach here)
        const unsigned value = *edit_high_ << 4 | *nibble;
        const std::string where = memory_bank_ ? std::string(bank_name(*memory_bank_)) + ":" + hex(*edit_, 4) : hex(*edit_, 4);
        actions.commands.push_back("> " + where + " " + hex(value, 2));
        if (*edit_ + 1 < total) {
            move(1);
        } else {
            edit_.reset();
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        edit_.reset();
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        move(-1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        move(1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        move(-static_cast<long>(per_row));
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        move(static_cast<long>(per_row));
    }
    if (!edit_) {
        edit_high_.reset();
    }
}

void BenchView::draw_cpu(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale, BenchActions& actions) {
    const Machine& machine = monitor.debugger().machine();
    const bool held = monitor.active();
    const std::string title = std::string(tr("CPU")) + kCpu;
    const bool open = ImGui::Begin(title.c_str());
    if (!open) {
        ImGui::End();
        return;
    }
    const Z80Registers now = monitor.debugger().registers();
    const bool compare = held && have_previous_;
    const Colour notice = skin.role(Role::Notice).on_case;
    ImGui::PushFont(fonts.data, skin.font_data_size);

    struct Field {
        const char* name;
        unsigned value;
        unsigned before;
        int digits;
    };
    const Z80Registers& p = previous_;
    const Field fields[] = {
        {"PC", now.pc, p.pc, 4},   {"SP", now.sp, p.sp, 4},     {"AF", now.af, p.af, 4}, {"AF'", now.af2, p.af2, 4},
        {"BC", now.bc, p.bc, 4},   {"BC'", now.bc2, p.bc2, 4},  {"DE", now.de, p.de, 4}, {"DE'", now.de2, p.de2, 4},
        {"HL", now.hl, p.hl, 4},   {"HL'", now.hl2, p.hl2, 4},  {"IX", now.ix, p.ix, 4}, {"IY", now.iy, p.iy, 4},
        {"I", now.i, p.i, 2},      {"R", now.r, p.r, 2},        {"IM", now.im, p.im, 1}, {"IFF", (now.iff1 ? 2u : 0u) | (now.iff2 ? 1u : 0u),
                                                                                          (p.iff1 ? 2u : 0u) | (p.iff2 ? 1u : 0u), 0},
    };
    if (ImGui::BeginTable("##registers", 4, ImGuiTableFlags_SizingFixedFit)) {
        for (const Field& f : fields) {
            ImGui::TableNextColumn();
            ImGui::TextColored(vec(skin.muted), "%s", f.name);
            ImGui::TableNextColumn();
            const Colour colour = compare && f.value != f.before ? notice : skin.label;
            if (f.digits == 0) {
                ImGui::TextColored(vec(colour), "%u %u", (f.value >> 1) & 1u, f.value & 1u);
                continue;
            }
            // Double-click edits a register while the machine is stopped; the monitor's `r` sets it
            std::string name = f.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (held && edit_register_ == name) {
                ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * (static_cast<float>(f.digits) + 1.5f));
                if (register_focus_) {
                    ImGui::SetKeyboardFocusHere();
                    register_focus_ = false;
                }
                constexpr ImGuiInputTextFlags kFlags =
                    ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
                if (ImGui::InputText("##register", register_text_, static_cast<std::size_t>(f.digits) + 1, kFlags)) {
                    if (register_text_[0] != 0) {
                        actions.commands.push_back("r " + name + "=" + register_text_);
                    }
                    edit_register_.clear();
                } else if (ImGui::IsItemDeactivated()) {
                    edit_register_.clear();
                }
                continue;
            }
            ImGui::TextColored(vec(colour), "%s", hex(f.value, f.digits).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", held ? tr("Double-click to edit") : tr("Stop the machine to edit registers"));
                if (held && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    edit_register_ = name;
                    std::snprintf(register_text_, sizeof register_text_, "%s", hex(f.value, f.digits).c_str());
                    register_focus_ = true;
                }
            }
        }
        ImGui::EndTable();
    }
    if (!held) {
        edit_register_.clear();
    }

    // Flags as eight cells, S at the left as in F's bit order
    ImGui::Spacing();
    const float cw = ImGui::CalcTextSize("0").x;
    const float cell = cw * 2.2f;
    const float cell_h = ImGui::GetTextLineHeight() + 4.0f * scale;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    static constexpr char kFlags[] = "SZYHXPNC";
    for (int i = 0; i < 8; ++i) {
        const unsigned bit = 7u - static_cast<unsigned>(i);
        const bool set = (now.af >> bit) & 1u;
        const bool changed = compare && ((now.af ^ p.af) >> bit & 1u);
        const ImVec2 c0{origin.x + static_cast<float>(i) * (cell + scale), origin.y};
        ImGui::SetCursorScreenPos(c0);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##flag", {cell, cell_h}) && held) {
            actions.commands.push_back("r f=" + hex((now.af & 0xFFu) ^ (1u << bit), 2));
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", held ? tr("Click to toggle the flag") : tr("Stop the machine to edit registers"));
        }
        if (set) {
            draw->AddRectFilled(c0, c0 + ImVec2{cell, cell_h}, to_imgui(skin.key));
        } else {
            draw->AddRect(c0, c0 + ImVec2{cell, cell_h}, to_imgui(skin.rib));
        }
        const char letter[2] = {kFlags[i], 0};
        const Colour colour = changed ? notice : set ? skin.label : skin.muted;
        draw->AddText({c0.x + (cell - cw) / 2.0f, c0.y + 2.0f * scale}, to_imgui(colour), letter);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({8.0f * (cell + scale), cell_h});

    ImGui::Spacing();
    if (machine.halted()) {
        ImGui::TextColored(vec(notice), "%s", tr("HALT"));
        ImGui::SameLine();
    }
    if (machine.irq_pending()) {
        ImGui::TextColored(vec(notice), "%s", tr("IRQ pending"));
    }
    ImGui::NewLine();
    ImGui::Separator();
    if (ImGui::BeginTable("##timing", 2, ImGuiTableFlags_SizingFixedFit)) {
        const auto line = [&](const char* name, unsigned long long value) {
            ImGui::TableNextColumn();
            ImGui::TextColored(vec(skin.muted), "%s", name);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", value);
        };
        line(tr("frame"), machine.frame_count());
        line(tr("line"), machine.video().beam_line());
        line(tr("column"), machine.video().beam_column());
        line(tr("T-states"), machine.ticks() / kTicksPerTState);
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::End();
}

void BenchView::console_output(std::string_view text) {
    if (text.empty()) {
        return;
    }
    console_partial_ += text;
    std::size_t eol;
    while ((eol = console_partial_.find('\n')) != std::string::npos) {
        console_.push_back(console_partial_.substr(0, eol));
        console_partial_.erase(0, eol + 1);
    }
    while (console_.size() > kConsoleLines) {
        console_.pop_front();
    }
    console_scroll_ = true;
}

void BenchView::console_command(std::string_view prompt, std::string_view line) {
    console_output((console_partial_.empty() ? std::string(prompt) : std::string()) + std::string(line) + "\n");
}

void BenchView::draw_console(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions) {
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Console")) + kConsole;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const float input_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(panel.background));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, vec(panel.background));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, vec(panel.text, 0.25f));
    if (ImGui::BeginChild("##scrollback", {0.0f, -input_h})) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(console_.size()));
        const ImVec4 echo = vec(panel.text, 0.6f);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& line = console_[static_cast<std::size_t>(i)];
                if (line.rfind("(C:$", 0) == 0) {
                    ImGui::TextColored(echo, "%s", line.c_str());  // a prompt with the command typed after it
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
        }
        if (console_scroll_) {
            ImGui::SetScrollHereY(1.0f);
            console_scroll_ = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(3);

    // The input line, with the monitor's prompt or "running" before it
    const std::string prompt = monitor.active() ? monitor.prompt() : std::string(tr("(running) "));
    ImGui::TextColored(vec(panel.text, 0.6f), "%s", prompt.c_str());
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (console_focus_) {
        ImGui::SetKeyboardFocusHere();
        console_focus_ = false;
    }
    const auto history = [](ImGuiInputTextCallbackData* data) -> int {
        auto* self = static_cast<BenchView*>(data->UserData);
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->history_.empty()) {
            return 0;
        }
        if (data->EventKey == ImGuiKey_UpArrow && self->history_pos_ > 0) {
            --self->history_pos_;
        } else if (data->EventKey == ImGuiKey_DownArrow && self->history_pos_ < self->history_.size()) {
            ++self->history_pos_;
        }
        data->DeleteChars(0, data->BufTextLen);
        if (self->history_pos_ < self->history_.size()) {
            data->InsertChars(0, self->history_[self->history_pos_].c_str());
        }
        return 0;
    };
    constexpr ImGuiInputTextFlags kInput = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputTextWithHint("##command", tr("monitor command (help lists them)"), console_input_, sizeof console_input_,
                                 kInput, history, this)) {
        const std::string line = console_input_;
        if (!line.empty()) {
            actions.console_lines.push_back(line);
            if (history_.empty() || history_.back() != line) {
                history_.push_back(line);
            }
        }
        history_pos_ = history_.size();
        console_input_[0] = 0;
        console_focus_ = true;  // Enter leaves the field; the next command goes in the same place
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

void BenchView::draw_agent(const Skin& skin, const SkinFonts& fonts) {
    if (const ImGuiWindow* machine = ImGui::FindWindowByName(kMachine); machine && machine->DockId != 0) {
        ImGui::SetNextWindowDockID(machine->DockId, ImGuiCond_FirstUseEver);  // an older layout: a tab beside the Machine card
    }
    const std::string title = std::string(tr("Agent")) + kAgent;
    if (!ImGui::Begin(title.c_str(), nullptr, kSecondaryTab)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(fonts.data, skin.font_data_size);
    if (!mcp_) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(vec(skin.muted), "%s", tr("No MCP server: switch it on in Settings, Remote, or start freeTVC with --mcp-http, and an agent's tool calls show here live."));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::End();
        return;
    }
    const std::deque<remote::McpCall>& calls = mcp_->calls();
    const std::uint64_t latest = calls.empty() ? 0 : calls.back().number;
    char header[160];
    std::snprintf(header, sizeof header, tr("%s · %llu calls"), mcp_endpoint_.c_str(), static_cast<unsigned long long>(latest));
    ImGui::TextColored(vec(skin.muted), "%s", header);
    ImGui::Separator();
    if (ImGui::BeginChild("##calls")) {
        const Colour notice = skin.role(Role::Notice).on_case;
        for (const remote::McpCall& c : calls) {
            ImGui::TextColored(vec(skin.muted), "#%llu", static_cast<unsigned long long>(c.number));
            ImGui::SameLine();
            ImGui::TextColored(vec(c.error ? notice : skin.label), "%s", c.tool.c_str());
            ImGui::SameLine();
            ImGui::TextColored(vec(skin.muted), "%.1f ms  frame %llu", c.milliseconds, static_cast<unsigned long long>(c.frame));
            ImGui::Indent();
            ImGui::PushTextWrapPos(0.0f);
            if (c.arguments != "{}") {
                ImGui::TextColored(vec(skin.muted), "%s", c.arguments.c_str());
            }
            if (!c.result.empty()) {
                ImGui::TextColored(vec(c.error ? notice : skin.label), "%s", c.result.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Unindent();
        }
        if (latest != agent_seen_) {
            agent_seen_ = latest;
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::End();
}

void BenchView::draw_machine(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale) {
    const Machine& machine = monitor.debugger().machine();
    if (const ImGuiWindow* cpu = ImGui::FindWindowByName(kCpu); cpu && cpu->DockId != 0) {
        ImGui::SetNextWindowDockID(cpu->DockId, ImGuiCond_FirstUseEver);  // an older layout: a tab beside the CPU card
    }
    const std::string title = std::string(tr("Machine")) + kMachine;
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        return;
    }
    const MachineState now = machine_state(machine);
    const MachineState& before = previous_machine_;
    const bool compare = monitor.active() && have_previous_;
    const Colour notice = skin.role(Role::Notice).on_case;
    const auto colour_of = [&](bool changed) { return vec(compare && changed ? notice : skin.label); };
    const auto name = [&](const char* text) {
        ImGui::TableNextColumn();
        ImGui::TextColored(vec(skin.muted), "%s", text);
        ImGui::TableNextColumn();
    };
    const auto swatch = [&](std::uint8_t igrb, bool changed) {
        const std::uint32_t rgb = kRgbPalette[igrb < Framebuffer::kColours ? igrb : 0];
        const float side = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({side, side});
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p, p + ImVec2{side, side}, IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF));
        draw->AddRect(p, p + ImVec2{side, side}, to_imgui(compare && changed ? notice : skin.rib));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("IGRB %X", igrb);
        }
    };
    ImGui::PushFont(fonts.data, skin.font_data_size);

    ImGui::SeparatorText(tr("Paging"));
    if (ImGui::BeginTable("##paging", 2, ImGuiTableFlags_SizingFixedFit)) {
        name(tr("port 02h"));
        ImGui::TextColored(colour_of(now.page != before.page), "%02X", now.page);
        name(tr("video 0Fh"));
        ImGui::TextColored(colour_of(now.video_paging != before.video_paging), "%02X  %s %u", now.video_paging, tr("shows VID"),
                           machine.memory().displayed_bank());
        name(tr("IOMEM"));
        ImGui::TextColored(colour_of(now.iomem != before.iomem), "%s %u", tr("slot"), now.iomem);
        ImGui::EndTable();
    }

    ImGui::SeparatorText(tr("Video"));
    if (ImGui::BeginTable("##video", 2, ImGuiTableFlags_SizingFixedFit)) {
        static const char* const kModes[] = {N_("2-colour"), N_("4-colour"), N_("16-colour"), N_("16-colour")};
        name(tr("mode"));
        ImGui::TextColored(colour_of(now.mode != before.mode), "%s", tr(kModes[now.mode & 3]));
        name(tr("border"));
        swatch(now.border, now.border != before.border);
        name(tr("palette"));
        for (unsigned i = 0; i < 4; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            swatch(now.palette[i], now.palette[i] != before.palette[i]);
        }
        ImGui::EndTable();
    }
    const unsigned selected = machine.video().crtc().selected();
    if (ImGui::BeginTable("##crtc", 6, ImGuiTableFlags_SizingFixedFit)) {
        for (unsigned r = 0; r < Mc6845::kRegisters; ++r) {
            ImGui::TableNextColumn();
            ImGui::TextColored(vec(r == selected ? skin.label : skin.muted), "R%u", r);
            ImGui::TableNextColumn();
            ImGui::TextColored(colour_of(now.crtc[r] != before.crtc[r]), "%02X", now.crtc[r]);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(tr("Sound"));
    if (ImGui::BeginTable("##sound", 2, ImGuiTableFlags_SizingFixedFit)) {
        name(tr("pitch"));
        if (now.pitch >= 0x0FFF) {
            ImGui::TextColored(colour_of(now.pitch != before.pitch), "%03X  %s", now.pitch, tr("stopped"));
        } else {
            ImGui::TextColored(colour_of(now.pitch != before.pitch), "%03X  %.1f Hz", now.pitch, 195312.5 / (4096.0 - now.pitch));
        }
        name(tr("tone"));
        ImGui::TextColored(colour_of(now.tone != before.tone), "%s", now.tone ? tr("on") : tr("off"));
        name(tr("amplitude"));
        ImGui::TextColored(colour_of(now.amplitude != before.amplitude), "%u", now.amplitude);
        name(tr("sound IRQ"));
        ImGui::TextColored(colour_of(now.sound_irq != before.sound_irq), "%s", now.sound_irq ? tr("on") : tr("off"));
        name(tr("IRQ line"));
        ImGui::TextColored(vec(machine.irq_pending() ? notice : skin.label), "%s", machine.irq_pending() ? tr("pending") : tr("clear"));
        ImGui::EndTable();
    }

    // Keyboard matrix: rows 0-9, columns 0-7; held keys lit like a pressed key
    ImGui::SeparatorText(tr("Keyboard"));
    const float cell = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float label_w = ImGui::CalcTextSize("0").x * 1.6f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (unsigned row = 0; row < 10; ++row) {
        const float y = origin.y + static_cast<float>(row) * (cell + scale);
        draw->AddText({origin.x, y}, to_imgui(skin.muted), std::to_string(row).c_str());
        for (unsigned column = 0; column < 8; ++column) {
            const ImVec2 c0{origin.x + label_w + static_cast<float>(column) * (cell + scale), y};
            if (machine.key_down(row, column)) {
                draw->AddRectFilled(c0, c0 + ImVec2{cell, cell}, to_imgui(skin.label));
            } else {
                draw->AddRect(c0, c0 + ImVec2{cell, cell}, to_imgui(skin.rib));
            }
        }
    }
    ImGui::Dummy({label_w + 8.0f * (cell + scale), 10.0f * (cell + scale)});
    ImGui::PopFont();
    ImGui::End();
}


void BenchView::show_in_listing(std::uint16_t address) {
    go_to(address);
    ImGui::SetWindowFocus(kListing);
}

void BenchView::show_in_memory(std::optional<Bank> bank, unsigned address) {
    memory_bank_ = bank;
    memory_top_ = address;
    edit_.reset();
    memory_error_.clear();
    ImGui::SetWindowFocus(kMemory);
}

void BenchView::command_result(std::string_view line, std::string_view output) {
    const auto sent = std::find(checkpoint_lines_.begin(), checkpoint_lines_.end(), line);
    if (sent == checkpoint_lines_.end()) {
        return;
    }
    checkpoint_lines_.erase(sent);
    if (output.starts_with("error: ")) {
        checkpoint_error_ = std::string(line) + ": " + std::string(output.substr(7, output.find('\n') - 7));
        return;
    }
    checkpoint_error_.clear();
    if (line == add_line_) {
        add_where_[0] = 0;
        add_condition_[0] = 0;
        add_line_.clear();
    }
}

void BenchView::draw_checkpoints(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions) {
    const debug::Debugger& debugger = monitor.debugger();
    const Machine& machine = debugger.machine();
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Checkpoints")) + kCheckpoints;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab);
    ImGui::PopStyleColor();
    if (!open) {
        edit_checkpoint_ = 0;
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const auto send = [&](std::string line) {
        checkpoint_lines_.push_back(line);
        actions.commands.push_back(std::move(line));
    };
    const float cw = ImGui::CalcTextSize("0").x;

    // The add form: one monitor line, `break` or `trace`
    const AddKind& kind = kAddKinds[add_kind_];
    float kind_w = 0.0f;
    for (const AddKind& k : kAddKinds) {
        kind_w = std::max(kind_w, ImGui::CalcTextSize(tr(k.label)).x);
    }
    ImGui::SetNextItemWidth(kind_w + ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##kind", tr(kind.label))) {
        for (int i = 0; i < static_cast<int>(std::size(kAddKinds)); ++i) {
            if (ImGui::Selectable(tr(kAddKinds[i].label), i == add_kind_)) {
                add_kind_ = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // The two fields share what the check box and the button leave
    const ImGuiStyle& style = ImGui::GetStyle();
    const float tail = ImGui::GetFrameHeight() + ImGui::CalcTextSize(tr("Log only")).x + ImGui::CalcTextSize(tr("Add")).x +
                       style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x + style.ItemSpacing.x * 3.0f;
    const float fields = std::max(cw * 16.0f, ImGui::GetContentRegionAvail().x - tail);
    const float where_w = std::min(cw * 22.0f, fields * 0.45f);
    ImGui::SetNextItemWidth(where_w);
    bool submit = ImGui::InputTextWithHint("##where", tr(kind.hint), add_where_, sizeof add_where_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fields - where_w - style.ItemSpacing.x);
    submit |= ImGui::InputTextWithHint("##if", tr("if: condition (optional)"), add_condition_, sizeof add_condition_,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Log only"), &add_trace_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Count and log the hits without stopping (the monitor's trace command)"));
    }
    ImGui::SameLine();
    submit |= ImGui::Button(tr("Add"));
    if (submit) {
        std::string line = std::string(add_trace_ ? "trace " : "break ") + kind.words;
        if (const std::string where = trimmed(add_where_); !where.empty()) {
            line += " " + where;
        }
        if (const std::string condition = trimmed(add_condition_); !condition.empty()) {
            line += " if " + condition;
        }
        add_line_ = line;
        send(line);
    }
    if (!checkpoint_error_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(vec(panel.role(skin, Role::Exec)), "%s", checkpoint_error_.c_str());
        ImGui::PopTextWrapPos();
    }

    const std::vector<debug::Checkpoint>& list = debugger.checkpoints();
    if (list.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("No checkpoints. Add one here, click the Listing's gutter (F9), or type break in the Console."));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PopStyleColor(pushed);
        ImGui::End();
        return;
    }
    char count[48];
    std::snprintf(count, sizeof count, tr("%zu checkpoint(s)"), list.size());
    ImGui::TextDisabled("%s", count);
    ImGui::SameLine();
    const double now = ImGui::GetTime();
    const bool armed = delete_all_armed_ >= 0.0 && now - delete_all_armed_ < kDeleteAllSeconds;
    if (ImGui::SmallButton(armed ? tr("Click again to delete all") : tr("Delete all"))) {
        if (armed) {
            send("delete");
            delete_all_armed_ = -1.0;
        } else {
            delete_all_armed_ = now;
        }
    }

    const bool held = monitor.active();
    const Colour notice = panel.role(skin, Role::Notice);
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_RowBg;
    const int table_colours = push_table_colours(panel);
    if (ImGui::BeginTable("##checkpoints", 8, kTable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("On"));
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn(tr("Kind"));
        ImGui::TableSetupColumn(tr("Place"));
        ImGui::TableSetupColumn(tr("Condition"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Ignore"));
        ImGui::TableSetupColumn(tr("Hits"));
        ImGui::TableSetupColumn("##delete");
        ImGui::TableHeadersRow();
        for (const debug::Checkpoint& c : list) {
            ImGui::PushID(static_cast<int>(c.id));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool on = c.enabled;
            if (ImGui::Checkbox("##on", &on)) {
                send((on ? "enable " : "disable ") + std::to_string(c.id));
            }

            ImGui::TableNextColumn();
            const bool stopped = held && std::find(stopped_by_.begin(), stopped_by_.end(), c.id) != stopped_by_.end();
            ImGui::TextColored(vec(stopped ? notice : panel.text, c.enabled ? 1.0f : 0.55f), "#%u", c.id);
            if (stopped && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("This checkpoint stopped the machine"));
            }

            ImGui::TableNextColumn();
            ImGui::TextColored(vec(access_colour(skin, panel, c.access), c.enabled ? 1.0f : 0.55f), "%s", access_names(c.access).c_str());
            if (!c.stop || c.temporary) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", !c.stop ? tr("log") : tr("once"));
            }

            // The place: a click shows it in the Listing (code) or the Memory panel (data)
            ImGui::TableNextColumn();
            const std::string place = checkpoint_place(c);
            const bool code = (c.access & (kAccessExec | kAccessIrq)) && !any_handler(c);
            const bool data = (c.access & (kAccessRead | kAccessWrite)) != 0;
            if (code || data) {
                if (ImGui::Selectable(place.c_str(), false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(place.c_str()))) {
                    if (!c.offsets || !c.bank) {
                        code ? show_in_listing(c.start) : show_in_memory(std::nullopt, c.start);
                    } else if (const auto at = cpu_address(machine, *c.bank, c.start); code && at) {
                        show_in_listing(*at);
                    } else {
                        show_in_memory(*c.bank, c.start);
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", code ? tr("Click to show it in the Listing") : tr("Click to show it in the Memory panel"));
                }
            } else {
                ImGui::TextUnformatted(place.c_str());
            }

            // Condition and ignore count: double-click to edit, Enter sends it to the monitor
            const auto editable = [&](int field, const std::string& shown, const std::string& initial, bool digits,
                                      const auto& make_line) {
                if (edit_checkpoint_ == c.id && edit_field_ == field) {
                    if (checkpoint_focus_) {
                        ImGui::SetKeyboardFocusHere();
                        checkpoint_focus_ = false;
                    }
                    ImGui::SetNextItemWidth(digits ? cw * 8.0f : -FLT_MIN);
                    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                                      (digits ? ImGuiInputTextFlags_CharsDecimal : 0);
                    if (ImGui::InputText("##edit", checkpoint_text_, sizeof checkpoint_text_, flags)) {
                        send(make_line(trimmed(checkpoint_text_)));
                        edit_checkpoint_ = 0;
                    } else if (ImGui::IsItemDeactivated()) {
                        edit_checkpoint_ = 0;
                    }
                    return;
                }
                // The whole cell takes the double-click, not only its text
                if (shown.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                ImGui::Selectable(((shown.empty() ? std::string("-") : shown) + "##field" + std::to_string(field)).c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick);
                if (shown.empty()) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tr("Double-click to edit"));
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        edit_checkpoint_ = c.id;
                        edit_field_ = field;
                        std::snprintf(checkpoint_text_, sizeof checkpoint_text_, "%s", initial.c_str());
                        checkpoint_focus_ = true;
                    }
                }
            };
            ImGui::TableNextColumn();
            editable(1, c.condition_text, c.condition_text, false, [&](const std::string& text) {
                return "condition " + std::to_string(c.id) + (text.empty() ? "" : " if " + text);
            });
            ImGui::TableNextColumn();
            const std::string ignore = c.ignore ? std::to_string(c.ignore) : std::string();
            editable(2, ignore, std::to_string(c.ignore), true, [&](const std::string& text) {
                return "ignore " + std::to_string(c.id) + " #" + (text.empty() ? "0" : text);
            });

            // Hits, with the latest one kept in the hit log on hover
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(c.hits));
            if (ImGui::IsItemHovered()) {
                const std::vector<debug::Hit> hits = debugger.hit_log().last(debugger.hit_log().size());
                const auto last = std::find_if(hits.rbegin(), hits.rend(), [&](const debug::Hit& h) { return h.checkpoint == c.id; });
                if (last != hits.rend()) {
                    ImGui::SetTooltip("%s %s", tr("Last hit:"), hit_text(*last).c_str());
                } else {
                    ImGui::SetTooltip("%s", c.hits ? tr("Hits counted; the latest is no longer in the hit log") : tr("No hits yet"));
                }
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("\xC3\x97")) {
                send("delete " + std::to_string(c.id));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("Delete"));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(table_colours);
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

void BenchView::update_watches(remote::Monitor& monitor) {
    const debug::ParseOptions options{16, &monitor.symbols()};
    const Machine& machine = monitor.debugger().machine();
    for (Watch& w : watches_) {
        const std::string value = evaluate_watch(w.text, static_cast<unsigned>(w.format), machine, options).text;
        if (w.stop_seen != 0) {
            w.before_stop = w.at_stop;
        }
        w.at_stop = value;
        w.stop_seen = stop_serial_;
    }
}

void BenchView::draw_watch(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions) {
    const Machine& machine = monitor.debugger().machine();
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Machine card
    if (const ImGuiWindow* card = ImGui::FindWindowByName(kMachine); card && card->DockId != 0) {
        ImGui::SetNextWindowDockID(card->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Watch")) + kWatch;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab);
    ImGui::PopStyleColor();
    if (!open) {
        edit_watch_.reset();
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const debug::ParseOptions options{16, &monitor.symbols()};
    const bool held = monitor.active();
    // A right-click for the row menu takes the keyboard from the Screen panel too, so F5-F9 work after it
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::SetWindowFocus();
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##add", tr("Add: HL, {SP}, [0B00], VID0:0000 or a label"), watch_input_, sizeof watch_input_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (const std::string text = trimmed(watch_input_); !text.empty()) {
            const auto [spec, error] = debug::parse_address(text, options);
            Watch w;
            w.text = text;
            w.format = spec && spec->bank ? WatchFormat::Bytes : WatchFormat::Value;
            w.at_stop = evaluate_watch(w.text, static_cast<unsigned>(w.format), machine, options).text;
            w.stop_seen = stop_serial_;
            watches_.push_back(std::move(w));
            ImGui::MarkIniSettingsDirty();
        }
        watch_input_[0] = 0;
    }
    if (watches_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("Expressions are evaluated every frame; a value that changed between the last two stops is yellow. "
                                     "Right-click a row for its format, Memory and a checkpoint on writes."));
        ImGui::PopTextWrapPos();
    }

    std::optional<std::size_t> remove;
    const Colour notice = panel.role(skin, Role::Notice);
    const Colour bad = panel.role(skin, Role::Exec);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(panel.background, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, vec(panel.text, 0.10f));
    if (!watches_.empty() && ImGui::BeginTable("##watches", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                                                                   ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("##expression");
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        for (std::size_t i = 0; i < watches_.size(); ++i) {
            Watch& w = watches_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const unsigned format = static_cast<unsigned>(w.format);
            const WatchResult r = evaluate_watch(w.text, format, machine, options);
            if (edit_watch_ == i) {
                if (watch_focus_) {
                    ImGui::SetKeyboardFocusHere();
                    watch_focus_ = false;
                }
                ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 20.0f);
                if (ImGui::InputText("##edit", watch_text_, sizeof watch_text_,
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (const std::string text = trimmed(watch_text_); !text.empty()) {
                        w.text = text;
                        w.before_stop.reset();
                        w.at_stop = evaluate_watch(w.text, format, machine, options).text;
                        ImGui::MarkIniSettingsDirty();
                    } else {
                        remove = i;
                    }
                    edit_watch_.reset();
                } else if (ImGui::IsItemDeactivated()) {
                    edit_watch_.reset();
                }
            } else {
                ImGui::Selectable(w.text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    edit_watch_ = i;
                    std::snprintf(watch_text_, sizeof watch_text_, "%s", w.text.c_str());
                    watch_focus_ = true;
                }
                if (ImGui::BeginPopupContextItem("##menu")) {
                    for (unsigned f = 0; f < std::size(kWatchFormats); ++f) {
                        if (ImGui::MenuItem(tr(kWatchFormats[f]), nullptr, format == f)) {
                            w.format = static_cast<WatchFormat>(f);
                            w.before_stop.reset();
                            w.at_stop = evaluate_watch(w.text, f, machine, options).text;
                            ImGui::MarkIniSettingsDirty();
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(tr("Show in Memory"), nullptr, false, r.address.has_value())) {
                        show_in_memory(r.bank, *r.address);
                    }
                    if (ImGui::MenuItem(tr("Show in Listing"), nullptr, false, r.address.has_value() && !r.bank)) {
                        show_in_listing(static_cast<std::uint16_t>(*r.address));
                    }
                    // Stop on writes to the bytes shown; a value names no memory of its own
                    if (ImGui::MenuItem(tr("Stop on write"), nullptr, false, r.address.has_value() && w.format != WatchFormat::Value)) {
                        const std::string prefix = r.bank ? std::string(bank_name(*r.bank)) + ":" : std::string();
                        std::string line = "break store " + prefix + hex(*r.address, 4);
                        if (r.length > 1) {
                            line += " " + prefix + hex(*r.address + r.length - 1, 4);
                        }
                        actions.commands.push_back(std::move(line));
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(tr("Remove"))) {
                        remove = i;
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::TableNextColumn();
            const bool changed = held && w.before_stop && *w.before_stop != w.at_stop;
            ImGui::TextColored(vec(r.error ? bad : changed ? notice : panel.text), "%s", r.text.c_str());
            if (w.format != WatchFormat::Value) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr(kWatchFormats[format]));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(2);
    if (remove) {
        watches_.erase(watches_.begin() + static_cast<std::ptrdiff_t>(*remove));
        edit_watch_.reset();
        ImGui::MarkIniSettingsDirty();
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

void BenchView::install_settings() {
    ImGuiSettingsHandler handler;
    handler.TypeName = "Bench";
    handler.TypeHash = ImHashStr("Bench");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
        return std::strcmp(name, "Watch") == 0 ? reinterpret_cast<void*>(1) : nullptr;
    };
    // "format expression", one watch per line
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* h, void*, const char* line) {
        auto* view = static_cast<BenchView*>(h->UserData);
        const std::string text = trimmed(line);
        const std::size_t space = text.find(' ');
        if (space == std::string::npos) {
            return;
        }
        const std::string name = text.substr(0, space);
        for (unsigned f = 0; f < std::size(kWatchFormats); ++f) {
            if (name == kWatchFormats[f]) {
                Watch w;
                w.text = trimmed(text.c_str() + space);
                w.format = static_cast<WatchFormat>(f);
                view->watches_.push_back(std::move(w));
            }
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out) {
        const auto* view = static_cast<const BenchView*>(h->UserData);
        if (view->watches_.empty()) {
            return;
        }
        out->append("[Bench][Watch]\n");
        for (const Watch& w : view->watches_) {
            out->appendf("%s %s\n", kWatchFormats[static_cast<unsigned>(w.format)], w.text.c_str());
        }
        out->append("\n");
    };
    ImGui::AddSettingsHandler(&handler);
}

void BenchView::draw_history(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts) {
    const debug::Debugger& debugger = monitor.debugger();
    const Machine& machine = debugger.machine();
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("History")) + kHistory;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const auto finish = [&] {
        ImGui::PopFont();
        ImGui::PopStyleColor(pushed);
        ImGui::End();
    };
    const debug::Ring<debug::TraceEntry>& trace = debugger.trace();
    if (trace.capacity() == 0) {
        ImGui::TextDisabled("%s", tr("The instruction history is off: type chis on in the Console."));
        finish();
        return;
    }
    if (!monitor.active()) {
        char text[128];
        std::snprintf(text, sizeof text, tr("Pause to read the history: the last %zu instructions before the stop."), trace.capacity());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
        finish();
        return;
    }

    const debug::DisasmOptions options = debug::tvc_disasm_options(monitor.symbols(), machine);
    const char* hint = tr("Filter: address, bank or instruction");
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(hint).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::InputTextWithHint("##filter", hint, history_filter_, sizeof history_filter_);
    // The newest entry is the instruction at the PC: logged as it starts, not run yet
    const std::uint64_t ticks = machine.ticks();
    const std::size_t done = trace.size() > 0 && trace.at(trace.size() - 1).ticks == ticks ? trace.size() - 1 : trace.size();

    // Rows: every entry, or those whose address, bank or text holds the filter
    const std::string filter = lower_text(trimmed(history_filter_));
    if (history_built_ != stop_serial_ || filter != history_filter_used_ || trace.pushed() != history_pushed_) {
        history_scroll_ = true;  // a new stop or filter starts at the newest instruction
        history_built_ = stop_serial_;
        history_filter_used_ = filter;
        history_pushed_ = trace.pushed();
        history_rows_.clear();
        for (std::size_t i = 0; i < done; ++i) {
            const debug::TraceEntry& e = trace.at(i);
            if (!filter.empty()) {
                const debug::Disassembly d = debug::disassemble(e.bytes, e.regs.pc, options);
                const std::string text = lower_text(hex(e.regs.pc, 4) + " " + bank_name(e.where.bank) + " " + d.text + " " + d.comment);
                if (text.find(filter) == std::string::npos) {
                    continue;
                }
            }
            history_rows_.push_back(static_cast<std::uint32_t>(i));
        }
    }
    ImGui::SameLine();
    char count[96];
    std::snprintf(count, sizeof count, tr("%zu of %zu instructions"), history_rows_.size(), done);
    ImGui::TextDisabled("%s", count);

    const Z80Registers now = debugger.registers();
    const Colour notice = panel.role(skin, Role::Notice);
    const int table_colours = push_table_colours(panel);
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##history", 5, kTable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("T before"));
        ImGui::TableSetupColumn(tr("Bank"));
        ImGui::TableSetupColumn("PC");
        ImGui::TableSetupColumn(tr("Instruction"));
        ImGui::TableSetupColumn(tr("Changed"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        if (history_scroll_) {
            ImGui::SetScrollY(1.0e9f);  // clamped to the end: the newest instruction
            history_scroll_ = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(history_rows_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const std::size_t i = history_rows_[static_cast<std::size_t>(row)];
                const debug::TraceEntry& e = trace.at(i);
                const bool last = i + 1 == done;
                const Z80Registers& after = last ? now : trace.at(i + 1).regs;
                const bool interrupt_follows = i + 1 < trace.size() && trace.at(i + 1).interrupt;
                ImGui::PushID(row);
                ImGui::TableNextRow();
                if (history_selected_ == e.ticks) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(vec(panel.text, 0.16f)));
                }
                ImGui::TableNextColumn();
                const std::string before = "-" + std::to_string((ticks - e.ticks) / kTicksPerTState);
                if (ImGui::Selectable(before.c_str(), history_selected_ == e.ticks, ImGuiSelectableFlags_SpanAllColumns)) {
                    history_selected_ = e.ticks;
                    show_in_listing(e.regs.pc);
                }
                if (ImGui::IsItemHovered()) {
                    char tip[256];
                    std::snprintf(tip, sizeof tip,
                                  tr("Click to show it in the Listing.\nAfter it: AF %04X BC %04X DE %04X HL %04X\nIX %04X IY %04X SP %04X"),
                                  after.af, after.bc, after.de, after.hl, after.ix, after.iy, after.sp);
                    ImGui::SetTooltip("%s", tip);
                }
                if (e.interrupt) {
                    ImGui::SameLine();
                    ImGui::TextColored(vec(notice), "%s", tr("irq"));
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", bank_name(e.where.bank));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hex(e.regs.pc, 4).c_str());
                ImGui::TableNextColumn();
                const debug::Disassembly d = debug::disassemble(e.bytes, e.regs.pc, options);
                ImGui::TextUnformatted(d.text.c_str());
                if (!d.comment.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("; %s", d.comment.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(vec(notice), "%s", register_changes(e.regs, after, interrupt_follows).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(table_colours);
    finish();
}

void BenchView::draw_io_log(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions) {
    debug::Debugger& debugger = monitor.debugger();
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("I/O log")) + kIoLog;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab);
    ImGui::PopStyleColor();
    actions.events |= open;
    if (!open) {
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const Colour io = panel.role(skin, Role::Io);
    const Colour notice = panel.role(skin, Role::Notice);

    ImGui::Checkbox("OUT", &io_out_);
    ImGui::SameLine();
    ImGui::Checkbox("IN", &io_in_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Off at start: the keyboard scan reads port 58h ten times a frame"));
    }
    ImGui::SameLine();
    ImGui::Checkbox("IRQ", &io_irq_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0").x * 18.0f);
    ImGui::InputTextWithHint("##ports", tr("Ports: 70 71 60-63"), io_ports_, sizeof io_ports_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Follow"), &io_follow_);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) {
        debugger.clear_io_log();
        io_selected_.reset();
    }
    std::array<bool, 256> ports{};
    bool port_filter = false;
    if (!parse_ports(io_ports_, ports, port_filter)) {
        ImGui::TextColored(vec(panel.role(skin, Role::Exec)), "%s", tr("ports are hex numbers or ranges, e.g. 70 71 60-63"));
        port_filter = false;
    }

    const debug::Ring<debug::IoEvent>& log = debugger.io_log();
    io_rows_.clear();
    for (std::size_t i = 0; i < log.size(); ++i) {
        const debug::BeamEvent& e = log.at(i).event;
        const bool kind = e.kind == debug::BeamEvent::Out ? io_out_ : e.kind == debug::BeamEvent::In ? io_in_ : io_irq_;
        if (kind && (e.kind == debug::BeamEvent::Irq || !port_filter || ports[e.port])) {
            io_rows_.push_back(static_cast<std::uint32_t>(i));
        }
    }
    char count[96];
    std::snprintf(count, sizeof count, tr("%zu shown of %zu kept"), io_rows_.size(), log.size());
    ImGui::TextDisabled("%s", count);
    if (log.size() == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("(events are recorded while this panel or the Screen panel is visible)"));
    }

    const int table_colours = push_table_colours(panel);
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##iolog", 8, kTable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Frame"));
        ImGui::TableSetupColumn(tr("Line"));
        ImGui::TableSetupColumn(tr("Col"));
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn(tr("Port"));
        ImGui::TableSetupColumn(tr("Value"));
        ImGui::TableSetupColumn(tr("What"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("PC");
        ImGui::TableHeadersRow();
        // Scrolling up stops following; new events scroll to the end while following
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel > 0.0f) {
            io_follow_ = false;
        }
        if (io_follow_ && log.pushed() != io_seen_) {
            io_seen_ = log.pushed();
            ImGui::SetScrollY(1.0e9f);
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(io_rows_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const debug::IoEvent& item = log.at(io_rows_[static_cast<std::size_t>(row)]);
                const debug::BeamEvent& e = item.event;
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(item.frame).c_str(), io_selected_ == item.ticks, ImGuiSelectableFlags_SpanAllColumns)) {
                    io_selected_ = item.ticks;
                    show_in_listing(e.pc);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s", describe_event(e).c_str(), tr("Click to show the instruction in the Listing"));
                }
                ImGui::TableNextColumn();
                ImGui::Text("%u", e.line);
                ImGui::TableNextColumn();
                ImGui::Text("%u", e.column);
                ImGui::TableNextColumn();
                if (e.kind == debug::BeamEvent::Irq) {
                    ImGui::TextColored(vec(notice), "IRQ");
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(tr("interrupt, handler"));
                } else {
                    ImGui::TextColored(vec(io), "%s", e.kind == debug::BeamEvent::Out ? "OUT" : "IN");
                    ImGui::TableNextColumn();
                    ImGui::Text("%02Xh", e.port);
                    ImGui::TableNextColumn();
                    ImGui::Text("%02X", e.value);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(port_name(e).c_str());
                }
                ImGui::TableNextColumn();
                ImGui::Text("%04X", e.pc);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(table_colours);
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

void BenchView::draw_video(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale) {
    const Machine& machine = monitor.debugger().machine();
    // A layout saved before the panel existed: a tab beside the Screen
    if (const ImGuiWindow* screen = ImGui::FindWindowByName(kScreen); screen && screen->DockId != 0) {
        ImGui::SetNextWindowDockID(screen->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Video")) + kVideo;
    if (!ImGui::Begin(title.c_str(), nullptr, kSecondaryTab | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) ||
        vram_texture_ == ImTextureID_Invalid || !display_) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const MemoryMap& memory = machine.memory();
    const auto vid = [](unsigned n) { return static_cast<Bank>(static_cast<unsigned>(Bank::Vid0) + n); };
    const auto exists = [&](unsigned n) { return n < 4 && !memory.bank_bytes(vid(n)).empty(); };
    if (video_bank_ && !exists(*video_bank_)) {
        video_bank_.reset();  // the new preset has fewer video banks
    }
    const unsigned shown_bank = memory.displayed_bank();
    const unsigned bank = video_bank_.value_or(shown_bank);
    const Video& video = machine.video();
    const std::uint8_t mode = video_mode_.value_or(video.mode());
    if (video_palette_live_) {
        video_palette_ = video.palettes();
    }

    // Controls: bank, mode, palette, screen area
    char label[64];
    std::snprintf(label, sizeof label, video_bank_ ? "VID%u" : tr("shown: VID%u"), bank);
    char widest[64];
    std::snprintf(widest, sizeof widest, tr("shown: VID%u"), 0u);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(widest).x + ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##bank", label)) {
        std::snprintf(label, sizeof label, tr("shown: VID%u"), shown_bank);
        if (ImGui::Selectable(label, !video_bank_)) {
            video_bank_.reset();
        }
        for (unsigned n = 0; n < 4; ++n) {
            std::snprintf(label, sizeof label, "VID%u", n);
            if (exists(n) && ImGui::Selectable(label, video_bank_ == n)) {
                video_bank_ = n;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("shown: the bank port 0Fh puts on the screen now; a VIDn: that bank whatever is shown"));
    }
    ImGui::SameLine();
    std::snprintf(label, sizeof label, video_mode_ ? "%s" : tr("machine: %s"), tr(kModeNames[mode & 3]));
    std::snprintf(widest, sizeof widest, tr("machine: %s"), tr(kModeNames[2]));
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(widest).x + ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##mode", label)) {
        std::snprintf(label, sizeof label, tr("machine: %s"), tr(kModeNames[video.mode() & 3]));
        if (ImGui::Selectable(label, !video_mode_)) {
            video_mode_.reset();
        }
        for (std::uint8_t m = 0; m < 3; ++m) {
            if (ImGui::Selectable(tr(kModeNames[m]), video_mode_ == m)) {
                video_mode_ = m;
            }
        }
        ImGui::EndCombo();
    }
    // The four palette entries; 16-colour bytes carry their own colours
    const bool paletted = (mode & 3) < 2;
    const float side = ImGui::GetFrameHeight();
    ImGui::BeginDisabled(!paletted);
    for (unsigned i = 0; i < ((mode & 3) == 0 ? 2u : 4u); ++i) {
        ImGui::SameLine(0.0f, i == 0 ? -1.0f : 2.0f * scale);
        const std::uint32_t rgb = kRgbPalette[video_palette_[i] & 15];
        const ImVec4 colour{static_cast<float>(rgb >> 16 & 0xFF) / 255.0f, static_cast<float>(rgb >> 8 & 0xFF) / 255.0f,
                            static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
        const std::string id = "##pal" + std::to_string(i);
        if (ImGui::ColorButton(id.c_str(), colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, {side, side})) {
            ImGui::OpenPopup(("##pick" + std::to_string(i)).c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(tr("palette %u = IGRB %X; click to choose another"), i, video_palette_[i]);
        }
        if (ImGui::BeginPopup(("##pick" + std::to_string(i)).c_str())) {
            for (std::uint8_t c = 0; c < 16; ++c) {
                const std::uint32_t v = kRgbPalette[c];
                const ImVec4 option{static_cast<float>(v >> 16 & 0xFF) / 255.0f, static_cast<float>(v >> 8 & 0xFF) / 255.0f,
                                    static_cast<float>(v & 0xFF) / 255.0f, 1.0f};
                if (c % 8 != 0) {
                    ImGui::SameLine(0.0f, 2.0f * scale);
                }
                const std::string pick = "##igrb" + std::to_string(c);
                if (ImGui::ColorButton(pick.c_str(), option, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, {side, side})) {
                    video_palette_[i] = c;
                    video_palette_live_ = false;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("IGRB %X", c);
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("machine palette"), &video_palette_live_);
    ImGui::EndDisabled();

    // What the 6845 shows: start, R1 characters x R6 rows x R9 + 1 lines, the cursor
    const Mc6845& crtc = video.crtc();
    const auto& r = crtc.registers();
    const std::bitset<Video::kBankSize> shown = displayed_vram(r);
    char geometry[160];
    std::snprintf(geometry, sizeof geometry, tr("start %04Xh   %u x %u characters, %u lines each   cursor %04Xh   %zu bytes on screen"),
                  crtc.start_address(), r[1], r[6] & 0x7Fu, (r[9] & 0x1Fu) + 1, crtc.cursor_address(), shown.count());
    ImGui::Checkbox(tr("dim off-screen"), &video_dim_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Darken the bytes the 6845 does not show with its current registers"));
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(vec(skin.muted), "%s", geometry);

    // The picture: line y is the 64 bytes from y * 64, at the PAL dot aspect of the Screen panel
    const std::span<const std::uint8_t> bytes = memory.bank_bytes(vid(bank));
    vram_pixels_.resize(static_cast<std::size_t>(kVramWidth) * kVramLines);
    std::array<std::uint8_t, 8> px{};
    for (unsigned a = 0; a < Video::kBankSize && a < bytes.size(); ++a) {
        Video::decode(bytes[a], mode, video_palette_, px.data());
        std::uint32_t* out = &vram_pixels_[(a >> 6) * kVramWidth + (a & 63) * 8];
        const bool dim = video_dim_ && !shown[a];
        for (unsigned k = 0; k < 8; ++k) {
            const std::uint32_t rgb = kRgbPalette[px[k]];
            out[k] = 0xFF000000u | (dim ? (rgb >> 2) & 0x3F3F3Fu : rgb);
        }
    }
    display_->update_texture(vram_texture_, vram_pixels_.data(), kVramWidth * 4);

    const float dot = 4.0f / 3.0f * static_cast<float>(kTvArea.height) / static_cast<float>(kTvArea.width);
    const float aspect = static_cast<float>(kVramWidth) * dot / static_cast<float>(kVramLines);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x > 1.0f && avail.y > 1.0f) {
        const float w = std::min(avail.x, avail.y * aspect);
        const ImVec2 size{std::floor(w), std::floor(w / aspect)};
        const ImVec2 min = ImGui::GetCursorScreenPos() + ImVec2{std::floor((avail.x - size.x) / 2.0f), 0.0f};
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##vram", size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddImage(vram_texture_, min, min + size);
        draw->AddRect(min - ImVec2{1.0f, 1.0f}, min + size + ImVec2{1.0f, 1.0f}, to_imgui(skin.rib));
        const float sx = size.x / static_cast<float>(kVramWidth);
        const float sy = size.y / static_cast<float>(kVramLines);
        const auto byte_rect = [&](unsigned address, unsigned lines) {
            const ImVec2 p0 = min + ImVec2{static_cast<float>((address & 63) * 8) * sx, static_cast<float>(address >> 6) * sy};
            return std::pair<ImVec2, ImVec2>{p0, p0 + ImVec2{8.0f * sx, static_cast<float>(lines) * sy}};
        };
        const float thick = std::max(1.0f, scale);
        if (bank == shown_bank) {
            // The cursor's character cell (lines R10-R11 of its row) unless R10 hides it
            const std::uint16_t cursor = crtc.cursor_address();
            if (((r[10] >> 5) & 3) != 1) {
                const unsigned first = std::min(r[10] & 0x1Fu, 3u);
                const unsigned last = std::min(r[11] & 0x1Fu, 3u);
                const auto [p0, p1] = byte_rect(Video::vram_address(cursor, static_cast<std::uint8_t>(first)), last >= first ? last - first + 1 : 1);
                draw->AddRect(p0 - ImVec2{thick, thick}, p1 + ImVec2{thick, thick}, to_imgui(skin.role(Role::Notice).on_case), 0.0f, thick);
            }
            // While stopped and inside the displayed area: the byte the beam reads next
            if (monitor.active() && crtc.row() < (r[6] & 0x7Fu) && crtc.column() < r[1]) {
                const auto [p0, p1] = byte_rect(Video::vram_address(crtc.ma(), static_cast<std::uint8_t>(crtc.raster())), 1);
                const ImU32 red = to_imgui(skin.role(Role::Exec).on_case);
                draw->AddLine({min.x, p0.y + sy / 2.0f}, {min.x + size.x, p0.y + sy / 2.0f}, with_alpha(skin.role(Role::Exec).on_case, 0.45f), thick);
                draw->AddRect(p0 - ImVec2{thick, thick}, p1 + ImVec2{thick, thick}, red, 0.0f, thick);
            }
        }
        // Hover: the byte, its dot and where the screen shows it; a click opens it in the Memory panel
        if (ImGui::IsItemHovered()) {
            const ImVec2 at = ImGui::GetIO().MousePos - min;
            const auto x = std::min(static_cast<unsigned>(at.x / sx), static_cast<unsigned>(kVramWidth - 1));
            const auto y = std::min(static_cast<unsigned>(at.y / sy), static_cast<unsigned>(kVramLines - 1));
            const unsigned address = y * 64 + x / 8;
            const std::uint8_t value = address < bytes.size() ? bytes[address] : 0;
            Video::decode(value, mode, video_palette_, px.data());
            const auto [p0, p1] = byte_rect(address, 1);
            draw->AddRect(p0, p1, to_imgui(skin.label));
            std::string tip = std::string(bank_name(vid(bank))) + ":" + hex(address, 4) + "h = " + hex(value, 2) + "   " + tr("line") + " " +
                              std::to_string(y) + ", " + tr("byte") + " " + std::to_string(x / 8) + ", " + tr("dot") + " " +
                              std::to_string(x % 8) + " = IGRB " + hex(px[x % 8], 1);
            if (shown[address] && r[1] != 0) {
                const unsigned ma = (address & 0x3F) | ((address >> 8) & 0x3F) << 6;
                const unsigned from = (ma - crtc.start_address()) & 0x3FFF;
                char where[96];
                std::snprintf(where, sizeof where, tr("\non screen: character row %u, column %u, line %u of the row"), from / r[1], from % r[1],
                              (address >> 6) & 3);
                tip += where;
            } else {
                tip += std::string("\n") + tr("not on screen with these CRTC values");
            }
            tip += std::string("\n") + tr("Click to show it in the Memory panel");
            ImGui::SetTooltip("%s", tip.c_str());
            if (ImGui::IsItemClicked()) {
                show_in_memory(vid(bank), address & ~15u);
            }
        }
    }
    ImGui::PopFont();
    ImGui::End();
}

void BenchView::draw_profiler(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions) {
    const debug::Debugger& debugger = monitor.debugger();
    const Machine& machine = debugger.machine();
    const debug::Profile& profile = debugger.profile();
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Profiler")) + kProfiler;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);

    // Recording goes through the monitor's profile command
    const bool on = debugger.profile_enabled();
    if (ImGui::Button(on ? tr("Stop") : tr("Record"))) {
        actions.commands.emplace_back(on ? "profile off" : "profile on");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", on ? tr("Stop recording; the counts are kept") : tr("Time every instruction from now on (costs about 11 %)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) {
        actions.commands.emplace_back("profile clear");
    }
    ImGui::SameLine();
    char status[160];
    std::snprintf(status, sizeof status, tr("%s   %llu instructions, %llu T = %.2f s of TVC time"), on ? tr("recording") : tr("stopped"),
                  static_cast<unsigned long long>(profile.instructions()), static_cast<unsigned long long>(profile.total_ticks() / kTicksPerTState),
                  static_cast<double>(profile.total_ticks() / kTicksPerTState) / kTStatesPerSecond);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(vec(on ? panel.role(skin, Role::Exec) : panel.text, on ? 1.0f : 0.55f), "%s", status);
    if (ImGui::RadioButton(tr("Routines"), profile_by_routine_)) {
        profile_by_routine_ = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Instructions"), !profile_by_routine_)) {
        profile_by_routine_ = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", profile_by_routine_ ? tr("routines are CALL/RST targets and interrupt entries; click one for the Listing")
                                                  : tr("the 500 busiest; click one for the Listing"));
    if (profile.total_ticks() == 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("No profile yet. Record times every instruction: exclusive T-states are a routine's own "
                                     "instructions, inclusive ones add the routines it calls."));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PopStyleColor(pushed);
        ImGui::End();
        return;
    }
    const double total = static_cast<double>(profile.total_ticks());
    const std::uint64_t now = machine.ticks();
    const Colour exec = panel.role(skin, Role::Exec);
    const auto open_place = [&](BankAddress where) {
        if (const auto cpu = cpu_address(machine, where.bank, where.offset)) {
            show_in_listing(*cpu);
        } else if (where.bank != Bank::Open) {
            show_in_memory(where.bank, where.offset);
        }
    };
    const int table_colours = push_table_colours(panel);
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerH;
    if (profile_by_routine_) {
        if (ImGui::BeginTable("##routines", 6, kTable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Routine"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupColumn(tr("Calls"), ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn(tr("Exclusive T"), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn(tr("Excl."), ImGuiTableColumnFlags_NoSort, ImGui::CalcTextSize("100.0%").x * 1.6f);
            ImGui::TableSetupColumn(tr("Inclusive T"), ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn(tr("Incl."), ImGuiTableColumnFlags_NoSort, ImGui::CalcTextSize("100.0%").x * 1.6f);
            ImGui::TableHeadersRow();
            const auto& routines = profile.routines();
            profile_rows_.resize(routines.size());
            for (std::size_t i = 0; i < routines.size(); ++i) {
                profile_rows_[i] = i;
            }
            int column = 2;
            bool ascending = false;
            if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
                column = specs->Specs[0].ColumnIndex;
                ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            const auto key = [&](std::size_t i) {
                return column == 1 ? routines[i].calls : column == 4 ? profile.inclusive(i, now) : routines[i].exclusive;
            };
            std::stable_sort(profile_rows_.begin(), profile_rows_.end(),
                             [&](std::size_t a, std::size_t b) { return ascending ? key(a) < key(b) : key(a) > key(b); });
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(profile_rows_.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const std::size_t i = profile_rows_[static_cast<std::size_t>(row)];
                    const debug::RoutineStats& r = routines[i];
                    const std::uint64_t inclusive = profile.inclusive(i, now);
                    ImGui::PushID(row);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string name = r.entry.bank == Bank::Open ? tr("(outside any call)") : place_name(machine, monitor.symbols(), r.entry);
                    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                        open_place(r.entry);
                    }
                    if (r.entry.bank == Bank::Open && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", tr("Time before the first call seen, or after returning from calls that started before recording"));
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(r.calls));
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(r.exclusive / kTicksPerTState));
                    ImGui::TableNextColumn();
                    share_cell(static_cast<double>(r.exclusive) / total, exec);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(inclusive / kTicksPerTState));
                    ImGui::TableNextColumn();
                    share_cell(static_cast<double>(inclusive) / total, exec);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    } else {
        // The busiest addresses: rebuilt when stopped and changed, or twice a second while recording
        const bool due = profile.instructions() != hot_built_ && (!on || machine.frame_count() >= hot_frame_ + 25);
        if (due) {
            hot_built_ = profile.instructions();
            hot_frame_ = machine.frame_count();
            hot_.clear();
            for (std::size_t b = 0; b < kBankCount; ++b) {
                for (std::uint16_t o = 0; o < debug::Profile::kBankSize; ++o) {
                    const BankAddress where{static_cast<Bank>(b), o};
                    if (const std::uint64_t t = profile.ticks(where); t > 0) {
                        hot_.push_back({where, profile.count(where), t});
                    }
                }
            }
            const std::size_t keep = std::min(hot_.size(), kHotInstructions);
            std::partial_sort(hot_.begin(), hot_.begin() + static_cast<std::ptrdiff_t>(keep), hot_.end(),
                              [](const HotInstruction& a, const HotInstruction& b) { return a.ticks > b.ticks; });
            hot_.resize(keep);
        }
        if (ImGui::BeginTable("##instructions", 5, kTable & ~ImGuiTableFlags_Sortable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Address"));
            ImGui::TableSetupColumn(tr("Instruction"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("Count"));
            ImGui::TableSetupColumn("T");
            ImGui::TableSetupColumn(tr("Share"), ImGuiTableColumnFlags_None, ImGui::CalcTextSize("100.0%").x * 1.6f);
            ImGui::TableHeadersRow();
            const debug::DisasmOptions options = debug::tvc_disasm_options(monitor.symbols(), machine);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(hot_.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const HotInstruction& h = hot_[static_cast<std::size_t>(row)];
                    ImGui::PushID(row);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(place_name(machine, monitor.symbols(), h.where).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                        open_place(h.where);
                    }
                    ImGui::TableNextColumn();
                    const std::span<const std::uint8_t> bytes = machine.memory().bank_bytes(h.where.bank);
                    std::array<std::uint8_t, 4> code{};
                    for (unsigned k = 0; k < code.size() && h.where.offset + k < bytes.size(); ++k) {
                        code[k] = bytes[h.where.offset + k];
                    }
                    const auto cpu = cpu_address(machine, h.where.bank, h.where.offset);
                    ImGui::TextUnformatted(debug::disassemble(code, cpu.value_or(h.where.offset), options).text.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", h.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(h.ticks / kTicksPerTState));
                    ImGui::TableNextColumn();
                    share_cell(static_cast<double>(h.ticks) / total, exec);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::PopStyleColor(table_colours);
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}

const std::vector<std::string>& BenchView::source_text(const std::string& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    const long long stamp = error ? -1 : static_cast<long long>(time.time_since_epoch().count());
    SourceText& text = source_texts_[path];
    if (text.stamp != stamp || (stamp != -1 && text.lines.empty())) {
        text.stamp = stamp;
        text.lines.clear();
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::replace(line.begin(), line.end(), '\t', ' ');
            text.lines.push_back(std::move(line));
        }
    }
    return text.lines;
}

void BenchView::draw_source(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale, BenchActions& actions) {
    const debug::Debugger& debugger = monitor.debugger();
    const Machine& machine = debugger.machine();
    const debug::SourceMap& sources = monitor.sources();
    const PanelColours panel = panel_colours(skin);
    // A layout saved before the panel existed: a tab beside the Listing
    if (const ImGuiWindow* listing = ImGui::FindWindowByName(kListing); listing && listing->DockId != 0) {
        ImGui::SetNextWindowDockID(listing->DockId, ImGuiCond_FirstUseEver);
    }
    const std::string title = std::string(tr("Source")) + kSource;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(panel.background));
    const bool open = ImGui::Begin(title.c_str(), nullptr, kSecondaryTab | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::End();
        return;
    }
    const int pushed = push_panel(skin);
    ImGui::PushFont(fonts.data, skin.font_data_size);
    const bool held = monitor.active();

    actions.load_symbols |= ImGui::Button(tr("Load symbols..."));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Labels and source lines: sjasmplus --sld, --lst, --sym; SDCC .noi, .cdb, .map; z88dk .map; name = value"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!held || sources.empty());
    actions.line_step |= ImGui::Button(tr("Line in"));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr("Run until the PC is on another source line (Shift+F6, the monitor's lstep)"));
    }
    ImGui::SameLine();
    actions.line_next |= ImGui::Button(tr("Line over"));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr("The same with calls run through (Shift+F7, the monitor's lnext)"));
    }
    ImGui::EndDisabled();

    const std::vector<std::string>& files = sources.files();
    if (sources.empty() || files.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("No source lines. Load an assembler's or compiler's debug file: sjasmplus --sld or --lst, "
                                     "SDCC .noi, .cdb or .map, z88dk .map (or ll in the Console)."));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PopStyleColor(pushed);
        ImGui::End();
        return;
    }

    // At a new stop, show the PC's file and line
    const std::uint16_t pc = machine.pc();
    const std::optional<debug::SourceLine> at_pc = sources.at(pc);
    if (held && source_stop_seen_ != stop_serial_) {
        source_stop_seen_ = stop_serial_;
        if (source_follow_ && at_pc) {
            source_file_ = at_pc->file;
            source_scroll_ = true;
        }
    }
    if (!source_file_ || *source_file_ >= files.size()) {
        source_file_ = at_pc ? at_pc->file : 0;
    }
    const std::uint32_t file = *source_file_;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::CalcTextSize("main.c").x * 3.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tr("Follow PC")).x - ImGui::GetFrameHeight() * 2.0f));
    if (ImGui::BeginCombo("##file", std::filesystem::path(files[file]).filename().string().c_str())) {
        for (std::uint32_t f = 0; f < files.size(); ++f) {
            if (ImGui::Selectable((std::filesystem::path(files[f]).filename().string() + "##" + std::to_string(f)).c_str(), f == file)) {
                source_file_ = f;
                source_follow_ = false;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", files[f].c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Follow PC"), &source_follow_);

    const std::vector<std::string>& text = source_text(files[file]);
    if (text.empty()) {
        ImGui::TextColored(vec(panel.role(skin, Role::Exec)), "%s %s", tr("cannot read"), files[file].c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor(pushed);
        ImGui::End();
        return;
    }
    const std::vector<std::uint32_t> code = sources.code_lines(file);
    const std::optional<std::uint32_t> pc_line = at_pc && at_pc->file == file ? std::optional(at_pc->line) : std::nullopt;
    const float cw = ImGui::CalcTextSize("0").x;
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const ImU32 red = to_imgui(panel.role(skin, Role::Exec));
    const int digits = static_cast<int>(std::to_string(text.size()).size());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(panel.background, 0.0f));  // the panel shows through
    const bool lines_open = ImGui::BeginChild("##lines", {0.0f, 0.0f}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    if (lines_open) {
        if (source_scroll_ && pc_line) {
            ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(*pc_line) - 1.0f) * row_h - ImGui::GetWindowHeight() / 3.0f));
            source_scroll_ = false;
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(text.size()), row_h);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto number = static_cast<std::uint32_t>(row + 1);
                const bool has_code = std::binary_search(code.begin(), code.end(), number);
                const std::optional<std::uint16_t> address = has_code ? sources.address_of({file, number}) : std::nullopt;
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float width = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetWindowWidth());
                if (pc_line == number) {
                    draw->AddRectFilled(p, {p.x + width, p.y + row_h}, with_alpha(panel.role(skin, Role::Exec), 0.16f));
                    const float mid = p.y + row_h / 2.0f;
                    const float half = row_h * 0.28f;
                    draw->AddTriangleFilled({p.x + cw * 2.0f, mid - half}, {p.x + cw * 2.0f, mid + half}, {p.x + cw * 2.0f + half * 1.6f, mid}, red);
                }
                if (source_cursor_ == number) {
                    draw->AddRect(p, {p.x + width, p.y + row_h}, with_alpha(panel.text, 0.35f));
                }
                if (address) {
                    if (const int state = checkpoint_state(debugger, *address); state != 0) {
                        const ImVec2 centre{p.x + cw, p.y + row_h / 2.0f};
                        state == 1 ? draw->AddCircleFilled(centre, row_h * 0.26f, red) : draw->AddCircle(centre, row_h * 0.24f, red, 0, std::max(1.0f, scale));
                    }
                }
                ImGui::PushID(row);
                // The gutter toggles a checkpoint at the line's first instruction; the text selects, a double-click opens the Listing
                ImGui::InvisibleButton("##gutter", {cw * 3.0f, row_h});
                if (ImGui::IsItemClicked() && address) {
                    actions.toggle_checkpoint = address;
                }
                if (ImGui::IsItemHovered() && address) {
                    ImGui::SetTooltip(tr("Click: checkpoint at %04X"), *address);
                }
                ImGui::SameLine(0.0f, 0.0f);
                char label[16];
                std::snprintf(label, sizeof label, "%*u ", digits, number);
                const std::string line = std::string(label) + text[static_cast<std::size_t>(row)];
                ImGui::PushStyleColor(ImGuiCol_Text, vec(panel.text, has_code ? 1.0f : 0.55f));
                if (ImGui::Selectable(line.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, row_h})) {
                    source_cursor_ = number;
                    if (address && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        show_in_listing(*address);
                    }
                }
                ImGui::PopStyleColor();
                if (address && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(tr("%04X; double-click to show it in the Listing"), *address);
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::End();
}
}  // namespace freetvc::sdl
