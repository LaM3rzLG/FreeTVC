// SPDX-License-Identifier: GPL-3.0-or-later
// Bench mode: run bar, page column and docked panels over the debug API (design/ui.md "Bench").
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "debug/disassembler.hpp"
#include "frontend/sdl/display.hpp"
#include "frontend/sdl/play_view.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"
#include "remote/mcp.hpp"
#include "remote/monitor.hpp"


namespace freetvc::sdl {

// Every command goes through the text monitor in the host (design/ui.md "Bench": control).
struct BenchActions {
    bool run = false;
    bool pause = false;
    bool step = false;
    bool over = false;
    bool out = false;
    bool play = false;  // back to Play
    std::optional<std::uint16_t> run_to;
    std::optional<std::uint16_t> toggle_checkpoint;  // CPU address; tied to the bank paged in there
    bool line_step = false;                          // lstep / lnext through the monitor
    bool line_next = false;
    bool load_symbols = false;                       // ask for a debug or label file, then `ll` it
    std::vector<std::string> commands;               // monitor lines: memory and register edits
    std::vector<std::string> console_lines;          // typed into the Console: sent through the hub's local client
    bool keys_to_tvc = false;                        // the Screen panel has focus
    bool heat = false;                               // the Memory panel is visible and wants heat
    bool events = false;                             // the Screen panel is visible and wants the event log
};

class BenchView {
public:
    // Gives the Screen panel focus in the next frame, so a running program keeps its keyboard.
    void focus_screen() { focus_screen_ = 10; }
    // Monitor text for the Console: the hub's output to it and the echo of Bench's own commands.
    void console_output(std::string_view text);
    // Echo of a command line: after the prompt the hub already sent, or after `prompt` when none is pending.
    void console_command(std::string_view prompt, std::string_view line);
    // The MCP server whose tool calls the Agent panel shows; nullptr: none was started.
    void set_mcp(const remote::McpServer* mcp, std::string endpoint) {
        mcp_ = mcp;
        mcp_endpoint_ = std::move(endpoint);
    }
    // The output of a monitor line Bench sent; errors of the Checkpoints panel's own lines show there.
    void command_result(std::string_view line, std::string_view output);
    // The streaming ARGB texture of kVramWidth x kVramLines the Video panel draws into; the host owns it.
    static constexpr int kVramWidth = 512;
    static constexpr int kVramLines = 256;
    void set_vram_texture(Display* display, ImTextureID texture) {
        display_ = display;
        vram_texture_ = texture;
    }
    // Keeps the Watch list in layout.ini; call once after ImGui is created and before its first frame.
    void install_settings();
    // Draws the whole window; the strip's actions go to `play`.
    BenchActions draw(const PlayStatus& status, remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts,
                      PlayActions& play);

private:
    struct Row {
        std::uint16_t address = 0;
        bool label = false;  // a "name:" line above the instruction
        std::string text;
        debug::Disassembly disassembly;
    };

    void track_stops(remote::Monitor& monitor);
    void update_watches(remote::Monitor& monitor);
    void draw_run_bar(const PlayStatus& status, remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts,
                      ImVec2 min, ImVec2 size, PlayActions& play, BenchActions& actions);
    void draw_pages(const Machine& machine, const Skin& skin, const SkinFonts& fonts, float scale, ImVec2 min, ImVec2 size);
    void draw_screen_panel(const PlayStatus& status, remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts,
                           BenchActions& actions);
    void draw_machine(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale);
    void draw_console(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions);
    void draw_agent(const Skin& skin, const SkinFonts& fonts);
    void draw_checkpoints(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions);
    void draw_watch(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions);
    void draw_history(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts);
    void draw_io_log(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions);
    void draw_video(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale);
    void draw_profiler(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, BenchActions& actions);
    void draw_source(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale, BenchActions& actions);
    // A source file's lines, read again when it changes on disk; empty if it cannot be read
    const std::vector<std::string>& source_text(const std::string& path);
    // Brings the Listing or the Memory panel to the front at this place
    void show_in_listing(std::uint16_t address);
    void show_in_memory(std::optional<Bank> bank, unsigned address);
    void draw_listing(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale, BenchActions& actions);
    void draw_cpu(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale, BenchActions& actions);
    void draw_memory(remote::Monitor& monitor, const Skin& skin, const SkinFonts& fonts, float scale, BenchActions& actions);
    void memory_keys(const Machine& machine, unsigned per_row, BenchActions& actions);
    void build_rows(remote::Monitor& monitor, std::uint16_t top, std::size_t count);
    // Start of the instruction `count` instructions before `address`, by the earliest decoding that lands on it.
    std::uint16_t back_up(const Machine& machine, std::uint16_t address, unsigned count) const;
    void go_to(std::uint16_t address);

    int focus_screen_ = 0;  // frames left to ask for the Screen panel's focus
    bool keys_to_tvc_ = false;
    bool layout_built_ = false;

    // Stops: a new one is a hold that starts, or a held machine whose time moved (a step)
    bool was_held_ = false;
    std::uint64_t held_ticks_ = 0;
    std::uint64_t stops_seen_ = 0;
    std::string reason_;
    // Machine state worth marking when it changed between stops
    struct MachineState {
        std::uint8_t page = 0;
        std::uint8_t video_paging = 0;
        unsigned iomem = 0;
        std::uint8_t border = 0;
        std::array<std::uint8_t, 4> palette{};
        std::uint8_t mode = 0;
        std::array<std::uint8_t, Mc6845::kRegisters> crtc{};
        std::uint16_t pitch = 0;
        bool tone = false;
        unsigned amplitude = 0;
        bool sound_irq = false;
    };
    static MachineState machine_state(const Machine& machine);

    Z80Registers shown_{};
    Z80Registers previous_{};
    MachineState shown_machine_{};
    MachineState previous_machine_{};
    bool have_shown_ = false;
    bool have_previous_ = false;

    // Screen markers
    bool show_out_ = true;
    bool show_in_ = false;
    bool show_irq_ = true;

    // Listing
    std::uint16_t top_ = 0;
    std::optional<std::uint16_t> cursor_;
    bool follow_ = true;
    char goto_text_[64] = {};
    std::string goto_error_;
    std::vector<Row> rows_;

    // Memory: the CPU's view (no bank) or one bank by offset
    std::optional<Bank> memory_bank_;
    unsigned memory_top_ = 0;
    std::optional<unsigned> edit_;       // byte being edited
    std::optional<unsigned> edit_high_;  // its first typed nibble
    char memory_goto_[64] = {};
    std::string memory_error_;

    // Console: scrollback, the input line and its history
    std::deque<std::string> console_;
    std::string console_partial_;  // output after the last newline
    bool console_scroll_ = false;
    char console_input_[256] = {};
    std::vector<std::string> history_;
    std::size_t history_pos_ = 0;
    bool console_focus_ = false;

    // Agent lane
    const remote::McpServer* mcp_ = nullptr;
    std::string mcp_endpoint_;
    std::uint64_t agent_seen_ = 0;

    // Checkpoints panel
    std::vector<std::string> checkpoint_lines_;  // sent, their result not seen yet
    std::string checkpoint_error_;
    std::vector<unsigned> stopped_by_;  // the checkpoints of the latest stop
    int add_kind_ = 0;
    bool add_trace_ = false;
    char add_where_[64] = {};
    char add_condition_[128] = {};
    std::string add_line_;  // the add form's line while it waits for its result
    unsigned edit_checkpoint_ = 0;
    int edit_field_ = 0;  // 1 condition, 2 ignore count
    char checkpoint_text_[128] = {};
    bool checkpoint_focus_ = false;
    double delete_all_armed_ = -1.0;  // ImGui time of the first click

    // Watch panel; the list is kept in layout.ini
    enum class WatchFormat : std::uint8_t { Value, Bytes, Text };
    struct Watch {
        std::string text;
        WatchFormat format = WatchFormat::Value;
        std::string at_stop;                     // the value at the latest stop
        std::optional<std::string> before_stop;  // and at the one before
        std::uint64_t stop_seen = 0;
    };
    std::vector<Watch> watches_;
    std::uint64_t stop_serial_ = 0;  // counts stops, for the watches' changed colour
    char watch_input_[128] = {};
    std::optional<std::size_t> edit_watch_;
    char watch_text_[128] = {};
    bool watch_focus_ = false;

    // History panel: the trace entries the filter keeps, rebuilt for a new stop or filter
    char history_filter_[64] = {};
    std::string history_filter_used_;
    std::uint64_t history_built_ = ~std::uint64_t{0};  // stop_serial_ the rows are for
    std::uint64_t history_pushed_ = 0;
    std::vector<std::uint32_t> history_rows_;
    bool history_scroll_ = false;
    std::optional<std::uint64_t> history_selected_;  // ticks of the clicked instruction

    // I/O log panel
    bool io_out_ = true;
    bool io_in_ = false;
    bool io_irq_ = true;
    char io_ports_[64] = {};
    bool io_follow_ = true;
    std::uint64_t io_seen_ = 0;  // io_log().pushed() at the last scroll to the end
    std::vector<std::uint32_t> io_rows_;
    std::optional<std::uint64_t> io_selected_;  // ticks of the clicked event

    // Video panel: a VID bank as a picture
    Display* display_ = nullptr;
    ImTextureID vram_texture_ = ImTextureID_Invalid;
    std::vector<std::uint32_t> vram_pixels_;
    std::optional<unsigned> video_bank_;    // empty: the bank the screen shows
    std::optional<std::uint8_t> video_mode_;  // empty: the machine's mode
    bool video_palette_live_ = true;
    std::array<std::uint8_t, 4> video_palette_{0, 15, 10, 9};  // IGRB, used when not live
    bool video_dim_ = true;                                   // dim the bytes the screen does not show

    // Profiler panel: the routines in the table's order, the busiest instructions (rebuilt now and then)
    bool profile_by_routine_ = true;
    std::vector<std::size_t> profile_rows_;
    struct HotInstruction {
        BankAddress where;
        std::uint32_t count = 0;
        std::uint64_t ticks = 0;
    };
    std::vector<HotInstruction> hot_;
    std::uint64_t hot_built_ = ~std::uint64_t{0};  // profile().instructions() when hot_ was built
    std::uint64_t hot_frame_ = 0;

    // Source panel
    std::optional<std::uint32_t> source_file_;  // the file shown; follows the PC's file at each stop
    bool source_follow_ = true;
    std::uint64_t source_stop_seen_ = 0;
    bool source_scroll_ = false;
    std::optional<std::uint32_t> source_cursor_;  // a clicked line
    struct SourceText {
        long long stamp = 0;  // the file's write time when read
        std::vector<std::string> lines;
    };
    std::map<std::string, SourceText> source_texts_;

    // CPU card: the register being edited
    std::string edit_register_;
    char register_text_[8] = {};
    bool register_focus_ = false;
};

}  // namespace freetvc::sdl
