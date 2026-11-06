# Változások

A teljes, angol nyelvű változásnapló: [CHANGELOG.md](CHANGELOG.md).

## Készülőben

### Új

- Videófelvétel ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): a TV-kép a TV 4:3-as arányában (1534 × 1148) és a hang MP4-be (H.264, AAC), ffmpeg-gel; `freetvc-headless --video FÁJL`. A csomagokban egy kis, GPL-es ffmpeg van az `ffmpeg/` almappában (`tools/release/build_ffmpeg.sh`), így semmit nem kell telepíteni.

## 1.1.0 — 2026-11-06

### Új

- A bővítőkártyák slotja választható a grafikus felületen ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): a Kártyák menüben a GameCard, a Multichip hangkártya, a TVC256k++, a NanoSD és a soros kártya alatt a *Betenni a slotba* (bent lévő kártyánál *Áthelyezni*) a 0–3. slotot kínálja a portjaival; a foglalt slot szürke, mellette a kártya neve. Beállítások → Gép: slotgombok minden kártya mellett. A `freetvc.ini` megjegyzi a slotot (`gamecard = 3`; `yes` = a kártya saját slotja). A HBF lemezkártya a 0. slotban marad.
- **2dfx grafikus kártya** (Vaczkó Károly kártyája: sprite-ok, 2D és 3D grafika a papír fölött, F8h/F9h port), a szerző saját, MIT licencű könyvtárával (2dfx v1.1.0, `third_party/twodfx/`): bármelyik slot (alapból a 2.), Kártyák menü és Beállítások, `twodfx` a `freetvc.ini`-ben, `--2dfx [SLOT]` a grafikus és a parancssori változatban, monitorparancs `2dfx`, MCP `media` `twodfx_on`/`twodfx_off`. Minden más kártya mellé betehető, kivéve a TVC256k++-t; mellette a SID-kártyának E0h–F7h marad. Kipróbálva AnyStone Cube, Wolfenstein 3D és AnyShow programjával (tvc.homeserver.hu). Az állapota a pillanatképbe és a visszatekerésbe is bekerül.
- macOS-csomagok Apple Siliconra és Intelre (macOS 13 vagy újabb), a `tools/mac/make_mac_release.sh` készíti; a Linux- és Windows-csomagot a GitHub Actions fordítja (`.github/workflows/release.yml`).
- `card256` MCP-eszköz ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): a TVC256k++ kártya állapota hibakereséshez, mellékhatás nélkül olvasva: minden regiszter értelmezve, a 16 sprite a képpontjaival, az overlay 32 × 30 csempekódja és színei, a kártya gyors RAM-ja és PSRAM-ja.

### Javítva

- A gazdagépes kazetta a nem `.CAS` adatfájlokat (például a 2dfx-es Wolfenstein 3D `.bin` fájljait) CAS-ként olvasta, és levágta az első 144 bájtjukat; most bájtra pontosan adja őket, ahogy a NanoSD kártya.

### Letöltések

| Fájl | Mire |
|---|---|
| `freeTVC-1.1.0-windows-x64.zip` | 64 bites Windows: bárhová kicsomagolható, a `freetvc.exe` indítja. A program nincs aláírva, ezért a SmartScreen első indításkor figyelmeztethet. |
| `freeTVC-1.1.0-linux-x86_64.tar.gz` | 64 bites Linux, glibc 2.39 vagy újabb (Ubuntu 24.04, Debian 13, Fedora 40 vagy újabb): kicsomagolás után `./freetvc`. |
| `freeTVC-1.1.0-macos-arm64.zip` | macOS 13 vagy újabb, Apple Silicon (M1 és újabb): kicsomagolás után `freetvc`. |
| `freeTVC-1.1.0-macos-x86_64.zip` | macOS 13 vagy újabb, Intel-processzoros Mac. |
| `SHA256SUMS`, `SHA256SUMS-macos` | a csomagok ellenőrzőösszegei |

A macOS-es programot az Apple nem hitelesítette. Első indításkor a macOS nem engedi megnyitni: a Finderben Control-kattintás a `freetvc`-re, majd *Megnyitás*, vagy `xattr -dr com.apple.quarantine` a kicsomagolt mappára.
