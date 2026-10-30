// SPDX-License-Identifier: GPL-3.0-or-later
// Source maps from real debug files: sjasmplus 1.24.0 SLD and listing, SDCC 4.2.0 NoICE/CDB/map, z88dk 2.4 maps.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

#include "debug/source_map.hpp"
#include "debug/symbols.hpp"

namespace {

using namespace freetvc::debug;

int g_failures = 0;

#define CHECK(cond)                                         \
    do {                                                    \
        if (!(cond)) {                                      \
            ++g_failures;                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                   \
    } while (false)

// prog.asm: a macro called on line 11 and inc.asm included on line 13 (sjasmplus --sld --lst --lstlab, DEVICE NOSLOT64K)
constexpr std::string_view kSld = R"(|SLD.data.version|1
prog.asm|1||0|-1|-1|Z|pages.size:65536,pages.count:32,slots.count:1,slots.adr:0
prog.asm|7||0|0|32768|F|start
prog.asm|7||0|0|32768|L|,start,,+used
prog.asm|8||0|0|32768|T|
prog.asm|9||0|0|32770|T|
prog.asm|10||0|0|32773|F|start.loop
prog.asm|10||0|0|32773|L|,start,loop,+local,+used
prog.asm|11||3|0|32773|T|
prog.asm|11||4|0|32775|T|
prog.asm|12||0|0|32777|T|
inc.asm|1||0|0|32779|F|helper
inc.asm|1||0|0|32779|L|,helper,,+used
inc.asm|2||0|0|32779|T|
inc.asm|3||0|0|32780|T|
prog.asm|14||0|-1|4660|D|value
prog.asm|14||0|-1|4660|L|,value,,+equ
)";
constexpr std::string_view kList = R"(# file opened: prog.asm
 1    0000                  DEVICE NOSLOT64K
 2    0000                  MACRO twice x
 3    0000 ~                ld b,x
 4    0000 ~                ld c,x
 5    0000                  ENDM
 6    0000                  ORG 8000h
 7    8000              start:
 8    8000 3E 05            ld a,5
 9    8002 CD 0B 80         call helper
10    8005              .loop:
11    8005                  twice 3
11    8005 06 03       >    ld b,3
11    8007 0E 03       >    ld c,3
12    8009 18 FA            jr .loop
13    800B                  INCLUDE "inc.asm"
# file opened: inc.asm
 1+   800B              helper:
 2+   800B 3C               inc a
 3+   800C C9               ret
 4+   800D
# file closed: inc.asm
14    800D              value EQU 1234h
15    800D                  SAVEBIN "prog.bin", start, $-start
16    800D
# file closed: prog.asm

Value    Label
------ - -----------------------------------------------------------
0x800B   helper
0x8000   start
0x8005   start.loop
0x1234 X value
)";

// main.c: twice() on lines 2-4, main() on lines 5-10 (sdcc -mz80 --debug --code-loc 0x8000 --data-loc 0x9000)
constexpr std::string_view kNoi = R"(DEF .__.ABS. 0x0
DEF l__CABS 0x0
DEF l__DABS 0x0
DEF l__GSFINAL 0x0
DEF l__GSINIT 0x0
DEF l__HOME 0x0
DEF l__INITIALIZED 0x0
DEF l__INITIALIZER 0x0
DEF s__CABS 0x0
DEF s__DABS 0x0
DEF l__DATA 0x1
DEF l__CODE 0x11
DEF s__CODE 0x8000
DEF s__DATA 0x9000
DEF s__GSFINAL 0x9001
DEF s__GSINIT 0x9001
DEF s__HOME 0x9001
DEF s__INITIALIZED 0x9001
DEF s__INITIALIZER 0x9001
DEF A$main$59 0x8000
DEF C$main.c$2$0_0$2 0x8000
DEF C$main.c$3$1_0$2 0x8000
DEF G$twice$0$0 0x8000
DEF _twice 0x8000
DEF A$main$67 0x8001
DEF C$main.c$4$1_0$2 0x8001
DEF XG$twice$0$0 0x8001
DEF A$main$81 0x8002
DEF C$main.c$5$1_0$5 0x8002
DEF C$main.c$7$3_0$6 0x8002
DEF G$main$0$0 0x8002
DEF _main 0x8002
DEF A$main$82 0x8005
DEF A$main$83 0x8008
DEF A$main$87 0x800B
DEF C$main.c$8$3_0$6 0x800B
DEF A$main$88 0x800E
DEF A$main$96 0x800F
DEF C$main.c$10$2_0$5 0x800F
DEF XG$main$0$0 0x800F
DEF Fmain$counter$0_0$0 0x9000
LOAD main.ihx
)";
constexpr std::string_view kCdb = R"(M:main
F:G$twice$0_0$0({2}DF,SC:U),C,0,0,0,0,0
S:Lmain.twice$x$1_0$0({1}SC:U),R,0,0,[c]
F:G$main$0_0$0({2}DF,SV:S),C,0,0,0,0,0
S:Fmain$counter$0_0$0({1}SC:U),E,0,0
S:G$twice$0_0$0({2}DF,SC:U),C,0,0
S:G$main$0_0$0({2}DF,SV:S),C,0,0
L:A$main$59:8000
L:C$main.c$2$0_0$2:8000
L:C$main.c$3$1_0$2:8000
L:G$twice$0$0:8000
L:A$main$67:8001
L:C$main.c$4$1_0$2:8001
L:XG$twice$0$0:8001
L:A$main$81:8002
L:C$main.c$5$1_0$5:8002
L:C$main.c$7$3_0$6:8002
L:G$main$0$0:8002
L:A$main$82:8005
L:A$main$83:8008
L:A$main$87:800B
L:C$main.c$8$3_0$6:800B
L:A$main$88:800E
L:A$main$96:800F
L:C$main.c$10$2_0$5:800F
L:XG$main$0$0:800F
L:Fmain$counter$0_0$0:9000
)";
constexpr std::string_view kSdccMap = R"(ASxxxx Linker V03.00 + NoICE + sdld,  page 1.
Hexadecimal  [32-Bits]

Area                                    Addr        Size        Decimal Bytes (Attributes)
--------------------------------        ----        ----        ------- ----- ------------
.  .ABS.                            00000000    00000000 =           0. bytes (ABS,CON)

      Value  Global                              Global Defined In Module
      -----  --------------------------------   ------------------------
     00000000  .__.ABS.                           main
     00000000  l__CABS                         
     00000000  l__DABS                         
     00000000  l__GSFINAL                      
     00000000  l__GSINIT                       
     00000000  l__HOME                         
     00000000  l__INITIALIZED                  
     00000000  l__INITIALIZER                  
     00000000  s__CABS                         
     00000000  s__DABS                         
     00000001  l__DATA                         
     00000011  l__CODE                         
     00008000  s__CODE                         
     00009000  s__DATA                         
     00009001  s__GSFINAL                      
     00009001  s__GSINIT                       
     00009001  s__HOME                         
     00009001  s__INITIALIZED                  
     00009001  s__INITIALIZER                  

ASxxxx Linker V03.00 + NoICE + sdld,  page 2.
Hexadecimal  [32-Bits]

Area                                    Addr        Size        Decimal Bytes (Attributes)
--------------------------------        ----        ----        ------- ----- ------------
_CODE                               00008000    00000011 =          17. bytes (REL,CON)

      Value  Global                              Global Defined In Module
      -----  --------------------------------   ------------------------
     00008000  A$main$59                          main
     00008000  C$main.c$2$0_0$2                   main
     00008000  C$main.c$3$1_0$2                   main
     00008000  G$twice$0$0                        main
     00008000  _twice                             main
     00008001  A$main$67                          main
     00008001  C$main.c$4$1_0$2                   main
     00008001  XG$twice$0$0                       main
     00008002  A$main$81                          main
     00008002  C$main.c$5$1_0$5                   main
     00008002  C$main.c$7$3_0$6                   main
     00008002  G$main$0$0                         main
     00008002  _main                              main
     00008005  A$main$82                          main
     00008008  A$main$83                          main
     0000800B  A$main$87                          main
     0000800B  C$main.c$8$3_0$6                   main
     0000800E  A$main$88                          main
     0000800F  A$main$96                          main
     0000800F  C$main.c$10$2_0$5                  main
     0000800F  XG$main$0$0                        main
ASxxxx Linker V03.00 + NoICE + sdld,  page 3.
Hexadecimal  [32-Bits]

Area                                    Addr        Size        Decimal Bytes (Attributes)
--------------------------------        ----        ----        ------- ----- ------------
_DATA                               00009000    00000001 =           1. bytes (REL,CON)

      Value  Global                              Global Defined In Module
      -----  --------------------------------   ------------------------
     00009000  Fmain$counter$0_0$0                main

ASxxxx Linker V03.00 + NoICE + sdld,  page 4.

Files Linked                              [ module(s) ]

main.rel                                  [ main ]

ASxxxx Linker V03.00 + NoICE + sdld,  page 5.

User Base Address Definitions

_CODE = 0x8000
_DATA = 0x9000

)";

// z88dk 2.4: z80asm -debug -m on prog2.asm (labels on lines 2, 5, 7), and lines of zcc +z80 -debug -m on main.c
constexpr std::string_view kZ88dkAsm = R"(__ASM_LINE_2_prog2_2easm        = $8000 ; addr, local, , prog2, , prog2.asm:2
__ASM_LINE_5_prog2_2easm        = $8005 ; addr, local, , prog2, , prog2.asm:5
__ASM_LINE_7_prog2_2easm        = $8007 ; addr, local, , prog2, , prog2.asm:7
__head                          = $8000 ; const, public, def, , ,
__size                          = $0009 ; const, public, def, , ,
__tail                          = $8009 ; const, public, def, , ,
helper                          = $8007 ; addr, local, , prog2, , prog2.asm:7
loop                            = $8005 ; addr, local, , prog2, , prog2.asm:5
start                           = $8000 ; addr, local, , prog2, , prog2.asm:2
)";
constexpr std::string_view kZ88dkC = R"(CLIB_64BIT_FLOATS               = $0000 ; const, public, , z80_crt0, , Z:/opt/z88dk/lib/config/../../lib/crt/classic/crt_rules.inc:223
CLIB_DEFAULT_SCREEN_MODE        = $FFFFFFFF ; const, public, , z80_crt0, , Z:/opt/z88dk/lib/config/../../lib/crt/classic/crt_rules.inc:231
__CDBINFO__F_3aG_24main_24_30_5f_30_24_31_28_7b_30_7dDF_2cDV_29_2cC_2c_30_2c_30_2c_30_2c_30_2c_30 = $0001 ; const, public, , main_c, code_compiler, main.c::main::3::4:9
__C_LINE_0_main_2ec             = $0050 ; addr, local, , main_c, , main.c:0
__C_LINE_1_main_2ec             = $0050 ; addr, local, , main_c, , main.c:1
__C_LINE_2_C_3a_5cusers_5cdey_5cTemp_5czcc6974_2ei = $0050 ; addr, local, , main_c, , C:\users\dey\Temp\zcc6974.i:2
__C_LINE_2_main_2ec             = $0050 ; addr, local, , main_c, , main.c:2
__C_LINE_2_main_2ec_3a_3atwice_3a_3a0_3a_3a0 = $008B ; addr, local, , main_c, code_compiler, main.c::twice::0::0:2
__C_LINE_3_main_2ec_3a_3atwice_3a_3a1_3a_3a1 = $008E ; addr, local, , main_c, code_compiler, main.c::twice::1::1:3
__C_LINE_5_main_2ec_3a_3amain_3a_3a0_3a_3a2 = $00A3 ; addr, local, , main_c, code_compiler, main.c::main::0::2:5
__C_LINE_5_main_2ec_3a_3atwice_3a_3a0_3a_3a1 = $00A3 ; addr, local, , main_c, code_compiler, main.c::twice::0::1:5
__C_LINE_6_main_2ec_3a_3amain_3a_3a1_3a_3a3 = $00A6 ; addr, local, , main_c, code_compiler, main.c::main::1::3:6
__C_LINE_7_main_2ec_3a_3amain_3a_3a3_3a_3a4 = $00A6 ; addr, local, , main_c, code_compiler, main.c::main::3::4:7
__C_LINE_8_main_2ec_3a_3amain_3a_3a3_3a_3a4 = $00A6 ; addr, local, , main_c, code_compiler, main.c::main::3::4:8
__C_LINE_9_main_2ec_3a_3amain_3a_3a3_3a_3a4 = $00BE ; addr, local, , main_c, code_compiler, main.c::main::3::4:9
_counter                        = $012B ; addr, local, , main_c, bss_compiler, main.c::main::3::4:9
_main                           = $00A3 ; addr, public, , main_c, code_compiler, main.c::main::0::2:5
_twice                          = $008B ; addr, public, , main_c, code_compiler, main.c::twice::0::0:2
)";

std::string line_at(const SourceMap& map, std::uint16_t address) {
    const auto l = map.at(address);
    return l ? map.files()[l->file] + ":" + std::to_string(l->line) : "-";
}

void sjasmplus_sld_and_listing() {
    for (const std::string_view text : {kSld, kList}) {
        SourceMap map;
        SymbolTable symbols;
        const auto loaded = map.load(text, "prog", "/src", symbols);
        CHECK(loaded.format == (text == kSld ? SourceMap::Format::Sld : SourceMap::Format::SjasmList));
        CHECK(loaded.errors.empty());
        CHECK(loaded.lines == 7);
        CHECK(line_at(map, 0x8000) == "/src/prog.asm:8");
        CHECK(line_at(map, 0x8002) == "/src/prog.asm:9");
        CHECK(line_at(map, 0x8005) == "/src/prog.asm:11");  // the macro's lines belong to its call
        CHECK(line_at(map, 0x8007) == "/src/prog.asm:11");
        CHECK(line_at(map, 0x8009) == "/src/prog.asm:12");
        CHECK(line_at(map, 0x800B) == "/src/inc.asm:2");
        CHECK(line_at(map, 0x800C) == "/src/inc.asm:3");
        CHECK(line_at(map, 0x8001) == "-");  // inside an instruction: exact records only
        CHECK(map.address_of({0, 11}) == 0x8005);
        CHECK(map.code_lines(0) == (std::vector<std::uint32_t>{8, 9, 11, 12}));
        CHECK(loaded.labels == 4);
        CHECK(symbols.find("start.loop") && symbols.find("start.loop")->address == 0x8005);
        CHECK(symbols.find("value") && symbols.find("value")->address == 0x1234);
    }
}

void sdcc_files() {
    const std::string_view texts[] = {kNoi, kCdb, kSdccMap};
    const SourceMap::Format formats[] = {SourceMap::Format::SdccNoi, SourceMap::Format::SdccCdb, SourceMap::Format::SdccMap};
    for (int i = 0; i < 3; ++i) {
        SourceMap map;
        SymbolTable symbols;
        const auto loaded = map.load(texts[i], "main", "", symbols);
        CHECK(loaded.format == formats[i]);
        // C lines first: twice() is `return x + x` then the closing brace; main() starts at line 7's code
        CHECK(line_at(map, 0x8000) == "main.c:3");
        CHECK(line_at(map, 0x8001) == "main.c:4");
        CHECK(line_at(map, 0x8002) == "main.c:7");
        CHECK(line_at(map, 0x8005) == "main.c:7");  // a C line lasts until the file's next record
        CHECK(line_at(map, 0x800E) == "main.c:8");
        CHECK(line_at(map, 0x800F) == "main.c:10");
        // The generated main.asm, line by line
        const auto& files = map.files();
        const auto asm_file = static_cast<std::uint32_t>(std::find(files.begin(), files.end(), "main.asm") - files.begin());
        CHECK(asm_file < files.size());
        CHECK(map.at(0x8005, asm_file) && map.at(0x8005, asm_file)->line == 82);
        CHECK(map.at(0x800E, asm_file) && map.at(0x800E, asm_file)->line == 88);
        CHECK(symbols.find("main") && symbols.find("main")->address == 0x8002);
        CHECK(symbols.find("counter") && symbols.find("counter")->address == 0x9000);
        CHECK(!symbols.find("s__CODE") && !symbols.find("XG"));
        if (texts[i] != kCdb) {
            CHECK(symbols.find("_twice") && symbols.find("_twice")->address == 0x8000);
        }
    }
}

void z88dk_maps() {
    SourceMap map;
    SymbolTable symbols;
    auto loaded = map.load(kZ88dkAsm, "prog2", "/w", symbols);
    CHECK(loaded.format == SourceMap::Format::Z88dkMap);
    CHECK(loaded.lines == 3);
    CHECK(line_at(map, 0x8000) == "/w/prog2.asm:2");  // z80asm marks labels only: a line lasts to the next label
    CHECK(line_at(map, 0x8002) == "/w/prog2.asm:2");
    CHECK(line_at(map, 0x8005) == "/w/prog2.asm:5");
    CHECK(line_at(map, 0x8008) == "/w/prog2.asm:7");
    CHECK(symbols.find("helper") && symbols.find("helper")->address == 0x8007);

    SourceMap c;
    SymbolTable c_symbols;
    loaded = c.load(kZ88dkC, "prog", "/w", c_symbols);
    CHECK(line_at(c, 0x008B) == "/w/main.c:2");
    CHECK(line_at(c, 0x008E) == "/w/main.c:3");
    CHECK(line_at(c, 0x00BE) == "/w/main.c:9");
    CHECK(line_at(c, 0x0050) == "-");  // the preprocessor's records (no section) are left out
    CHECK(c_symbols.find("_main") && c_symbols.find("_main")->address == 0x00A3);
    CHECK(!c_symbols.find("CLIB_DEFAULT_SCREEN_MODE"));  // a 32-bit constant
}

void detection_and_removal() {
    CHECK(SourceMap::detect("start = 8000h\nloop EQU 8005h\n") == SourceMap::Format::Plain);
    SourceMap map;
    SymbolTable symbols;
    map.load(kSld, "a.sld", "", symbols);
    map.load(kNoi, "b.noi", "", symbols);
    CHECK(line_at(map, 0x8000) == "main.c:3");  // two programs over the same addresses: the later load wins
    CHECK(symbols.at(0x8000, freetvc::BankAddress{}) && symbols.at(0x8000, freetvc::BankAddress{})->name == "twice");  // and its label
    CHECK(symbols.find("main") && symbols.find("main")->address == 0x8002);
    symbols.remove_source("a.sld");  // loaded again as the monitor does it: newer than the NoICE file
    map.remove_source("a.sld");
    map.load(kSld, "a.sld", "", symbols);
    CHECK(symbols.at(0x8000, freetvc::BankAddress{})->name == "start");
    CHECK(map.remove_source("b.noi") > 0);
    CHECK(line_at(map, 0x8000) == "prog.asm:8");
    map.clear();
    CHECK(map.empty());
}

}  // namespace

int main() {
    sjasmplus_sld_and_listing();
    sdcc_files();
    z88dk_maps();
    detection_and_removal();
    if (g_failures == 0) {
        std::printf("source_map_test: all checks passed\n");
        return 0;
    }
    std::printf("source_map_test: %d check(s) failed\n", g_failures);
    return 1;
}
