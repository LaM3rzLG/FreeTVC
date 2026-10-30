# Glossary

Terms used across the freeTVC documentation, and Hungarian words that appear in TVC sources.

## TVC / project terms

| Term | Meaning |
|---|---|
| TVC | Videoton TV Computer (also "TV-Computer"), Hungarian Z80 home computer, 1985–86 |
| 32k / 64k / 64k+ | TVC models by RAM; 64k+ has 64 KiB video RAM in four banks (video paging register at ports 0Ch–0Fh) and BASIC 2.x |
| SYS ROM | 16 KiB system ROM with BASIC (versions 1.2, 2.1, 2.2; Cyrillic 1.2 variant) |
| EXT ROM | extension ROM in the 8 KiB socket at E000–FFFF: 1.x is a 4 KiB image in the upper half (F000–FFFF), 2.x is 8 KiB (checked against the MAME CRCs) |
| CART | cartridge slot ROM (up to 16 KiB) |
| IOMEM | memory window mapped to an expansion card's ROM/RAM, selected via port 03h |
| U0–U3 | RAM pages (16 KiB each) in the memory map |
| VID / VRAM | video RAM |
| psel | page select bits written to port 02h |
| HBF | the Videoton disk interface card (WD1793 FDC + an EPROM with VT-DOS 1.1 `hbf.rom` or 1.2 `hbf12.rom`) |
| VT-DOS | MS-DOS-3-compatible disk OS for TVC (also exists as cartridge ROM) |
| UPM | Videoton's CP/M-like OS |
| MOPS | header signature at offset 0 of cartridges and card ROMs (C000h); the ROM starts a cartridge at C004h |
| NanoSD | dikdom's NanoSD IO card: an expansion card whose Arduino Nano serves files from an SD card |
| TVC256k++ | dikdom's modern expansion card: extra RAM, PSRAM, USB pendrive, flash and DSK drives, a SID emulation (vSID), mouse, graphics overlay with sprites, firmware command line |
| Multichip / GameCard | expansion cards with TI SN76489 sound chips (the Multichip also has a SAA1099 and a YM3812; it is also called SoundMagic) |
| CAS | disk-side file format for TVC programs (128-byte UPM header + 16-byte program header + payload) |
| TTP | WinTVC's byte-level tape format (no sync signals) |
| TVS | WinTVC snapshot format (`.tvs`); freeTVC imports it |
| TVCS | freeTVC's own snapshot format |
| preset | one of freeTVC's machine and ROM combinations: `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`, `64k+-2.1`, `64k+-2.2` |
| host cassette | freeTVC's stand-in for the cassette class: the ROM's cassette calls served from CAS files on the host |
| Play / Bench | the GUI's two modes: the machine on its own, and the debugging workspace (F12) |
| DZRP | DeZog's remote protocol, served by freeTVC for the DeZog debugger |
| MCP | Model Context Protocol, served by freeTVC for AI agents |
| clock stretching | the CPU clock held for 1–4 half-periods of 160 ns when the CPU accesses video RAM, until its slot in the time-shared VRAM cycle ([tvc-hardware.md](hardware/tvc-hardware.md#video-ram-contention-clock-stretching-hw-224-fig-2022) §2) |
| IGRB | TVC palette/colour encoding (Intensity, Green, Red, Blue) |

## Hungarian words seen in sources

| Hungarian | English |
|---|---|
| magnó, kazetta, szalag | tape recorder, cassette, tape |
| lemez, diszk, meghajtó | disk, disk, drive |
| kártya, kártyahely, csatolókártya | card, card slot, interface card |
| betöltés / mentés | load / save |
| hidegindítás, hideg reset | cold start, cold reset |
| szünet | pause |
| billentyűzet, kiosztás | keyboard, layout |
| botkormány | joystick |
| hang, hangfelvétel | sound, audio recording |
| kép, képernyő | picture, screen |
| sebesség | speed |
| beállítás | settings |
| vágólap / vágóasztal | clipboard |
| saját | own / custom |
| Olvass el | "read me" |
| Műszaki leírás | technical description (manual) |
| ékezetes betű | accented letter |
| Cirill | Cyrillic |
