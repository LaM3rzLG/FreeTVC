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

## Képütemezés (`frame-pacing` ág, 2026-11-08)
- A kész TVC-képek sorba kerülnek (`FramePacer`), a kijelző egyenletes órával, 1,5 képpel a gép mögött veszi ki őket: nem marad ki kép
  (előtte ~6% kimaradt). A gép sebessége nem változik. Leírás: `docs/design/ui.md` "Frame pacing".
- Mérés: `FREETVC_FRAME_LOG=fájl` a GUI-nak, utána `python3 tools/frame_pacing_check.py fájl`.
- Mac: az OpenGL VSync-kel 180 Hz-es monitoron is csak 60 Hz -> a GUI ezt 1 s alatt méri, és saját időzítésre vált a monitor
  frekvenciáján (`PresentTiming`); a képhatások maradnak. Belső 60 Hz-es kijelzőn marad a VSync; áthúzáskor újra dönt. Mindenhol 0 kihagyás.
  A felhasználó a 180 Hz-es saját időzítést jónak látta. Teljes képernyő nincs ellenőrizve. Windows (PC, 60/144 Hz): OpenGL és D3D 0 kihagyás; a felhasználó szemmel is jónak látta.
- A felhasználó a Befosás scrollját a Macen simának látta ("mint a TMC20").

## Képkeverés és a sáv kattintási hibája (1.3.2, 2026-11-08)
- Hiba: kisebbre húzott sávnál a gombok nem reagáltak; a barázda láthatatlan fogantyú-ablaka az ImGui `WindowMinSize`-t kapta
  és rálógott a gombokra. Javítva (`play_view.cpp`), szintetikus kattintással ellenőrizve.
- Képkeverés (`frame_blend.cpp`): 60/120/144/180 Hz-en a két szomszédos TVC-képet a kijelzőóra szerint keveri (lineáris fényben);
  Automatikus = nem kever 50/100 Hz-en. Élesség (`blend_sharpness`, alap 50 %). Alsó sáv: **Hz** gomb (bal klikk be/ki,
  bekapcsolva jobb klikk: Élesség csúszka, fehér fogantyú). Mérés a Hóvágtán (betöltés, 5 mp, Space, D) a beépített 60 Hz-es
  kijelzőn: az arány frissítésenként 0,17-et lép, 0 kimaradt kép. A felhasználó a GUI-ban kipróbálta.
- Szintetikus egérkattintás a GUI-ba: kis Swift program `CGEvent`-tel (a VS Code-nak van Accessibility joga), a
  `screencapture -C` mutatja a kurzort. Ideiglenes ImGui-napló a kattintás címzettjéről segített megtalálni a hibát.

## Ikon (2026-11-08)
- Az indítókép TVC-fotója sötét, lekerekített alapon (`data/icon/freetvc.svg`); `sh tools/make_icons.sh` készíti belőle az
  ablakikont (`data/about/freetvc-icon.png`, Dock/tálca) és a Windows exe ikonját (`data/icon/freetvc.ico`, `.rc`).
  Macen és PC-n a felhasználó látta, jó. Nyitott: Mac `.app` csomag, hogy a Finderben is ikon legyen.

## 2dfx grafikus kártya (`2dfx` ág, 2026-11-06)
- Kártyák menü / Beállítások / ini `twodfx` / `--2dfx [SLOT]`; bármelyik slot, minden kártya mellé, kivéve TVC256k++.
- Tesztprogramok (tvc.homeserver.hu 2026/Kis_Róbert(AnyStone)): Cube, Wolf3D, AnyShow — mind jók, headless és GUI.
  Helyben: `2DFXCAS/` (git-ignore, `.git/info/exclude`), NanoSD-hez másolat: `~/Documents/TVC Projects/Wolf3D_2dfx`,
  `.../AnyShow_2dfx`. Részletek: `docs/design/cards.md` „2dfx graphics card”, tervek: `TERVEK.md`.

## Játékmentések és felület (1.3.0, 2026-11-07)
- Játékmentés: F9 vagy a TVC gomb melletti ikon → a gép megáll, sötét, név bekérése; `SaveState/<játék>/<név>.tvcs` + png.
  Shift+F9 = legutóbbi; jobb klikk = mentések listája. MCP `game_saves`. A felhasználó a GUI-ban kipróbálta.
- Üzenetek a kép felső keretén; sebesség az ablak címében; alsó sáv húzható (75–250 %, `strip_size`); betűméret
  lenyíló listából (`ui_font_size`). Betűtípus-választás kivéve (a vak listából díszbetű lett kiválasztva).
- Ellenőrzés kattintás nélkül: `screencapture` csak akkor jó, ha az ablak elöl van (`set frontmost`), különben mást fotóz.

## Kiadás (1.1.0-tól; az 1.1.0 kiadva 2026-11-06, az 1.2.0 és az 1.3.0 2026-11-07, az 1.3.1 2026-11-08, a felhasználó fiókjáról)
- Az Apple clang elnéz hiányzó `#include`-okat (pl. `<cmath>` a `std::lround`-hoz), a Linuxos GCC nem: az 1.3.0 első
  Actions-futása ezen bukott el. Új `std::` függvénynél a fejlécet is be kell írni.
- Linux + Windows: GitHub Actions (`.github/workflows/release.yml`, kézi indítás vagy `v*` tag) → artifact.
- macOS: `sh tools/mac/make_mac_release.sh OUT` → arm64 és x86_64 zip (macOS 13+, ad-hoc aláírás, nincs notarizálás).
- A release-t Claude hozza létre a felhasználó fiókjáról (`gh release create`), mind a négy csomaggal, angol és
  magyar változásnaplóval (`CHANGELOG.md`, `CHANGELOG.hu.md`).

## Videófelvétel és GUI (2026-11-06)
- Saját kis ffmpeg (`tools/release/build_ffmpeg.sh native|macos-x86_64|mingw OUT`, Homebrew nasm + pkgconf), a program
  mellett `ffmpeg/`-ben (`build/mac-release/src/ffmpeg/` kézzel másolva; a csomagokba a szkriptek teszik).
- GUI-átalakítás és UPM: `gui-upm` ág, részletek a `TERVEK.md` „FOLYAMATBAN” részében.
- A GUI-t a külső monitoron kell megnyitni (3440×1440; System Events `set position of window 1 to {-900, -1340}`).

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

## GUI (2026-11-07)
- Induló gépfotó és új alsó sáv (ikonok) — részletek a TERVEK.md „FOLYAMATBAN: GUI-átalakítás” részében.
