// SPDX-License-Identifier: GPL-3.0-or-later
// What the monitor and MCP may do with program media; the front end implements it with host files.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/media/media.hpp"

namespace freetvc {

class MediaControl {
public:
    virtual ~MediaControl() = default;
    // A CAS file becomes the attached cassette; false with `error` set.
    virtual bool attach(const std::string& path, std::string& error) = 0;
    virtual void detach() = 0;
    // attach(), then a fast autostart (Media::autostart).
    virtual bool autostart(const std::string& path, std::string& error) = 0;
    // One line: the attached file and the folder.
    virtual std::string describe() const = 0;
    // Attach warnings and the cassette's opens, saves and problems since the last call.
    virtual std::vector<std::string> take_messages() = 0;
    virtual Media& media() = 0;

    // The tape deck. A WAV, TTP or CAS file goes in as a tape, stopped at its start; false with `error` set.
    virtual bool tape_insert(const std::string& path, std::string& error) = 0;
    // tape_insert(), then an autostart through the deck (Media::autostart_tape).
    virtual bool tape_autostart(const std::string& path, std::string& error) = 0;
    // A blank tape, recording; the recording goes to `path` as a WAV file at tape_stop(), tape_eject() or the end.
    virtual bool tape_record(const std::string& path, std::string& error) = 0;
    virtual void tape_play() = 0;
    virtual bool tape_stop(std::string& error) = 0;
    virtual void tape_rewind() = 0;
    virtual bool tape_eject(std::string& error) = 0;
    // One line: the tape, its state and counter.
    virtual std::string tape_describe() const = 0;

    // Cartridges: the image goes in and the machine powers on. `type` is "" (by size), "plain", "kilocart" or
    // "megacart"; `bank` is a plain board's switch setting. False with `error` set.
    virtual bool cart_insert(const std::string& path, const std::string& type, unsigned bank, std::string& error) = 0;
    // Takes the cartridge out and powers on.
    virtual void cart_remove() = 0;
    // One line: the cartridge, its type and page.
    virtual std::string cart_describe() const = 0;
    // A MultiCart v2 image at `out` built from the firmware in `language` and the files; it does not go in.
    virtual bool multicart_build(const std::string& out, const std::vector<std::string>& files, const std::string& language,
                                 std::string& error) = 0;

    // The SD Dual cartridge: in (its firmware sddual.rom, or sddual-gm.rom with `gamer`, from the ROM search) or
    // out, and the machine powers on; it takes the cartridge port. False with `error` set.
    virtual bool sddual(bool on, bool gamer, std::string& error) = 0;
    virtual bool sddual_on() const = 0;
    // A raw SD card image into socket 0 (microSD) or 1 (SD, with the write-protect switch); the cartridge sees the card
    // go in. The sockets keep their cards while the cartridge is out. False with `error` set.
    virtual bool sd_insert(unsigned socket, const std::string& path, std::string& error) = 0;
    virtual void sd_eject(unsigned socket) = 0;
    // The cartridge, then one line per socket.
    virtual std::string sd_describe() const = 0;
    // A new SD card image at `out` of `mib` MiB with one FAT partition holding the host files and folders; it does not go in.
    virtual bool sd_build(const std::string& out, std::uint64_t mib, const std::vector<std::string>& files, std::string& error) = 0;

    // Snapshots: the machine, plus the tape by path and position and the cassette by path. Loading may change
    // the board and ROM set (found by CRC-32) and powers nothing on; false with `error` set.
    virtual bool snapshot_save(const std::string& path, std::string& error) = 0;
    virtual bool snapshot_load(const std::string& path, std::string& error) = 0;

    // Rewind by time: back to the newest kept point at least `seconds` of machine time ago; newer points are
    // dropped, so asking again goes further back. False with `error` set when nothing is kept that far.
    virtual bool rewind(unsigned seconds, std::string& error) = 0;
    // One line: how much is kept.
    virtual std::string rewind_describe() const = 0;
    // After every run slice: keeps a rewind point each second of machine time, and saves written disks once idle.
    virtual void after_slice() = 0;

    // The disk card: the HBF card in slot 0 goes in or out, and the machine powers on. False with `error` set.
    virtual bool disk_card(bool on, std::string& error) = 0;
    virtual bool disk_card_on() const = 0;
    // A raw sector image into drive 0-3 (A-D); the card goes in first when it is out. False with `error` set.
    virtual bool disk_insert(unsigned drive, const std::string& path, std::string& error) = 0;
    // Writes a disk the machine changed back to its file, then takes it out.
    virtual bool disk_eject(unsigned drive, std::string& error) = 0;
    // disk_insert() into A:, then a fast autostart that LOADs the first CAS file in its root directory.
    virtual bool disk_autostart(const std::string& path, std::string& error) = 0;
    // The card, then one line per drive.
    virtual std::string disk_describe() const = 0;

    // The GameCard: into `slot` or out, and the machine powers on; without its EPROM image it still has its
    // joysticks and sound. False with `error` set.
    virtual bool gamecard(bool on, unsigned slot, std::string& error) = 0;
    // Its slot, or -1 when it is out.
    virtual int gamecard_slot() const = 0;
    virtual std::string gamecard_describe() const = 0;

    // The Multichip sound card: into `slot` or out, and the machine powers on; without its EEPROM image the chips
    // still play. False with `error` set.
    virtual bool multichip(bool on, unsigned slot, std::string& error) = 0;
    // Its slot, or -1 when it is out.
    virtual int multichip_slot() const = 0;
    virtual std::string multichip_describe() const = 0;

    // The SID card: "6581" or "8580" puts that chip in at ports E0h-FFh (outside the slots), "off" takes it
    // out; the machine powers on. False with `error` set.
    virtual bool sid_card(const std::string& model, std::string& error) = 0;
    // "off", "6581" or "8580"
    virtual std::string sid_card_model() const = 0;
    virtual std::string sid_card_describe() const = 0;

    // The TVC256k++: into `slot` (kCardSlots: slot 0, else the first free one) or out, and the machine powers
    // on. Not on the 32k + HBM board. False with `error` set.
    virtual bool card256(bool on, unsigned slot, std::string& error) = 0;
    // Its slot, or -1 when it is out.
    virtual int card256_slot() const = 0;
    virtual std::string card256_describe() const = 0;
    // Its USB port: "none", "mouse", or a folder that becomes the pendrive. False with `error` set.
    virtual bool card256_usb(const std::string& what, std::string& error) = 0;
    // Its flash drive: a littlefs image, or a folder packed into one. False with `error` set.
    virtual bool card256_flash(const std::string& path, std::string& error) = 0;
    // A mouse report: movement (y down) and HID buttons (b0 left, b1 right, b2 middle); the mouse must be plugged in.
    virtual bool card256_mouse(int dx, int dy, unsigned buttons, std::string& error) = 0;

    // The NanoSD: into `slot` or out, and the machine powers on. False with `error` set.
    virtual bool nanosd(bool on, unsigned slot, std::string& error) = 0;
    // Its slot, or -1 when it is out.
    virtual int nanosd_slot() const = 0;
    virtual std::string nanosd_describe() const = 0;
    // Its SD card: a host folder, or "none" for an empty socket; the Nano sees the card go in or out. False with `error` set.
    virtual bool nanosd_sd(const std::string& dir, std::string& error) = 0;

    // The printer: plugged in with its output to `raw` and `text` files (either may be empty; the last 64 KiB
    // are always kept), or unplugged, the default. False with `error` set.
    virtual bool printer_plug(const std::string& raw, const std::string& text, std::string& error) = 0;
    virtual void printer_unplug() = 0;
    virtual bool printer_plugged() const = 0;
    // What was printed since the last call, raw
    virtual std::string printer_take() = 0;
    virtual std::string printer_describe() const = 0;

    // The serial card: into `slot` or out, and the machine powers on. False with `error` set.
    virtual bool serial_card(bool on, unsigned slot, std::string& error) = 0;
    virtual int serial_slot() const = 0;
    // Its cable: "off", "on" (the host is the peer), "listen[:PORT]", "connect:PORT"
    virtual bool serial_peer(const std::string& spec, std::string& error) = 0;
    virtual std::string serial_peer_name() const = 0;
    // true: the peer sends only while the TVC's RTS is on
    virtual void serial_flow(bool rts) = 0;
    virtual bool serial_rts_flow() const = 0;
    // Bytes, or a file's bytes, for the TVC to receive
    virtual void serial_queue(std::string_view bytes) = 0;
    virtual bool serial_queue_file(const std::string& path, std::string& error) = 0;
    // A file that gets everything the TVC sends; empty stops it
    virtual bool serial_capture(const std::string& path, std::string& error) = 0;
    // What the TVC sent since the last call
    virtual std::string serial_take() = 0;
    virtual std::string serial_describe() const = 0;
};

}  // namespace freetvc
