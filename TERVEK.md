# freeTVC — tervek és a GitHub-hibajegyek állapota

Közös (Windows + Mac), nem platformfüggő fejlesztések. A Mac-es fordítás/futtatás: `MAC_ALLAPOT.md`.

## Issue #1 „Missing features need to be implemented” (belaszalontai) — állapot 2026-11-02
- 1. kártyahely a felületen: KÉSZ (ed4863b) · 5. MCP `card256`: KÉSZ (a3647ee)
- 2. videófelvétel: **KÉSŐBBRE** (2026-11-02). Irány: FFmpeg (a freeTVC a képkockákat és a hangot a külön telepített
  `ffmpeg`-nek adja át -> MP4). Minta: az ep128emu beépített AVI-írója (`~/Documents/TVC-emu/ep128emu/src/videorec.cpp`,
  RLE8/YV12, GPL-2+ -> átvehető). Tesztelni csak Windows-on és Mac ARM-on tudunk.
- 3. billentyűzetkiosztás: **KÉSŐBBRE** (2026-11-02). A rajz már megvan (`keyboard_editor.cpp`, a TVC-gombok alatt a
  gép-gombok); hiányzik: csak megnézős mód menüből/gyorsbillentyűvel (~1-2 óra), Mac-es alapkiosztás `mac.keys`
  (a MacBookon nincs jobb Ctrl = FIRE és számbillentyűzet = 2. joystick; ~1 óra), esetleg gamepad (~1 óra).
- 4. About + „Bronze” sötét betű: **ELVETVE** (a felhasználó döntése, 2026-11-02)

## Ep128web (kks2003.github.io/Ep128web) — mit tud, amit mi nem (felmérés 2026-11-06)
Böngészős ep128emu (Varga István, GPL-2, WebAssembly) Enterprise + TVC géppel. A tények angolul:
`docs/hardware/tvc-hardware.md` §9 (2dfx, UPM). Repó: github.com/kks2003/Ep128web.
- **MCP-ben nem több**: Node + Playwright vezérli a böngészőt (`mcp/server.js`, 19 eszköz: start, gépválasztás, fájl,
  gépelés, képernyőszöveg, kép, memória, disasm, töréspont, snapshot, `eval_js`). Ötletnek: `wait_text`, `screen_text`.
- **Debuggerben ötletek**: visszalépés (utolsó 40 állapot), 65536 utasításos trace, Lua- és JS-szkript.
- **UPM** — mi már most is elfogadjuk az UPM kártya-EPROM-ot (B3A567AD) `hbf.rom` néven, de nincs kipróbálva. Teendő:
  1. ROM-ok: UPM kártya-EPROM (tvc.homeserver.hu `cas/odor/UPM.FDR`) és UPM cartridge (49202, `tvcupm_c.rom`) felvétele
     az autofetch-be saját néven (pl. `hbfupm.rom`, `upm.rom`), `--disk-rom upm`, menü: „Lemezkártya: UPM”. ~1-2 óra.
  2. Kipróbálás egy UPM lemezzel (CP/M 2.2, 720 KiB, 4 foglalt sáv, 2 KiB blokk); a FDC-emuláció elvileg kész.
  3. Lemez-autostart: CP/M lemez felismerése (nem FAT12) → UPM ROM + cartridge, RETURN a `*` után. ~2-3 óra.
  4. Üres UPM lemez létrehozása (E5h-val töltött kép) a lemez menüben. ~1 óra. Összesen ~1 nap, kockázat kicsi.
- **2dfx grafikus kártya** (Vaczkó Károly): saját sprite/2D/3D grafika F8h/F9h porton, a TVC képe fölé (/EXTC). A mi
  TVC256k++ overlay-ünk ugyanazon az úton jön be, erre ráépülhet. Két út:
  - A) **A szerző MIT-es C99 könyvtárát átvenni** `third_party/twodfx/`-be (THIRD_PARTY.md), az Elite/Wriggler demóadatok
    nélkül (azok nem MIT-esek → a C9h, D3h–D5h demó kimarad). Bekötés: port F8h/F9h, VSYNC/HSYNC hívás, /INT, overlay a
    paper területén, snapshot. A szerző a hardverrel vetette össze. ~2-3 nap.
  - B) Tiszta szobás újraírás a `COMMAND_REFERENCE.md`-ből (32 KiB leírás): sok hét, és a demók nélkül nincs mihez mérni.
  - Javaslat: A). Kérdés a felhasználónak: az AGENTS.md „clean room” szabálya más emulátorok kódjára vonatkozik; ez a
    kártya szerzőjének saját, MIT-es könyvtára (mint a reSID) — rendben van-e így átvenni?
  - Ütközések: a mi SID kártyánk E0h–FFh-t foglal (benne F8h/F9h) → 2dfx mellett a SID kártya E0h–F7h-ra szűkül, vagy
    a kettő kizárja egymást. Az Ep128web-ben a 2dfx és a TVC256k++ nem mehet együtt (mindkettő overlay).
  - 8 MiB erőforrás-RAM: a visszatekerés (rewind) snapshotjaiba csak a változott részt szabad tenni, különben túl nagy.
  - Nyitott: Graphics 4 módban teljes vagy fél felbontás? (Ep128web: fél; a mi HBA-2 olvasatunk: teljes.)

### 2dfx — félkész, `2dfx` ág (2026-11-06, Windows; folytatás a Macen)
- A felhasználó döntése: a szerző MIT-es könyvtára átvéve (`third_party/twodfx/`, THIRD_PARTY.md), a C9h és a D3h–D5h
  demó nélkül (Wriggler/Elite adatok). A TVC256k++ és a 2dfx kizárja egymást (Ep128web: mindkettő EC0-3, /EXTC);
  a SID kártya mehet mellé, F8h/F9h a 2dfx-é (2026-11-06).
- Kész: `CardKind::TwoDfx` (`src/core/cards/twodfx_card.*`), bármely slot; F8h/F9h a `Machine::io_read/io_write`-ban;
  overlay a papíron (raw X 126, Y 49 = a BASIC papír bal felső sarka), 4 és 16 színben minden 2. 2dfx-pixel,
  duplázva (Ep128web; a „minden 4.” a Wolf3D címképének ditherét csíkokra bontotta); /INT a raszter-latch-ekből; reset = bekapcsolás (Ep128web README ⚑); headless `--2dfx [SLOT]`
  (alap 2; `--card256`-tal hiba); pillanatkép elfogadja a kártyát, de a 2dfx állapotát NEM menti.
- Másolatok (rewind) egy közös 2dfx-eszközön osztoznak (8 MiB RAM nem másolódik) → a rewind a 2dfx-et nem állítja vissza.
- Kipróbálva (headless, 64k+-2.2): Cube (tvc.homeserver.hu `Cube_2dfx.zip`) — forgó kocka hat színnel, egyezik a
  honlap képével.
- Wolf 3D (`Wolf3d_2dfx.zip`) megy (2026-11-06, Mac, headless): címkép és játék (HUD, fegyver). A hiba nem a 2dfx-ben
  volt: a gazdagépes kazetta a nyers `.bin` adatfájlokat CAS-nak nézte, és 144 bájtot levágott belőlük. Most a nem
  `.CAS` kiterjesztésű fájlt bájtra pontosan, pufferelt fájlként adja, ahogy a NanoSD kártya (`host_cassette.cpp`).
  A 9300h–9400h körüli ciklus csak a SPACE/ESC-re váró billentyűolvasás volt.
- GUI (2026-11-06, a felhasználó kipróbálta): Kártyák menü és Beállítások → Kártyák „2dfx grafikus kártya” slotválasztóval,
  ini `twodfx` (yes/no/0-3), `--2dfx [SLOT]`; csak a TVC256k++ van kizárva (mindkét irányban);
  a felhasználó döntése: minden más kártya mehet mellé, a SID kártya ekkor E0h–F7h-t kapja (kipróbálva SID + Multichip +
  soros kártyával). hu.po kész.
  Kipróbálva még: AnyShow2dfx (tvc.homeserver.hu 2026/Kis_Róbert(AnyStone), 15 kép) — jó.
- Dokumentálva (2026-11-06): cards.md „2dfx graphics card”, tvc-hardware.md §9, media.md, status.md, MAC_ALLAPOT.md, skill.
- MCP `media` `twodfx_on`/`twodfx_off` + `status`, monitor `2dfx` kész (2026-11-06).
- Mentés kész: pillanatkép és visszatekerés (a könyvtár mentése tömörítve; Wolf3D címképnél 482 KB, visszatöltve jó).
- Hiányzik: unit teszt.
- A Windows-gépen az Intelligens alkalmazásvezérlés (Smart App Control) letiltja a friss, aláíratlan fordítást.

## Elkészült

### Kártyahely választása a felületen (2026-11-01, kérésre)
- Kártyák menü: minden kártya alatt „Behelyezés kártyahelyre” / „Átrakás másik kártyahelyre” almenü (0-3, portokkal; a
  foglalt hely szürke, a benne lévő kártya nevével). Beállítások → Gép: 0-3 gombok minden kártya mellett.
- `freetvc.ini`: `gamecard`, `multichip`, `card256`, `nanosd`, `serial_card` = yes / no / 0-3 (yes = a kártya saját helye).
- A lemezkártya (HBF) marad a 0-son. MCP-vel ellenőrizve: az ini-ből a megadott helyre kerül, átrakás után az ini frissül.
  A menük kattintása: a felhasználó próbálja ki.

### MCP `card256` eszköz (2026-11-01, issue #1 5. pont)
- `registers` / `sprites` (`sprite`, `pixels`) / `tiles` (`rows`) / `ram` (`address`, `length`, `psram`, `format`); mellékhatás
  nélkül (peek_register). Kipróbálva MCP-n: bekapcsolás utáni sapka-sprite pixelre egyezik a képpel, ütközés nem törlődik.
