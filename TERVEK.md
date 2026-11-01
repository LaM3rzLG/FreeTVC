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
