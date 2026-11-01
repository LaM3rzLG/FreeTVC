# User interface — design

This document describes the GUI (`freetvc`): its visual direction, Play mode and the status strip, Bench and its panels, the settings window, the picture model, the keyboard editor, the language system, the settings file with the ROM search, and skins.

No part of the UI is generic boilerplate: every choice below names the reason it was made.

## Design direction

### The machine's own materials

The chrome is built from the real TVC's materials. The colours were taken by eye from two public photos on Wikimedia Commons (used only as a reference; neither is redistributed):

- [Videoton TVC 01.JPG](https://commons.wikimedia.org/wiki/File:Videoton_TVC_01.JPG), by VillanyGabi, public domain;
- [Videoton TV-Computer (1986) (37650450314).jpg](https://commons.wikimedia.org/wiki/File:Videoton_TV-Computer_(1986)_(37650450314).jpg), by Wolfgang Stief (via Flickr), CC0.

| Material on the machine | Role in the UI | Default (`bronze` skin) | Reason |
|---|---|---|---|
| Bronze-brown case, upper part | frame around the screen | `#2a231e` | a warm, mid-dark surround does not compete with the saturated TVC picture, and is not the near-black + cyan template |
| Case, keyboard deck | status strip, panels, popups | `#3b322b` | the strip sits where the keyboard deck sits: under the screen |
| Dark groove between case top and keyboard deck | the line above the strip, panel header bands | `#1c1713` (groove), `#4c4037` (rib) | the same horizontal break the case has |
| Charcoal modifier keys (LOCK, SHIFT, CTRL, ALT) | buttons and other controls | `#565048` | controls look like the machine's function keys |
| Case, a step lighter than the deck | dense data panels (Listing, Memory, Console) | `colour.panel` `#443a32`, text in the lettering colour | light data panels stood out from the dark UI as if from another program; dark ones keep their own surface but belong to the case |
| Putty letter keys | dense data panels with `panel.light = yes` (the `deck` skin) | `#dcd5c6`, ink `#2b2520` | dark-on-light like the printed listings of the TVC manuals, kept as a skin choice |
| White "VIDEOTON TV-Computer" lettering | text on the case | `#ece6dc`, muted `#b3a796` | the only light text the case carries |
| Four ribbed expansion-slot covers | four slanted marks left of the nameplate | rib colour; lit when a card is inserted | the machine's four slots, shown where the user can see what is plugged in |

These are approximations (the photos' white balance differs); they are skin values and can be tuned.

### Colour meaning: only the TVC's own 16 colours

The only saturated colours anywhere in the UI are the TVC's 16 IGRB colours ([tvc-hardware.md](../hardware/tvc-hardware.md#colour-levels): component = C·(4/7 + I·3/7)). Each has one fixed meaning:

| Meaning | TVC colour | Skin key |
|---|---|---|
| execute, PC, breakpoint | red | `role.exec` |
| read | green | `role.read` |
| write | magenta | `role.write` |
| I/O port access | cyan | `role.io` |
| VRAM | blue | `role.vram` |
| IRQ, changed value, notice | yellow | `role.notice` |

Play mode uses only `role.notice` (paused state, strip notices); Bench uses all six.

Rule taken from the hardware: the **bright** variant (I = 1) is used on the dark case surfaces, the dark data panels included, and the **dark** variant (I = 0, about 146/255) on light putty panels (`panel.light = yes`). A skin may replace either variant with any colour.

### Shapes and type

- Square corners, 1 px lines, no shadows, no gradients, no icon font. Labels are words; the few glyphs (slot ribs, state marks, the play/stop key) are drawn with ImGui's draw list.
- **Atkinson Hyperlegible Next** (Regular, Medium) for labels and **Atkinson Hyperlegible Mono** for data (registers, hex, addresses). The Braille Institute designed the family so that look-alike glyphs (0/O, 1/l/I, B/8, 5/S) cannot be confused, which is the main legibility problem of hex dumps. SIL OFL 1.1; `cmake/FreeTVCFonts.cmake` fetches them at configure time from [googlefonts/atkinson-hyperlegible-next](https://github.com/googlefonts/atkinson-hyperlegible-next) and [googlefonts/atkinson-hyperlegible-next-mono](https://github.com/googlefonts/atkinson-hyperlegible-next-mono) (version 2.001, commits and SHA-256 in `THIRD_PARTY.md`), and they are shipped as separate files in `fonts/` next to the program, never compiled into it.

## Play mode

Play is the default mode at start: the 4:3 screen, centred in the frame colour, nothing else on top of it, and the status strip under it.

### Picture area

- **TV area by default**: the framebuffer from x 131, y 23, 650 × 287 pixels.
  - A PAL set shows 52 µs of each 64 µs line, starting about 10.5 µs after the sync edge (650 pixels from x 131), and about 287 lines per field.
  - This hides the HSYNC strip (x 0–15), the vertical blanking (rows 0–18) and the two mid-line blanking steps (row 19 from x 712, row 313 from x 200), all real signal features ([machine.md](machine.md), section "Generator").
  - The display area sits in the middle: left and right borders 69 and 68 pixels, top and bottom 26 and 21 lines.
  - WinTVC 1.4.1 shows almost the same area (x 136, y 24, 640 × 285; measured on its screenshots under Wine, [machine.md](machine.md), section "Golden frames").
- **Full raster (debug)**, in the main menu under Screen, in the settings window, or with `--full-raster`: all 800 × 314 framebuffer pixels, sync and blanking included, with the TV area outlined (label colour, 60 % alpha). Meant for code that changes the CRTC, border or blanking.
- The TV area has no overscan: a real set cuts another 5–10 %.

### Status strip

One status strip at the bottom (the keyboard deck), separated by the groove line. Left to right:

- four **slot ribs**: a slot with a card is lit in the lettering colour, an empty one stays in the rib colour; the tooltip lists slots 0–3 with their ports and cards; a click opens the **Cards** menu;
- the nameplate **free**TVC: opens the **main** menu;
- the preset name (e.g. `64k+-2.2`): opens the **machine and ROM** menu;
- state (running/paused) and emulation speed in % of real time; a click pauses or resumes;
- keyboard mapping: a click toggles symbolic/positional ([machine.md](machine.md), section "Host input");
- audio: a click mutes;
- the **play/stop key** (a triangle or a square drawn on the deck), then the **tape item**, both always there. The key stops while PLAY or RECORD is down, else plays; with no tape it opens the insert-tape dialog and the chosen tape goes in playing. The item shows **no tape**, or the deck's state and counter (in the notice colour while the tape moves), and opens the **Tape** menu;
- **disk**, only while the disk card is in: **disk A:** in the notice colour while the drive is active (the drive light; fast disk runs unpaced then), the drives in its tooltip ([cards.md](cards.md), section "Front ends" of the HBF card); it opens the **Disk** menu;
- right-aligned: the latest notice (screenshot path, skin error, preset error), shown for a few seconds. It never covers the strip's items: a notice too long for the space after them is cut with "..." and shows whole on hover. The start position is computed from the items, not from ImGui's cursor, which wraps to the next line once the row is full.

### Strip menus

Each device has its own menu, opened from its own strip item, so no menu grows taller than the window (one combined menu needed scrolling). The main menu fits the default 1000 × 760 window with room to spare.

| Strip item | Menu | Contents |
|---|---|---|
| slot ribs | **Cards** | "Disk card in slot 0" (HBF; the disk-card switch lives here, not in the Disk menu); GameCard; Multichip sound card (under each slot card, indented, **Put in slot** or, while it is in, **Move to slot**: a submenu of slots 0–3 with their ports, the card's own checked, a slot holding another card greyed out with its name; the choice puts the card there or moves it and powers on); the SID card, MOS 6581 or 8580 (one at a time: the checked one takes it out; no slot rib, it sits outside the slots); TVC256k++ and, while it is in, its USB port (pendrive folder, mouse, empty) and flash drive image ([cards.md](cards.md), TVC256k++ "Front ends"); NanoSD and, while it is in, its SD card folder or "Take the SD card out"; the serial card (slot 1, else the first free one) and, while it is in, its cable ("Cable unplugged", "Cable to TCP port 6585" showing "connected" or "waiting"), "Send a file..." and "Capture to a file..."; a **Printer port** section: "Printer to a file..." (a save dialog: `.txt` gets UTF-8 text, anything else the raw bytes) and "No printer" (the default; the port is the main board's own, but a printer is something plugged into the machine); a **Cartridge** section: "Insert cartridge...", "Remove cartridge", "New MultiCart image..." (pick the programs, then the image's name; it goes in), "SD Dual cartridge" with its "microSD card image...", "SD card image..." and "Take ... out", and "New SD card image..." (pick the files, then the image's name; it goes into the microSD socket, with the cartridge). Cartridges are plug-in boards, though not slot cards, so they are here |
| nameplate **free**TVC | **main** | Program: "Load program..." (every format, so it belongs to no one device), "Run again", "Eject cassette"; Snapshot: quick save, quick load, save and load snapshot; Keyboard: symbolic, positional; Skin: the skins found, "Reload skin"; Language; Screen: full raster, screenshot, full screen; then **Settings...**, **Bench** and **Quit** |
| preset name | **machine and ROM** | the presets, **Download ROMs** (the TVC's ROMs and the card ROMs, with the system's curl, into the `rom_dir` folder if one is set, else the user data folder; [build.md](build.md), section "ROM autofetch"), then Pause, Reset, Power cycle (they act on the machine) |
| tape item | **Tape** | "Insert tape..." (a `.wav` or `.ttp` file, or a `.cas` played as a tape), "Run the tape", Play, Stop, Rewind, "Record to WAV...", "Eject tape", "Fast tape" ([media.md](media.md), section "Tape deck") |
| disk item | **Disk** | A:–D: insert and eject, "Fast disk" (full speed while a drive works; the machine's own timing stays real) |

Rules:
- **Disk**: as soon as the disk card is in (from the Cards menu, a dropped disk, the command line or `freetvc.ini`), the disk item and its Disk menu are in the strip with no further step; while the card is out there is none. The item appears; the menu does not pop open by itself.
- **Tape**: the tape item is always in the strip, with the play/stop key before it. With a tape in, the key plays or stops it; with no tape, play opens the insert-tape file dialog. The Tape menu also has its own **Insert tape...** entry.
- Each menu opens upward from its item's left edge, above the groove. A click on another strip item while a menu is open switches to that item's menu in one click; a click on the same item, or anywhere else, closes it. **Escape** closes it too (ImGui's keyboard navigation stays off, so the strip does that itself). While a menu is open, keys go to the menu and every held TVC key is released.
- Bench shares the strip, so it has the same menus.

### Reserved host keys

Never sent to the TVC:

| Key | Action |
|---|---|
| F11 | full screen |
| F12 | Play ↔ Bench |
| Pause | pause/resume; a pause is a monitor stop, the same state Bench and remote clients see |
| F9 | quick save (Play only; in Bench F9 sets a checkpoint) |
| Shift+F9 | quick load |
| F10 (held) | rewind ([media.md](media.md), sections "Snapshots" and "Rewind") |

F1–F8 stay with the TVC (positional mapping).

### Mouse capture

With a mouse on the TVC256k++'s USB port, a left click on the picture (in Play, no menu open) puts the host mouse in relative mode and sends its motion and buttons to the card once a frame. **Ctrl+Alt** (the usual virtual-machine release; every F-key is taken) or losing focus lets it go, with a notice each way. Keys still reach the TVC while it is captured.

### Screenshots

The screenshot key and menu item write a PNG, one pixel per framebuffer pixel, of the area in view: 650 × 287 (TV area) or 800 × 314 (full raster), with SDL's PNG writer. They are saved in the user data folder (`SDL_GetPrefPath`, subfolder `screenshots/`), and the strip shows the full path. Screenshots are always the framebuffer's pixels, never the picture model's output ("Picture" below).

### Checks

On the real GUI (`tools/ui/ui_drive.py`: XTEST clicks and keys, window grabs, a scratch user data folder):
- each of the five menus opening at its item; Disk → Tape and Cards → Main in one click; Escape closing; a second click on the nameplate closing;
- the key stopping and playing a tape (`tape stop` / `tape play`, square ↔ triangle); the key with no tape opening the "Open File" dialog with the Tapes filter, a picked `Brekk.cas` going in playing; with the key before the tape item, the item opening the Tape menu and the key the file dialog;
- "Disk card in slot 0" putting the card in (slot 0 lit, **disk** in the strip, no menu opening by itself) and taking it out;
- the main and Cards menus in Bench.
- Default positions at 1000 × 760 with `bronze` (English): ribs (27, 745), nameplate (83, 745), preset (144, 745). Hungarian strip items sit further right, so coordinates from English grabs miss.
- Not checked: a display scale other than 1, the `deck` skin, Windows.

## Bench

F12 switches the window between Play and Bench, the debugging mode.

### Design

- **Screen as timeline.** The framebuffer is beam-ordered and a frame is 314 lines × 200 T, so a frame's timing has the shape of the picture. Port accesses and interrupts are drawn as markers on the live screen at the beam position where they happened, with the beam line in red when stopped. This replaces a separate raster event viewer.
- **Page column**: a permanent bar for 0000/4000/8000/C000 showing the current mapping.
- **Listing and Memory** on data panels; memory shows read/write/exec heat as fading tints.
- **CPU and machine cards**: registers, flags, IM/IFF, beam position, CRTC registers, the real palette swatches, border, mode, sound, the live keyboard matrix.
- **Console** with the same parser as the TCP text monitor, and an **Agent** lane that shows what a connected MCP client reads and does, live.
- One window; Bench panels dock inside it. Docking stays enabled so panels can be moved, but the default arrangement is fixed and the layout is saved per user (`layout.ini`, beside `freetvc.ini`). A missing or broken file gives the default arrangement; deleting `layout.ini` restores it (there is no "default layout" command). Panels have no close button, so none can be lost.
- F12 never changes the machine's state: a running machine keeps running in Bench.
- Every UI string goes through `tr()` ("Language" below).

### Window frame

Top to bottom:
- **Run bar** (deck colour, groove line under it): Run / Pause, Step, Over, Out, To cursor, Reset, the stop state ("stopped at CAA3: checkpoint #1", or the speed), and a "Play" button on the right.
- **Page column**, fixed on the left: the four 16 KB pages, 0000h at the top. Each segment names what is paged there now (SYS, CART, U0–U3, VID0–3, EXT, CARDn); a page split at 8 KB (C000h card, E000h EXT) shows both halves. VID text is blue (`role.vram`), and the segment holding the PC has a red edge (`role.exec`). A click opens the Listing and the Memory panel there.
- **Dock space** in the middle.
- The **status strip** at the bottom, the same strip as in Play.

### Control through the monitor

Every Bench command goes through the text monitor engine (`remote/monitor.hpp`), so Bench, TCP clients, MCP and DeZog see one stopped state ([debug-and-remote.md](debug-and-remote.md#text-monitor)). The GUI therefore always creates the debugger and the monitor; the TCP, MCP, DZRP and GDB listeners open only when asked for (command line, `freetvc.ini` or the settings window). Cost, measured: 5000 frames headless take 3.41–3.46 s without the monitor and 3.72–3.76 s with it (+8 %, still about 27× real time). Pause (key, strip or run bar) enters the monitor in both modes; Run is `x`, Step `z`, Over `n`, Out `ret`, To cursor `until`.

### Keyboard

Keys go to the TVC only while the Screen panel has focus (its title says "Screen: keys go to the TVC"). Entering Bench gives it focus, so a running program keeps its keyboard. Only the focused panel's tab is lit (rib colour, label overline; unfocused tabs are deck). Otherwise these Bench keys work:

| Key | Action |
|---|---|
| F5 | run |
| F6 | step |
| F7 | over |
| F8 | out |
| F4 | to cursor |
| F9 | checkpoint at the cursor |
| Shift+F6, Shift+F7 | line in, line over (while stopped, with a debug file loaded) |

F11, F12 and Pause stay reserved in both modes.

### Default arrangement

Screen top left, Listing under it, CPU card on the right, the Machine card under the CPU card. The Memory, Console, Checkpoints, History, I/O log and Profiler panels are tabs beside the Listing (Source right after it), the Video panel a tab beside the Screen, the Agent lane a tab beside the Machine card, and the Watch panel under the Machine card. Memory, Console and Agent join their dock node behind its first panel (`NoFocusOnAppearing`), so a fresh layout opens with the Listing and the Machine card in front. A layout saved before a panel existed gets it at its default place on first use (`SetNextWindowDockID`): the Memory panel beside the Listing, the Machine card as a tab beside the CPU card, the Watch panel as a tab beside the Machine card.

### Screen

The picture at the PAL aspect (TV area or full raster, as in Play), always with sharp pixels (the event marks need them). While stopped, the beam position is a red line across the row with a mark at the column.

**Event log** in the debugger (`debug::EventLog`; no core change): while the Screen panel is visible every port is flagged and interrupt entries are watched; each OUT, IN and interrupt entry is kept with its value, PC and the beam's line and column. The video catches up every T-state before a port access is handled, so the beam position is exact. Two frames are kept: the one on screen and the one in progress (at most 4096 events each; extra ones are counted). Cost, measured: 5000 frames 4.45–4.50 s with or without it (port accesses are rare).

**Markers** at the beam position of each event: ports in cyan (`role.io`), interrupt entries in yellow (`role.notice`). Events of the frame in progress are drawn hollow (visible while stopped). Toggles above the picture choose OUT, IN and IRQ; IN starts off because the keyboard poll (58h) fills every frame. Events outside the picture (blanking, in the TV area) are counted under the toggles; full raster shows them all. Hovering lists the events under the mouse (port name, value, CRTC register for 71h, PC); a click lists the PC.

### Listing

A data panel: label lines, address, bytes, instruction, comment; the disassembler with the monitor's symbol table ([debug-and-remote.md](debug-and-remote.md#disassembler)). A red triangle marks the PC, red dots the exec checkpoints (hollow when disabled). Click a line to set the cursor; click the gutter to toggle an exec checkpoint there, tied to the bank paged in at that address (bank + offset). The wheel scrolls by instructions; "Go to" takes a monitor expression; the listing follows the PC and follows it again after every stop. While a profile has data, a share column shows each instruction's share of the T-states ("Profiler" below).

### CPU card

Case colour, data font: main and alternate registers, IX, IY, SP, PC, I, R, IM, IFF1/2, the flags as eight cells, HALT, IRQ pending, frame, beam line and column, T-states. Values that changed since the previous stop are yellow (`role.notice`).

**Register editing**: while stopped, a double-click on a value edits it (Enter sets it with the monitor's `r name=value`, leaving the field cancels); a click on a flag cell toggles that flag (`r f=..`). Monitor errors show in the strip.

### Memory

A data panel. The view is "CPU" (current paging, a bank tag per row) or one bank by offset. Rows of 8, 16 or 32 bytes, whichever fits, in groups of 8, then ASCII. "Go to" takes `[BANK:]expression` and switches to that bank. The wheel scrolls by three rows; a page-column click moves both the Listing and the Memory panel.

- **Heat**: while the Memory panel is visible the debugger stamps every byte's last exec, read and write with the frame number (`debug::HeatMap`, 16 banks × 16 KB × 3 kinds, 3 MB). Operand bytes read inside the current instruction count as exec. The latest kind tints the byte (`role.exec` red, `role.read` green, `role.write` magenta, in the panel's variant), fading over 50 frames. Cost, measured: 5000 frames 4.49–4.53 s with the debugger and trace, 5.11–5.13 s with heat (+14 %, about 20× real time).
- **Editing**: click a byte, type two hex digits per byte (the cursor moves on; arrows move it; Enter or Esc ends). Each byte is a monitor `>` line. ROM banks (SYS, CART, EXT) and cards are read-only here: a click there says "SYS is read-only". (The monitor's `>` and MCP's `memory write` can patch ROM images; only this panel keeps them read-only.)

### Machine card

Case colours, data font: paging (port 02h, video paging 0Ch–0Fh with the displayed bank, IOMEM slot), video mode, border and the four palette swatches, the 18 CRTC registers (the selected one marked), the sound generator (pitch, f = 195 312.5 / (4096 − P) Hz, tone, amplitude, IRQ), IRQ pending, and the keyboard matrix (10 × 8, held keys lit). Values that changed since the previous stop are yellow.

### Console

A data panel: the text monitor as an in-process client of the hub (`MonitorHub::attach_local`, `submit_local`, `take_local_output`). Its lines go through the hub's queue like a TCP client's, so a line typed while the machine runs stops it first (as in VICE), and stop reports reach it like every client. Bench's own commands (run bar, Pause, F-keys, edits) are echoed there with their output, so the Console is the log of everything Bench did. Up/Down recall earlier lines; the scrollback keeps 4000 lines. With the local client attached, stop reports no longer go to stdout.

### Agent

The MCP server's last 256 tool calls (`McpServer::calls()`), each with its number, tool, arguments, the first text of the result (`[image]` for a screenshot), the time it took and the frame it came in; errors in yellow (`role.notice`). Without an MCP server it says how to start one.

### Checkpoints

A data panel listing every checkpoint of the debugger, whoever set it (gutter, Console, TCP, MCP, DeZog, GDB): an enable box, number, kinds (in their role colours: exec red, load green, store magenta, ports cyan, irq yellow; `log` for trace checkpoints, `once` for temporary ones), place, condition, ignore count, hits and a delete button. Every change is a monitor line (`break`/`trace`, `enable`, `disable`, `condition`, `ignore #n`, `delete`), so it works while the machine runs without stopping it, and it shows in the Console.

- The add form: a kind (exec, load, store, load + store, in, out, in + out, irq, line), the place as the monitor takes it (`[BANK:]start [end]`, ports, `#row`; empty for any irq), an optional condition and "Log only" (`trace`). The monitor's error shows under the form and in the strip; a line that worked clears the form.
- Condition and ignore count: double-click the cell, Enter sends it, an empty condition clears it.
- The checkpoint that stopped the machine has its number in yellow; hovering the hits shows the latest hit from the debugger's hit log (e.g. "store 0003 = F0 from PC FFF8").
- Clicking the place of an exec or irq checkpoint opens the Listing there (a bank offset where that bank is paged now), of a load/store one the Memory panel (its bank view for `BANK:offset`).
- "Delete all" asks for a second click within 3 s.

### Watch

A data panel of expressions in the monitor's syntax (`HL`, `{SP}`, `[0003]`, `VID0:0000`, labels), evaluated every frame. Formats, from the row's right-click menu: **value** (hex, signed decimal, the character for 20h–7Eh), **bytes** (8 bytes at `[BANK:]address`) and **text** (up to 32 printable characters to the first 00h). A `BANK:` expression starts as bytes. A value that changed between the last two stops is yellow (as on the CPU card); an error (e.g. an unknown label) is red.

- Double-click edits a row (empty removes it); the menu also has Show in Memory, Show in Listing, "Stop on write" (a `break store` over the bytes or text shown) and Remove. A right-click focuses the panel, so F5–F9 work after the menu.
- The list is kept in `layout.ini` as a `[Bench][Watch]` section (`format expression` lines), so it survives restarts with the layout; checkpoints are not kept.

### History

A data panel: the debugger's instruction history (the monitor's `chis`, 65 536 instructions) as a table, only while the machine is stopped (running, it says so). Columns: T-states before the stop, bank, PC, the instruction with labels and system call names, and **Changed**: the registers the instruction changed (A, F, BC, DE, HL, IX, IY, SP, the alternate set, I, IM, IFF; not R), in yellow. The registers after an instruction are the next entry's "before"; when an interrupt follows, its own push and DI are left out. Interrupt entries are marked `irq`.

- The newest entry is the instruction at the PC, logged as it starts and not run yet, so it is left out (the Listing shows it).
- A filter keeps the rows whose address, bank, instruction or comment holds the text (case-insensitive); a new stop or filter scrolls to the newest row. Rebuilding disassembles every entry: 65 536 take 3.2 ms (measured).
- A click selects the row and opens the Listing at its PC; hovering shows the main registers after it.

### I/O log

A data panel over the debugger's I/O ring (`Debugger::io_log()`, 65 536 `IoEvent`s: the screen markers' `BeamEvent` with its tick and frame), recorded while the Screen panel or the I/O log is visible and kept while neither is; Clear empties it. Columns: frame, beam line and column, OUT/IN (cyan) or IRQ (yellow), port, value, what the port does (the markers' names), PC (the handler for an IRQ).

- OUT, IN and IRQ toggles (IN off at start: the keyboard scan reads 58h ten times a frame) and a port filter (`70 71 60-63`; a bad word is reported); IRQ rows ignore the port filter.
- Follow keeps the newest event in view; scrolling up turns it off.
- Hovering shows the screen marker's text; a click opens the Listing at the PC.

### Video

Case colours: a VID bank as a 512-dot × 256-line picture at the Screen panel's PAL dot aspect. The bank layout makes this exact for every mode: address = column + 64 × (row × 4 + line), so picture line y is the 64 bytes from y × 64 ([tvc-hardware.md](../hardware/tvc-hardware.md#6845-usage)).

- **Bank**: "shown" (the bank port 0Fh puts on the screen now) or any VID bank the machine has. **Mode**: the machine's or 2-, 4- or 16-colour. **Palette**: the machine's four entries live, or chosen from the 16 IGRB colours (a choice unticks "machine palette"); greyed out in 16-colour mode.
- The bytes come out through `Video::decode()`, the generator's own rule (`video_test` compares the two on 240 × 64 bytes in all three modes); decoding a bank takes 0.05 ms (measured), so it is redrawn every frame, live while running.
- **Screen area**: `displayed_vram()` gives the bytes the 6845 shows with its current registers (R6 rows of R1 characters from R12/R13, R9 + 1 lines, RA b1–0); "dim off-screen" darkens the rest. The line above the picture gives the start, the geometry, the cursor address and the byte count.
- On the shown bank: the cursor's cell in yellow (lines R10–R11; not when R10 hides it), and while stopped with the beam in the displayed rows and columns, the byte it reads next in red with a faint red line across. (Outside them, for example in vertical blanking, the 6845's address does not point at a shown byte, so no mark is drawn.)
- Hovering gives the bank address and byte, the picture line, byte and dot with its IGRB colour, and where the screen shows it (character row, column, line of the row) or that it is off screen; a click opens the Memory panel there.
- Core support: `Video::decode()` (public; the generator calls it), `Video::palettes()`, `displayed_vram()`, `Mc6845::ma()` and the public start and cursor addresses. The texture (ARGB 512 × 256) belongs to `main.cpp`.

### Profiler

A data panel over the debugger's `Profile` (`src/debug/profile.*`; [debug-and-remote.md](debug-and-remote.md#profile)). Record and Stop send the monitor's `profile on` / `profile off` (off keeps the counts), Clear `profile clear`; the status line gives the instructions, T-states and the TVC seconds they make.

- **Routines**: one row per routine (a taken CALL or RST target, an interrupt entry) plus "(outside any call)": calls, exclusive T-states (its own instructions) and inclusive (with its callees, a running call included), each share with a bar; sortable by calls, exclusive or inclusive (exclusive first). The name is the CPU address where the bank is paged now, the bank and offset, and a label; a click opens the Listing there, or the Memory panel for a bank not paged in.
- **Instructions**: the 500 busiest addresses with the instruction (disassembled from the bank's bytes), count, T-states and share; rebuilt when stopped and changed, every 25 frames while recording. A click opens the Listing.
- **Listing column**: while a profile has data, each instruction's share of the T-states, right-aligned in yellow (`<0.1%` when it ran but rounds to 0.0).
- Cost, measured headless: 5000 frames 5.34–5.42 s without, 5.98–6.01 s with (+11 %, about 17× real time).

### Source

A data panel over the monitor's source map ([debug-and-remote.md](debug-and-remote.md#source-level)): Load symbols... (a file dialog, then the monitor's `ll`), Line in and Line over (`lstep`, `lnext`; also Shift+F6 and Shift+F7 anywhere in Bench while stopped), the file list and Follow PC.

- The file's text with line numbers, read from disk and read again when it changes; lines that have code in the panel's ink, the rest faint. At each stop the PC's file is shown and its line is red with the PC triangle (Follow PC; choosing a file turns it off).
- The gutter toggles an exec checkpoint at a line's first instruction (the Listing's dots); a click selects a line, a double-click opens the Listing at its code.
- The scroll area draws no background of its own, so the panel's surface shows through.
- Without a debug file it says which files work.

### Checks

All on the real GUI (`tools/ui/ui_drive.py`: XTEST keys and clicks, window grabs, the user data folder redirected with `XDG_DATA_HOME`), most of them on a virtual X display (Xvfb):

- **Shell, Screen, Listing, CPU card**: F12 into Bench with the machine running; the Screen panel has focus and a space reaches the TVC (logo to the BASIC prompt); after a click into the Listing, typed letters do not reach it. Pause; Go to `PC` and `reboot` (label lines from the ROM symbols); wheel up and down; a page-column click lists 0000h with its tooltip. A gutter click sets an exec checkpoint (red dot); F5 stops on it ("stopped at CAA3: checkpoint #1"); F6 steps, with PC and R in yellow; F4 runs to the cursor. F12 back to Play keeps the machine stopped; Pause in Play resumes it. A splitter dragged with the mouse is saved in `layout.ini` and restored at the next start. With `--remote-monitor`, a TCP client's `r` stops the GUI, Bench shows the same registers, a Bench step is seen by the client, and the client's `x` resumes. Autostart of `Brekk.cas` in the GUI runs through the monitor's run loop.
- **Memory**: tab placement with a fresh and with an older `layout.ini`; CPU view at the BASIC prompt with heat (write at 0003h, the interrupt entry at 0038h executed); a page-column click to U2, typing `4142` at 8000h (`41 42`, "AB"); a click into SYS ("SYS is read-only"); HL set to 1234 by double-click and typing; the C flag toggled (AF 0042h → 0043h); the view menu listing CPU and the banks. Not checked: a bank view edit (`ui_drive.py` cannot type `:`), heat in a bank view, editing while running.
- **Markers and Machine card**: markers at the BASIC prompt (the cursor interrupt and the handler's port writes; "16 marked, 16 outside the picture" in the TV area); a hover tooltip ("line 299 col 33: OUT 03h = 08h (IOMEM slot, keyboard row), from CD1D") and its click listing CD1Dh, the ROM's keyboard scan; the IN toggle adding the 58h reads; the logo frame full of OUT marks; the Machine card (port 02h = 70h, 4-colour, palette black/green/red/blue, CRTC registers, pitch FAFh = 2411.3 Hz); a held A lighting row 4, column 6; the Machine card under the CPU card in a fresh layout and as a tab beside it in an older one. Not checked: the changed-value colours on the Machine card, full raster with markers, a program that changes the palette mid-frame.
- **Console and Agent**: three MCP calls over HTTP (`registers`, two failing `memory` reads) appearing live in the Agent lane with arguments, results, timing and the errors in yellow; the Console with `r` typed while running ("(stopped by the monitor)" and the registers), `break 0CAA3`, `x` and the stop report, Up arrow recalling `break 0CAA3`, and the run bar's Run echoed as `x`; the Agent text without an MCP server; a fresh layout with the Listing and Machine card in front. Not checked: an agent screenshot call in the lane, a TCP client and the Console at once.
- **Checkpoints and Watch**: the fresh layout; `CAA3` refused with the monitor's message under the form and in the strip, `0CAA3` added, the machine stopped at it with #1 in yellow and the form cleared; a condition `A==1` typed by double-click, after which F5 ran on with the hits unchanged; ignore 3 then F5: 5 hits, stopped again; disable and delete; a `store 0003` checkpoint stopping at FFFBh, its place opening Memory at 0000h; watches `HL`, `PC` and an unknown `U0` (red) added by typing, PC yellow after a step; six watches with `[`, `{` and `:` loaded from a seeded `layout.ini` (bytes at 0000h, `{SP}`, `[0003]` with its character, text at `SYS:0FA3`); "Stop on write" on the bytes watch giving `store 0000-0007`, Run stopping at FFFBh with the hit tooltip and three watches in yellow; a watch edited to `DE`; a log-only checkpoint counting about 100 000 hits while running at 100 %; Delete all with its second click. Not checked: line and port checkpoints from the form, a bank-offset place click, the text format's "Stop on write", other skins.
- **History and I/O log**: the History while stopped, newest at the bottom (`OR A` F=44, `LD HL,0EAFh` HL=0EAF, `DI` IFF=0, `EI` IFF=1), two F6 steps adding rows, the not-run row left out (the last row `LD HL,0BE5h` 10 T before the stop); the filter `ret` keeping 231 of 65 535 rows, a taken `RET Z` with its SP change and a not-taken one without; a row click opening the Listing at C3F1h. The I/O log stopped (2244 events) and running (following, 5440 after 1.5 s); port `58` with IN on (keyboard reads from CD1Fh); IRQ alone (one handler entry a frame at line 288–289); the hover text; a click opening the Listing at IT_ENTRY (0038h). `debug.core` checks the ring (also from the MinGW build under Wine). Not checked: the History with the trace off (`chis off`), a very long filter on a slow machine, other skins.
- **Video**: the BASIC screen in the picture while running, with the off-screen lines 240–255 dimmed and the info line "start 0000h, 64 × 60 characters, 4 lines each, cursor 0EFFh, 15 360 bytes"; a hover checked by hand (084Eh = 70h: row 8, line 1, column 14; dot 7 = palette 1 = IGRB C); a stop from `break line 64` putting the beam mark at display line 19, column 14 (frame line 68 − 49, beam column 39 − 25); 16-colour mode with the swatches greyed and its hover checked (60h, left dot IGRB 4); the bank list VID0–VID3 on 64k+ and VID1 without the cursor and beam marks; white chosen for palette 1; a click opening Memory at VID0:06C0. Not checked: a program that scrolls with R12/R13 or changes R1/R6/R9, other skins.
- **Profiler**: a typed BASIC loop (`FOR I=1 TO 400: PRINT I: NEXT`) recorded for 7.30 s of TVC time; the Routines table (EXT 1076h 53 % exclusive; SYS 0321h 73 % inclusive over 1113 calls; banks not paged shown by bank and offset); the Instructions view (the screen scroll's `LDIR` at F08Dh with 51 % over 485 760 repeats); a click opening the Listing at F08Dh with its 51.1 % and the neighbouring shares; a recording at the prompt giving the wait loop's shares in the Listing. Not checked: the "<0.1%" text on screen (no such row was in view), sorting by another column, other skins.
- **Source**: with the SDCC sample (`main.c`) loaded by `--moncommands` (`bload`, `ll main.noi`, PC at `main`): the Listing with `twice:`/`main:` and `counter` as operand names; the Source panel with line 7 in red and the code lines lit; Line in to line 3 in `twice()`, Shift+F6 to line 4, Line over back to line 7 at 8008h; a gutter click on line 8 (checkpoint #2 at 800Bh) and F5 stopping there; Load symbols... through a real zenity dialog loading `main.cdb` (the Console's `ll` line and "3 labels and 15 source lines (SDCC CDB)"). Not checked: an SLD or listing in the GUI (headless only), a source file that changes on disk, other skins.
- Not checked for Bench as a whole: other skins, a display scale other than 1, Windows.

## Settings window

**Settings...** in the main menu opens a sheet at the right of the window, from the top (Play) or below the run bar (Bench) down to the strip. Play's screen and Bench's dock space give up its width, so the picture stays visible and a change shows at once. Code: `frontend/sdl/settings_view.*`; it returns the same `PlayActions` as the menus (preset, cards, skin, language, mapping, mute...) plus its own `SettingsActions`.

- Materials: the deck colour with the groove along its left edge, a header band in the rib colour with **Close**; the sections as words down a column as wide as the longest name, the current one marked with a lit slot rib (the strip's motif) and the strong font. Escape closes it while it has the keys; while it has them they stay out of the TVC.
- **Machine**: the presets; the ROM folder in use, **Choose folder...** (writes `rom_dir`; the machine powers on only when the preset's ROMs differ), **Default places** (removes the key), **Download ROMs**; the disk card in slot 0 and its EPROM: VT-DOS 1.1 (`hbf.rom`), VT-DOS 1.2 (`hbf12.rom`) or an image file (a card that is in takes it at once and powers on); the other cards (the Cards menu's switches, each with slot buttons 0–3 in a column right of the names: the card's slot marked, a slot holding another card greyed out, its tooltip naming the slot's ports and card) and the SID card's chip.
- **Picture**: see "Picture"; the TVC's rear colour switch; full screen now and at start, full raster; what draws (OpenGL or SDL's renderer) and SDL's renderer from the next start.
- **Sound**: mute, volume (the stream's gain, saved as `volume`), sound output from the next start (`audio`).
- **Keyboard and joysticks**: symbolic or positional, the key layout and **Edit the layout...** (see "Keyboard editor"), the joystick keys.
- **Interface**: language, skin, the settings file's path and whether it is the portable one.
- **Remote**: the text monitor, MCP over HTTP (with its token and **Copy**, monitor commands allowed or not), DZRP (TVC banks or not), the GDB stub. Each is switched on or off and moved to another port at once (the monitor's listener closes with `MonitorHub::stop_listening()`, the others' hosts are rebuilt); the sheet shows the port each listens on or why it could not start. A server the command line asked for still ends the program when it cannot listen; one from `freetvc.ini` shows its error in the sheet.
- A value the command line decides shows "Set by --x for this run; a change here is saved" in the notice colour.

Checked on Xvfb (XTEST clicks, window grabs, stderr, a scratch `freetvc.ini`): the menu item; every section in Play and over Bench (the dock space narrower, the run bar above the sheet); the disk card switched in and to VT-DOS 1.2 from the sheet ("the disk card has its new EPROM; the machine powered on"; `disk_card = yes`, `disk_rom = 1.2` written at once) and both back at the next start; the text monitor switched on from the sheet and a TCP `r` answered on 6580, moved to 6590 (6590 answers, 6580 refused, `monitor = 6590`), started from `freetvc.ini` at the next start; MCP switched on, `tools/list` over HTTP with the token the sheet shows, `mcp = on`. Not checked: the folder and file dialogs from the sheet (zenity on Xvfb), the volume by ear (no audio on Xvfb), Windows.

## Picture

The GUI draws through **OpenGL 3.3 core** (`frontend/sdl/display_gl.cpp`, Dear ImGui's OpenGL3 backend, and our own function table `gl_api.hpp` loaded with `SDL_GL_GetProcAddress`, so no GL headers or library are needed at build time). Without a 3.3 context, or with `--renderer sdl` / `renderer = sdl`, it uses SDL's renderer (`display_sdl.cpp`) with sharp pixels only. `display.hpp` is the interface `main.cpp` and Bench use for textures and frames. Bench keeps sharp pixels: its event marks need them.

**The model** (`picture_shaders.hpp`) is written from the signal, not taken from other shaders:
- **Signal passes** at 25 MHz, two samples a TVC pixel (the framebuffer's 800 pixels a line are the 12.5 MHz clock); offscreen rows = raster lines. The TVC's encoder: Y, U = 0.492(B−Y), V = 0.877(R−Y), the subcarrier at 4.43361875 MHz (0.3547 cycles a pixel, 283.7516 a line), V inverted every other line. **Pass 1** demodulates U and V with the set's subcarrier (plus an optional phase error) and a Gaussian low-pass 3 dB down at `chroma.bandwidth`. **Pass 2** takes the luma: on a colour set the remodulated chroma is subtracted (the chroma trap; what it misses is cross-luminance), then a Gaussian at `luma.bandwidth`; a black and white set has no trap, so the subcarrier stays as fine dots in coloured areas. U and V go through the delay line (the line before averaged in), so a phase error becomes a saturation loss; without it, Hanover bars.
- **The rear switch** (`config.colour`, port 59h b6, `Machine::set_colour_switch()`, `colour_switch` in `freetvc.ini`): off, the TVC sends no subcarrier and no burst, so the set's colour killer turns its chroma path and trap off: a grey picture, measured exactly grey with the mask off.
- **Glow**: the light blurred wide in two passes at half resolution.
- **The tube**, drawn by an ImGui draw callback over the screen's rectangle: the signal as light (`gamma.crt`), each of the nearest raster lines a Gaussian beam whose width grows with brightness (`beam.min` to `beam.max`, normalised, so wider lines are not brighter), never narrower than the window's pixels can show (else the 287 lines alias into moiré on a small picture); RGB's horizontal spot over the sharp pixels; aperture grille or shadow mask at window pixels with brightness compensation; glow; curvature; vignette; output for an sRGB screen.
- **Files**: `data/pictures/*.picture` (copied next to the GUI as `pictures/`; `pal-tv.picture` names every key): `sharp`, `rgb-monitor`, `pal-tv`, `bw-tv`. `--picture NAME|FILE`, `picture` in `freetvc.ini`. The file in use is watched like the skin. The settings window's sliders (bandwidths, saturation, phase error, delay line, spot, beams, mask, glow, curvature, vignette, tube gamma, brightness) apply at once and, when let go, write the whole picture to `pictures/<name>.picture` in the user data folder, which then shadows the shipped file; **Back to the shipped values** removes that copy. `frontend/sdl/picture.*` parses them (test `ui.picture`).
- Screenshots stay the framebuffer's pixels.

Checked on Xvfb (Mesa llvmpipe, OpenGL 4.5 core) with window grabs (a GL window must be grabbed itself; a grab of the root window is black): the four pictures on the start-up logo at 100 % speed (PAL's colour smear and fringes, scanlines, shadow mask, glow, vignette zoomed in); `bw-tv`'s dots on coloured areas; the switch on B/W from `freetvc.ini` (grey, no dots); the moiré before and after the beam limit; a slider edit writing the user copy and taking effect; `--renderer sdl` sharp and identical to the GL sharp picture; Bench and its Video panel under GL; the Windows build under Wine (`drawing with OpenGL 4.5`, the same PAL picture). Not checked: a hardware GPU, macOS, a visual comparison with a real TV.

## Keyboard editor

`frontend/sdl/keymap.*` (library `freetvc_keymap`): a **key layout** is one binding per host key: the TVC key it presses and whether it forces SHIFT (Backspace = DEL with SHIFT up, Delete = DEL with SHIFT down). Host input (`host_input.*`) follows it: positional mapping follows the whole layout; symbolic mapping types a key's character when it has one, else follows the layout (AltGr excepted).

- Files `keymaps/<name>.keys`: `HOST = TVC [+shift|-shift]` or `HOST = none`; host keys by stable names (A, GRAVE, KP_8, RCTRL...; SDL's own names hold `=` and `;`), TVC keys by legend or word (`SEMICOLON` for `;`, JOY2_UP...).
- Shipped in `data/keymaps/`: `hungarian` (equal to the built-in table, checked by `ui.keymap`) and `us`.
- `--keys NAME|FILE`, `keys` in `freetvc.ini`, the settings window's list.

**Edit the layout...** opens a panel along the bottom, above the strip, where the keyboard sits under a TV. The settings sheet closes (the editor needs the width); Play's screen and Bench's dock space give up its height. `frontend/sdl/keyboard_editor.*`:
- the TVC keyboard from [tvc-hardware.md](../hardware/tvc-hardware.md#7-keyboard-and-joysticks): the letter and number keys in putty with ink legends (the shifted character small above), the modifiers (INS, ESC, DEL, RETURN, LOCK, SHIFT, CTRL, ALT) in charcoal; the two SHIFTs are one switch; joysticks 1 and 2 at the right as charcoal crosses with FIRE and ACCEL, like the built-in stick;
- under each legend the host keys that press it, as the user's own keyboard shows them (`SDL_GetKeyName`, or short words for keys that type nothing: Bksp, LShift, Kp8...);
- a click on a key waits for a host key (the key outlined and the header's hint in the notice colour); the next key press binds it (taken before ImGui and the TVC see it; F9–F12 and Pause stay reserved); a second click or a click beside the keys stops; a right click clears the key;
- host keys held down press their TVC keys in the picture (a key goes down a pixel and darkens); while the editor has the keys, the TVC gets none;
- each change goes at once into `keymaps/<name>.keys` in the user data folder, which shadows the shipped file; **Back to the shipped layout** removes it.

Checked on Xvfb: the section, the editor full-width in Hungarian; TVC Q clicked (yellow outline, the prompt), Tab pressed: Q shows "Q Tab", the user copy holds `TAB = Q`; W held down drawn pressed and not typed on the TVC; after closing, Tab Tab RETURN typed `qq` at the BASIC prompt; the input tests (`input.host`) unchanged. Not checked: the `us` layout typed on a real US keyboard, gamepads.

## Language

The UI has a Hungarian option next to English, for the menus and everything else the GUI shows. The TVC is a Hungarian machine and practically all of its software is Hungarian, so its users expect a Hungarian emulator.

- **Lookup**: every UI string goes through `tr()` (`frontend/sdl/text.hpp`), with the English text as the key (as in gettext) and as the fallback; `trc(context, text)` where one English word needs two translations (the tape's "Play" and Bench's "Play" button, context `mode`); `tr_format()` is printf with the translated format; `N_()` marks a literal that reaches `tr()` later (tables, `cond ? N_("a") : N_("b")`). The strip's notices go through `Notice::show()`/`show_text()`: English on stderr, the UI language in the strip.
- **Catalogs**: gettext `.po` files in `data/lang/` (`hu.po`, 764 strings), copied next to the GUI as `lang/`, read by our own parser (`frontend/sdl/translation.*`: escapes, continuation lines, msgctxt; fuzzy, untranslated and plural entries are left out). Header fields `Language` (the code) and `X-Language-Name` (the name in the menu, in its own language, e.g. "Magyar"). Catalogs stay loaded once used, so a pointer `tr()` returned stays valid after a switch.
- **Choice**: `--language CODE`, else `language` in `freetvc.ini`, else the first of the OS's preferred languages (`SDL_GetPreferredLocales`; on Linux `LANGUAGE`/`LC_*`/`LANG`) that is English or has a catalog, else English. The main menu's **Language** section lists English and each catalog by its own name and switches on the next frame; only a menu choice is written to `freetvc.ini`. A `freetvc.ini` language without a catalog is a strip notice, an unknown `--language` an exit with code 2.
- **Translated**: menus, the strip, every Bench panel, tooltips, the file dialogs' filter names, the no-ROMs message boxes, the GUI's own notices and the messages built in shared code (below). **English**: the text monitor and the Console's output, MCP, DZRP, GDB, `--help`, stderr, the monitor-only state descriptions (cards, printer, serial line), the TVC256k++ firmware's own screen texts, and OS error texts (`ec.message()`).
- **Shared messages**: `frontend/common` and `core` build their errors, warnings and strip descriptions with `msg(format, ...)` (`core/message.*`), printf into a string, so the monitor and MCP get plain English. The GUI sets a hook (`record_message`) that keeps the last 4096 results with their format and each conversion's text; `translate()` rebuilds one with `tr(format)` and its parts, each translated again when it is itself a recorded message (nested `%s: %s` errors), and leaves anything unrecorded in English. `Notice::set`/`show` and the message boxes go through it; `show()` builds its line with `vmsg()`, so a shared message among its arguments is translated too. A message cut before display (the ROM search's `; searched` list) is cut at a recorded boundary. On MinGW the format attribute is `gnu_printf`, matching its ANSI stdio.
- **Rule for new strings**: user-facing messages in `core/` or `frontend/common/` use `msg()`, GUI text uses `tr()`, and every new string gets its `hu.po` entry (the test `ui.translation_coverage` fails otherwise).
- **Wording** (`hu.po`): the Hungarian manuals' terms and [glossary.md](../glossary.md) (magnó, szalag, kazetta, lemez, kártyahely, botkormány, hidegindítás for "powered on"); product and chip names unchanged (GameCard, NanoSD, MultiCart, SD Dual, TVC256k++); Z80 and monitor words kept (IN, OUT, IRQ, HALT, `break`, `chis`); informal address (tegeződés); no article or suffix on a value, which would depend on how it sounds (`kinézet: %s`, `%s kivétele`, `GameCard: %d. kártyahely`); Bench's narrow form fields use short words (`futás`, `olvas`, `ír`, `Csak napló`, `Hozzáad`), because the long ones were cut.
- **Tools** ([tools/lang/README.md](../../tools/lang/README.md)): `python3 -I tools/lang/po_check.py pot src freetvc.pot` writes the template (`tr`, `trc`, `N_`, `tr_format`, `msg`, `.show`, `.show_text`); `msgmerge -U --backup=none data/lang/hu.po freetvc.pot` merges it (gettext tools); `po_check.py check src data/lang/*.po` is the test `ui.translation_coverage`: every string translated, not fuzzy, no stale entries, the same printf conversions in the same order, the same leading/trailing spaces and line breaks. `msgfmt --check` agrees. The parser, `msg()` against printf and the rebuild of nested messages are tested by `ui.translation`.
- **Fonts**: Atkinson Hyperlegible Next and Mono have every Hungarian letter (á é í ó ö ő ú ü ű, Ő Ű, „ ”; checked with `fc-query`), and ImGui 1.92 builds glyphs on demand, so no glyph ranges are needed; only ImGui's built-in fallback font (used when a font file is missing) lacks ő and ű.
- The documentation is in English; the language setting concerns only what the program shows.

Checked on Xvfb with XTEST input, window grabs and stderr:
- Play mode and its main, Cards, Machine and Tape menus in Hungarian (ő/ű zoomed in); Bench while stopped with the Screen, CPU, Machine, Watch, Checkpoints, Video, I/O log and Profiler panels; the strip notice "pillanatkép mentve: quick.tvcs" after F9 with the English line on stderr; a live switch to English from the menu and `language = en` in `freetvc.ini` afterwards; with no setting, `LANG=hu_HU.UTF-8` gives Hungarian and `LANG=en_US.UTF-8` English, and a saved `language = en` wins over `LANG=hu_HU.UTF-8`; a run that did not touch the menu wrote no `language` key; the no-ROMs message box (an empty `--rom-dir`) in Hungarian with Kilépés/Letöltés.
- Shared messages: a bad `freetvc.ini` line ("freetvc.ini, 1. sor: ismeretlen kulcs: 'foo'"), `gamecard = yes` without `gamecard.rom` (the GameCard note with its nested "nincs gamecard.rom ..." part) and a damaged `quick.tvcs` loaded with Shift+F9 ("quick.tvcs: nem freeTVC-pillanatkép ..."), each in Hungarian in the strip and in English on stderr, with the monitor and MCP tests unchanged; `translation_test` from MinGW under Wine (`msg()` against MinGW's printf).
- Not checked: the file dialogs' filter names (zenity on Xvfb), every tooltip, the Hungarian text on Windows. The Hungarian wording has not had a native speaker's full review.

## About window

Main menu → *About freeTVC...* (`src/frontend/sdl/about_view.*`): a black panel over the dimmed window with the L@M3rZ logo (`data/about/lamerz-logo.png`, resampled with SDL to its size on screen and shrunk so the panel fits the window), copper raster bars and a sine scroller in the TVC's own bright colours, the version, the build (commit, `(modified)` for a changed tree, commit date; generated at every build by `cmake/FreeTVCBuildInfo.cmake`), the build date and compiler, platform, SDL version and renderer, the credits (LG / L@M3rZ, Dey / L@M3rZ), the licence and the repository link. *Copy build info* puts the same lines in English on the clipboard for bug reports; Esc, *Close* or a click outside closes it. The logo is under the project's licence, GPL-3.0-or-later.

## Settings file and ROM search

Code: the ROM search in `frontend/common/presets.{hpp,cpp}` (`rom_places`, `find_preset_roms`, `program_directory`, `user_data_directory`; test `frontend.rom_search`), the settings file in `frontend/common/settings.{hpp,cpp}` (test `frontend.settings`). The ROM autofetch ([build.md](build.md), section "ROM autofetch") fills `roms/` next to the programs at build time, or `roms/` in the user data folder from the programs: the GUI offers it in a message box when it finds no ROMs at start, and under the machine menu's **Download ROMs**, which fills the `rom_dir` folder instead when one is set.

### ROM search

- **ROM folder**, first match wins:
  1. `--rom-dir`;
  2. `FREETVC_ROM_DIR`;
  3. `rom_dir` in the settings file;
  4. `roms/` next to the executable: the release layout, so a release package starts with no setup;
  5. `roms/` in the user data folder (`~/.local/share/freeTVC/roms`, `%APPDATA%\freeTVC\roms`): for source builds and ROM-free packages.

  A folder is used only when it has both of the preset's ROMs. If none has them, the error lists every place searched.
- **File names**: a file under the expected name (`sys12/21/22.bin`, `ext1x/2x.bin`) is taken as it is; otherwise the folder's files up to the ROM's size are checked by CRC-32 in name order against the known TVC ROMs (the CRC-32s MAME lists): SYS 1.2/2.1/2.2, EXT 1.x as 4 KiB or as MAME's 8 KiB socket image, EXT 2.x. So renamed dumps from other emulators or EPROM readers work. Card ROMs are found the same way.
- The GUI takes the program and user data folders from SDL (`SDL_GetBasePath`, `SDL_GetPrefPath`), headless from `/proc/self/exe` or `GetModuleFileNameW` and the same user data folder rule. The GUI prints where its ROMs came from on stderr. A preset switched to in the menu is searched the same way; when it fails, the strip shows the first line and stderr the places.
- No launcher scripts (`.sh`, `.bat`) are needed: the search finds the ROMs.

### `freetvc.ini`

`key = value` lines, `;` comments: the same format and parser as the skins.

- **Where**: portable when present. A `freetvc.ini` next to the executable is used (a release folder on a USB stick keeps its settings); otherwise the one in the user data folder. `layout.ini` follows the same rule (beside `freetvc.ini`).
- **Reading**: the GUI reads it before the command line, so its values are the defaults and options override them for one run. A bad line or unknown key is reported in the strip and on stderr and skipped; a later line wins over an earlier one; an unknown preset falls back to 64k+-2.2. A relative path (such as `rom_dir`) is read against the file's folder, so a portable release can say `rom_dir = roms`.
- **Headless** reads only `rom_dir` (for the ROM search): a GUI session must never steer a headless run.
- **Writing**: the GUI writes the machine's state (preset, skin, keymap, the cards and their media) when it changes (checked once a second, every frame while the settings window is open), and the settings window writes its own keys at once. A card key is written once that card was ever on or the file already has it. A value given on the command line (for the disk card also `--disk`, `--disk-rom` and a disk autostart) is written only if the menu changed it during the run. `language` is written only after a menu choice. `full_raster` is only read. A serial cable to the host peer (`on`) is saved as `off`, since its files are not saved.
- Writing keeps the file: the lines of those keys are rewritten in place (with their comments), every other line stays, missing keys are appended, and a new file starts with a short comment naming the keys. An empty value removes the key (for `rom_dir`: back to the default places). The file is written to `freetvc.ini.new` and renamed.
- GUI checks run with a scratch `XDG_DATA_HOME` (`tools/ui/ui_drive.py` sets one unless given), so they never touch a user's own files.

| Key | Values | Meaning |
|---|---|---|
| `rom_dir` | folder | the ROM folder (step 3 of the search) |
| `preset` | `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`, `64k+-2.1`, `64k+-2.2` | machine and ROM set |
| `skin` | name or path | the skin |
| `keymap` | `symbolic`, `positional` | keyboard mapping |
| `keys` | name or path | the host key layout ("Keyboard editor") |
| `language` | `en`, `hu` | UI language |
| `audio` | yes/no | sound output at start |
| `volume` | 0–100 | the stream's gain |
| `fullscreen` | yes/no | start in full screen |
| `full_raster` | yes/no | start with the full raster |
| `renderer` | `gl`, `sdl` | what draws ("Picture") |
| `picture` | name or path | the picture file |
| `colour_switch` | `colour`, `bw` | the TVC's rear switch |
| `disk_card` | yes/no | the HBF card in slot 0 |
| `disk_rom` | `1.1`, `1.2`, file | its EPROM (VT-DOS 1.1 `hbf.rom`, VT-DOS 1.2 `hbf12.rom`, or an image) |
| `gamecard` | yes/no/0–3 | the GameCard in slot 1 (`yes`) or that slot; gamepads 3 and 4 are its joysticks 3 and 4 |
| `multichip` | yes/no/0–3 | the Multichip sound card in slot 2 (`yes`) or that slot |
| `sid_card` | `off`, `6581`, `8580` | the SID card at ports E0h–FFh |
| `card256` | yes/no/0–3 | the TVC256k++ in slot 0 or the first free one (`yes`), or that slot |
| `card256_usb` | `none`, `mouse`, folder | its USB port |
| `card256_flash` | image or folder | its flash drive |
| `nanosd` | yes/no/0–3 | the NanoSD in slot 3 (`yes`) or that slot |
| `nanosd_sd` | `none`, folder | its SD card |
| `nanosd_menu`, `nanosd_sort` | yes/no; `none`, `name`, `size` | the Nano's EEPROM: the file-select menu at boot and its sort order |
| `printer` | `off`, `on`, file | the printer (`on` keeps the output in memory) |
| `serial_card` | yes/no/0–3 | the serial card in slot 1 (`yes`) or that slot |
| `serial_line` | `off`, `on`, `listen[:PORT]`, `connect:PORT` | its cable |
| `serial_flow` | `rts`, `none` | its flow control |
| `monitor`, `mcp`, `dzrp`, `gdb` | `off`, `on`, port | the remote servers at start |
| `mcp_allow_monitor` | yes/no | MCP's `monitor_command` allowed |
| `dzrp_banks` | yes/no | DZRP reports the TVC banks |

### Checks

In the real GUI (scratch user data folders): the first exit writes the commented file; a hand-edited file (comment, `rom_dir` with a comment, 64k-1.2, `deck`, positional) starts that preset, skin and mapping, the ROMs "from … (freetvc.ini)", and keeps every line on exit; `--skin bronze` is used but `skin = deck` stays, while the mapping clicked in the strip is saved; a `freetvc.ini` next to the binary is read and updated there and `layout.ini` written beside it, with the user data folder left empty; headless takes `rom_dir` from the file. `settings_test.exe` passes under Wine.

ROM search: headless with nothing anywhere (the error lists both default places); renamed files (`TVC22_D6.64`, `TVC22_D7.64`) in the user data folder booting headless and in the GUI (stderr: "ROMs from … (user data folder)", window grab at the BASIC prompt); a copied headless binary started from `/` with `roms/` beside it; under Wine, `rom_search_test.exe` and the Windows headless binary with renamed ROMs beside it, its screenshot byte-identical to Linux. Not checked: the GUI under Windows, macOS.

## Skins

Everything visual is a skin value: colours, role colours, fonts, font sizes and metrics. A skin is a plain text file `<name>.skin`:

```
; comments start with ';'
name = bronze
colour.frame = #2a231e
role.exec = red            ; a TVC colour name: bright on case, dark on putty
role.read = #00ff00 #009200 ; or two colours: on case, on putty
font.ui = AtkinsonHyperlegibleNext-Regular.ttf
font.ui.size = 15
metric.strip.height = 30
```

- Keys missing from a file keep the built-in `bronze` value, so a skin can change a single colour.
- Search order: `skins/` in the user data folder, then `skins/` next to the executable. Font files are looked up next to the skin file, then in `fonts/` next to the executable.
- The skin file in use is **watched** (every 0.5 s): saving it in an editor applies it at once (fonts included). A bad line keeps the last good skin and shows the error in the strip.
- `--skin NAME|PATH` picks one at start; the main menu lists every skin found.
- Shipped skins: `bronze` (default; `data/skins/bronze.skin` equals the built-in values, checked by a test) and `deck`, the full light skin: every case surface in putty tones with dark lettering, light data panels (`panel.light = yes`), and the TVC colours' dark (I = 0) variants everywhere; its `role.notice` is darker than the I = 0 yellow, which is too faint on the light case.
- `data/skins/` is copied next to the GUI whenever a skin file changes (target `freetvc_skins`), not only when the program is relinked, so an edited skin is never stale in the build folder.
- `panel.light = yes|no` chooses the data panels' look; `colour.panel` is their dark surface.

Key reference: `data/skins/bronze.skin` lists every key with a comment.

## Code

`src/frontend/sdl/`:
- `skin.{hpp,cpp}` (library `freetvc_skin`): the `Skin` values (member defaults = `bronze`), the parser, skin discovery. No SDL or ImGui, so `tests/sdl/skin_test.cpp` (`ui.skin`) tests it, including that `data/skins/bronze.skin` equals the built-in values.
- `skin_style.{hpp,cpp}`: a skin to every `ImGuiStyle` colour and size, and the fonts. A font change clears the whole atlas (`ImFontAtlas::Clear()`): `ClearFonts()` alone leaves stale glyph pixels. Fonts are loaded before the style is applied, because clearing the atlas writes the old size back into `style.FontSizeBase`.
- `play_view.{hpp,cpp}`: screen, strip, the five strip menus; takes a `PlayStatus`, returns `PlayActions`, touches nothing else. `draw_strip` is shared with Bench.
- `deck.{hpp,cpp}`: the screen picture (`draw_screen`, `ScreenRect`) and `DeckRow`, the row of deck items used by the strip and the run bar.
- `bench_view.{hpp,cpp}`: `BenchView` (run bar, page column, dock space and every Bench panel); reads the machine through the `Monitor`, returns `BenchActions` that `main.cpp` runs as monitor commands.
- `text.{hpp,cpp}`, `translation.{hpp,cpp}`: `tr()` and its relatives, the `.po` loader ("Language").
- `settings_view.*`: the settings window.
- `display.*`, `display_impl.hpp`, `display_gl.cpp`, `display_sdl.cpp`, `gl_api.hpp`, `picture.*`, `picture_shaders.hpp`: drawing and the picture model.
- `keymap.*`, `keyboard_editor.*`, `host_input.*`: key layouts, their editor, host keyboard and gamepads.
- The docking layout lives in `layout.ini` (ImGui's ini file plus the Watch list); the default arrangement is built with ImGui's dock builder only where the file has none.
- `main.cpp`: loop, pacing (pause-aware), reserved keys, skin and picture watching, settings saving, actions.

GUI checks: `tools/ui/ui_drive.py` starts the GUI and runs steps (XTEST keys and clicks including `dclick`/`rclick`, window grabs, shell commands for skin edits); see [tools/ui/README.md](../../tools/ui/README.md). Runs that may open a file dialog go under `dbus-run-session` with `SDL_FILE_DIALOG_DRIVER=zenity`, so the dialog opens on the virtual display.

## Limitations

- Overscan: the TV area has none; a real set cuts another 5–10 %.
- No "default layout" command: deleting `layout.ini` restores the default arrangement.
- Text that stands for the TVC's own screen is drawn in the UI fonts. The TVC font (SYS ROM C574h–C8D3h, 10 bytes per character, [rom.md](../hardware/rom.md)) is not used; if it ever is, it must be read at run time from the user's ROM and never shipped.
- Not checked anywhere in the GUI: a display scale other than 1, Windows on real hardware, macOS.
