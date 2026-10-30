// SPDX-License-Identifier: GPL-3.0-or-later
// What the machine hands an attached printer (design/machine.md "Printer port").
#pragma once

#include <cstdint>

namespace freetvc {

class PrinterPort {
public:
    virtual ~PrinterPort() = default;
    // /STROBE fell at `tick` (half-T) with `value` on the data lines; the printer's /ACK follows on its own.
    virtual void printer_byte(std::uint64_t tick, std::uint8_t value) = 0;
};

}  // namespace freetvc
