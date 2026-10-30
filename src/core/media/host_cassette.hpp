// SPDX-License-Identifier: GPL-3.0-or-later
// Host cassette (design/media.md): performs the cassette class's RST 30h functions from an attached CAS file and a
// host folder, so the ROM's own LOAD, SAVE, OPEN and INPUT # work unchanged. ABI: hardware/rom.md §12.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/media/cas.hpp"

namespace freetvc {

class Machine;

// Files by TVC name; the front end implements it (the core has no file I/O).
class CassetteFolder {
public:
    virtual ~CassetteFolder() = default;
    // `name` is converted (tvc_file_name). Returns the file's bytes and sets `found` to its TVC name, or nothing.
    virtual std::optional<std::vector<std::uint8_t>> read(std::string_view name, std::string& found) = 0;
    // A complete CAS file; false with `error` set when nothing can be written.
    virtual bool write(std::string_view name, std::span<const std::uint8_t> cas, std::string& error) = 0;
    virtual bool writable() const = 0;
};

class HostCassette {
public:
    // The cassette for OPEN with an empty or any name; `name` is its TVC name.
    void attach(std::string name, CasFile file);
    void detach();
    bool attached() const { return attached_.has_value(); }
    const std::string& attached_name() const { return attached_name_; }
    const std::optional<CasFile>& attached_file() const { return attached_; }
    // nullptr: no folder. The folder must outlive the attachment.
    void set_folder(CassetteFolder* folder) { folder_ = folder; }
    // With nothing attached and no folder the ROM's own tape routines run.
    bool active() const { return attached_ || folder_; }

    // Performs `code` (the caller's function code, routed to the built-in cassette) at the 0030h boundary and returns
    // past the function byte; false leaves the machine untouched for the ROM.
    bool call(Machine& machine, std::uint8_t code);

    // Unbuffered blocks read in full so far: LOAD's program body (BASIC's error handler also closes files).
    std::uint64_t blocks_read() const { return blocks_read_; }
    // One line per open, save and problem, for the front end's log.
    std::vector<std::string> take_messages();

private:
    struct ReadFile {
        CasFile file;
        std::size_t pos = 0;  // buffered: into the stream; unbuffered: into the 16 header bytes
        bool block_done = false;
    };
    struct WriteFile {
        std::string name;
        bool buffered = false;
        bool protect = false;
        std::vector<std::uint8_t> stream;  // unbuffered: header bytes, then the block
        std::size_t header_bytes = 0;
        bool block_done = false;
    };

    std::optional<std::uint8_t> open_read(Machine& m, std::uint16_t& de);
    std::optional<std::uint8_t> create(Machine& m, std::uint16_t& de);
    std::uint8_t finish_write(Machine& m);

    std::optional<CasFile> attached_;
    std::string attached_name_;
    CassetteFolder* folder_ = nullptr;
    std::optional<ReadFile> read_;
    std::optional<WriteFile> write_;
    std::uint64_t blocks_read_ = 0;
    std::vector<std::string> messages_;
};

}  // namespace freetvc
