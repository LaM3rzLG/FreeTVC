# freeTVC Macen — állapot (2026-10-31)

**Egyelőre minden rendben.** A Macen fordított freeTVC fut, a TVC-programok (SIDSoftEMU, WaterPumpGame .dsk-ról)
működnek, az MCP-vezérlés és az időzítés ellenőrizve.

## Fordítás (Apple Silicon, macOS 27)
- Eszközök: Apple clang + Homebrew `cmake`, `ninja`.
- A gyári presetek csak Linux/Windows, ezért kézi konfiguráció, tesztek nélkül:
  ```sh
  cmake -S . -B build/mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFREETVC_BUILD_TESTS=OFF -DFREETVC_FETCH_ROMS=ON
  cmake --build build/mac-release -j8
  ```
- **Automatikus teszt (ctest) nem futhat** — a felhasználó kérése.
- Futtatható: `build/mac-release/src/freetvc` (és `freetvc-headless`); a `roms/`, `fonts/`, `skins/` stb. mellette.

## Mac-javítások
- **Retina:** a felület kétszeres méretű volt. UI-skála = kijelzőskála / pixelsűrűség (`src/frontend/sdl/main.cpp`).
  Commit `b5c98b5`. Minden a https://github.com/LaM3rzLG/FreeTVC repóba megy (2026-11-01 óta; a FreeTVC-Mac nem használt).
- **ROM-letöltés:** a "ROM-ok letöltése" a beállított `rom_dir` mappába tölt (ha van), különben a felhasználói
  adatmappába. `.gitignore`: `/roms/`, `/ROMS/`, `.DS_Store`. A felhasználó kipróbálta: a ROM-ok jó helyre mennek.

## 2dfx grafikus kártya (`2dfx` ág, 2026-11-06)
- Kártyák menü / Beállítások / ini `twodfx` / `--2dfx [SLOT]`; bármelyik slot, minden kártya mellé, kivéve TVC256k++.
- Tesztprogramok (tvc.homeserver.hu 2026/Kis_Róbert(AnyStone)): Cube, Wolf3D, AnyShow — mind jók, headless és GUI.
  Helyben: `2DFXCAS/` (git-ignore, `.git/info/exclude`), NanoSD-hez másolat: `~/Documents/TVC Projects/Wolf3D_2dfx`,
  `.../AnyShow_2dfx`. Részletek: `docs/design/cards.md` „2dfx graphics card”, tervek: `TERVEK.md`.

## Tudás a repóban
- Tervek, GitHub-hibajegyek (nem Mac-es): `TERVEK.md`.
- `CLAUDE.md` (munkaszabályok), `.claude/skills/freetvc-mac/` (fordítás, futtatás, MCP, időzítésmérés).
- `tools/mac/`: `build_mac.sh` (fordítás, teszt nélkül), `mcp.sh` (egy MCP-hívás), `mcp_pace.py` (valós idejű
  sebesség), `mcp_cycles.py` (RAM/VRAM ütemszámok, képkockahossz).

## Ellenőrizve (2026-10-31)
- GUI: szövegméret rendben; BASIC 2.2 indul; ROM-ok a program mellől.
- SIDSoftEMU fut (`.dsk`), a 4 változat gombbal vált; hang-IT 5008 Hz.
- WaterPumpGame: `.dsk`-ról a főmenüig fut; `.cas`-ról lila keret = a játék tervezett viselkedése (adatfájlok csak lemezről).
- MCP (`./freetvc --mcp-http --mcp-token ...`, port 6584): gépinfó, gépelés, képernyőkép, memória, disassembler,
  io_state, programbetöltés, gombnyomás — mind működik.
- Időzítés:
  - valós időben 99,97% (3,1240 MHz, 49,73 kép/s) 60 s alatt; elrejtett ablakkal is 100%; az indulás utáni első
    ~10 s-ban 85–90%.
  - RAM-ciklus 20,0 T; VRAM olvasás/írás 22,0 T és kód VRAM-ból 20,0 T (a hardverleírás táblázata szerint pontosan);
    képkocka 62 800 T (49,76 Hz).
  - Valódi géppel még nincs összevetve (ötlet: ugyanaz a mérőprogram .dsk-n a valódi TVC-n).
