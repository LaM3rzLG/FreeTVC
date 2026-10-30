// SPDX-License-Identifier: GPL-3.0-or-later
// Host service of operating-system calls (design/media.md): the machine offers every instruction at the RST 30h
// entry (0030h) to an attached handler, which may perform the call itself and return past the function byte.
#pragma once

#include <cstdint>

namespace freetvc {

class Machine;

inline constexpr std::uint16_t kOsEntry = 0x0030;

class OsCallHandler {
public:
    OsCallHandler() = default;
    OsCallHandler(const OsCallHandler&) = delete;
    OsCallHandler& operator=(const OsCallHandler&) = delete;
    virtual ~OsCallHandler() = default;

    // At the boundary before the instruction at 0030h, after the debugger had its look. A handler that performs the
    // call moves execution with Machine::return_to(); one that declines must leave the machine untouched.
    virtual void on_os_call(Machine& machine) = 0;
};

}  // namespace freetvc
