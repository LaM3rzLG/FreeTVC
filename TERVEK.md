# freeTVC — tervek és a GitHub-hibajegyek állapota

Közös (Windows + Mac), nem platformfüggő fejlesztések. A Mac-es fordítás/futtatás: `MAC_ALLAPOT.md`.

## Issue #1 „Missing features need to be implemented” (belaszalontai) — állapot 2026-11-02
- 1. kártyahely a felületen: KÉSZ (ed4863b) · 5. MCP `card256`: KÉSZ (a3647ee)
- 2. videófelvétel: **FOLYAMATBAN** (2026-11-06). Kész: `FrameSink` a Video-ban, `VideoRecorder` (`frontend/common`,
  ffmpeg gyerekfolyamat: RGB cső → x264 MP4, hang f32 fájlba, a végén AAC-vel összefűzve), `--video FILE` (headless),
  saját kis ffmpeg (`tools/release/build_ffmpeg.sh`, 3,5–4,3 MB) a csomagok `ffmpeg/` mappájában. Hátra: GUI (alsó sor,
  külön menü kamera ikonnal — a felhasználóval megbeszélni), monitor, MCP. Csak az élő kép (a felhasználó kérése).
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

## FOLYAMATBAN: GUI-átalakítás és UPM — átadás (2026-11-06 este, Mac)
Ág: `gui-upm` (a `main`-ből; a `main` = v1.1.0 + videófelvétel headless). Minden lefordul (`ninja -C build/mac-release`),
a felhasználó a GUI-ban végignézte és jóváhagyta az alábbiakat; release NEM volt, és ne is legyen kérés nélkül.
- **Alsó sáv (strip)** 1,5×: bronze skin `metric.strip.height = 45`, új kulcs `font.strip.size = 22` (`Skin::font_strip_size`,
  0 = `font.ui.size`); a sávból nyíló menük is ekkora betűvel (`PushFont(nullptr, …)` a menük körül, `play_view.cpp`).
  A felhasználó `sajat.skin`-je még a régi bronze másolata (30/—).
- **Gép gomb** = rajzolt mini TVC ikon (`DeckRow::tvc`, a választott „A” változat 90 %-on); súgó: „Gép és ROM-ok: <preset>”.
- **Gépmenü**: `TVC 32K · BASIC 1.2`, `TVC 64K · BASIC 1.2`, `TVC 64K+ · BASIC 2.1`, `TVC 64K+ · BASIC 2.2` (MenuItem shortcut
  oszlop = BASIC; `PushID` a két „TVC 64K+” miatt). A 32k+HBM preset nincs a listában: Kártyák menü alján „HBM 32 KiB
  RAM-kártya” (csak 32K-n látszik, a 32k ↔ 32k+hbm presetet váltja).
- **Kártyák menü**: csak `Slot 0`…`Slot 3` + a kártya neve (üresnél „Üres”, halványan, a leghosszabb név szélességén
  középen; a címke szóköz-kitöltés, a szöveg rárajzolva). Slot ▸ lista: Üres, HBF (csak slot 0), GameCard, Multichip,
  TVC256k++, 2dfx, NanoSD, soros. Csere: a régi kártya kikerül (`take_out`), a másik slotból áthelyez. A bent lévő,
  beállítható kártya neve maga nyíló almenü (`draw_card_extras`): HBF (lemezek A–D, gyors lemez, „Lemezes rendszer”:
  VT-DOS 1.1 / 1.2 / UPM — az UPM még szürke), TVC256k++ (pendrive, egér, flash), NanoSD (SD-mappa), soros (kábel, fájlok).
  A SID-kártya csak a Beállításokban; Nyomtató + Cartridge ideiglenesen a főmenü (névtábla) végén (`draw_ports_menu`).
- **REC ikon** (rajzolt, sarokjelek + piros pont + REC): bal klikk indít/leállít (név: `<program> ÉÉÉÉ-HH-NN óó-pp-mm.mp4`,
  alap: Movies/Videók, `video_dir`), mellette az idő, a pont villog; felvétel közben a TV-kép jobb felső sarkában is
  villog egy piros pont (csak az ablakon). Jobb klikk: menü (indít/leállít, mappa…, mappa megnyitása). Kilépéskor lezár.
- **Fényképező ikon** (REC-szélességű, ugyanolyan sarokjelek): PNG az Asztalra (`screenshot_dir`), a főmenü
  „Képernyőkép” is. Kép és videó 4:3-ban: 1534×1148 (sor ×4, vízszintesen területátlag, `kPixelAspect` a
  `framebuffer.hpp`-ben, `pal_width()`), a videó ffmpeg-gel `scale=…neighbor,scale=…area`.
- **Induló kép**: `data/about/tvc64kplus.png` (a felhasználó fotója, fehér háttér és árnyék kivágva) halványan a TV-kép
  helyén induláskor, bekapcsoláskor és kártyacserénél; a gép közben fut; legalább 2,5 s, amíg a gép nem rajzol (a 64k
  ~5 s fekete), max. 10 s, 0,5 s elhalványulás (`show_machine()` a `main.cpp`-ben).
- **Sáv (2026-11-07)**: slotok · Cart (elforgatott cartridge ikon + „Cart”/név, egy gomb, Cartridge menü) · gép ·
  billentyűzet ikon (menü: Szimbolikus/Pozíciós/Kiosztás ▸) · hangszóró (némítva piros áthúzás) · 5,25" floppy (kártya
  nélkül halvány) · kazetta (szalag nélkül halvány) · ▶ (szürke szalag nélkül) … üzenet … REC · fénykép · % · freeTVC
  (jobbra, főmenü). Gépmenü fejléce „Verzió” középen.
- **freeTVC főmenü (2026-11-07)**: Program betöltése · Legutóbbi programok ▸ (10, ini `recent`, `|` választja el) ·
  Újraindítás · Pillanatkép ▸ · Teljes képernyő · Kinézet ▸ · Nyelv ▸ · ROM-ok letöltése · Beállítások · Munkapad ·
  Névjegy · Kilépés. Billentyűzet, képernyőkép, teljes raszter, kazetta kivétele kikerült. Nyomtató: saját ikon a REC
  előtt (halvány/világos/sárga nyomtatáskor). Magnó: kazetta (forgáskor analóg hullám a valódi jelből, 1,5 ms) ·
  ▶ + számláló · ■ · ◀◀; menü csak a kazettán; a magnó és a lemez menüje fejléc nélkül.
- **Lemez és cartridge (2026-11-07)**: floppy menü első pontja „Program közvetlen betöltése” (csak .dsk/.img,
  `disk_autostart` akció → `open_program`); a „Program betöltése” nem fogad CRT-t (szűrő + üzenet: Cart gomb); a Cartridge
  menü fejléc nélkül; a HBF kártya almenüjében csak a „Lemezes rendszer”, a lemezek a floppy ikonon. A gyors lemez marad
  (a felhasználó kérte vissza). `HostTape::disk_autostart` most az első üres meghajtóba teszi a lemezt és `LOAD "B:NÉV"`
  -et gépel — **NYITOTT**: így a SIDSoftEMU B:-ről elakad (a fájljait az A:-n keresi); javasolt: az új lemez mindig A:,
  a régi A: az első üres meghajtóba — a felhasználó még nem döntött.
- **SIDSoftEMU + HBF**: a felhasználónál „nem jó” (2026-11-07), nálam A:-ról (64K/1.2, HBF) a fájlkezelő és a SONG1.STC
  lejátszás megy (IT 5008 Hz, valós sebesség). A felhasználótól még kell: mi a hiba pontosan, melyik gép/slot.
- **Tárolós kártyák betétele (2026-11-07)**: a slot listájában a még nem betett NanoSD és TVC256k++ almenüt kap
  (SD-kártya mappa… / Pendrive mappa… / Flash meghajtó képfájl… / Betétel üresen): a kártya bekerül, és rögtön nyílik a
  tallózó (a meglévő `nanosd_sd` / `card256_pendrive` / `card256_flash` akciók).
- Elvetve: program-autodetect / programlista (a felhasználó: „aki nem tudja, mi kell hozzá, így járt”).
- Hátra a GUI-ból: Nyomtató végleges helye; Ctrl/Cmd+R gyorsbillentyű
  (a CTRL a TVC-nek is kell — megbeszélni); cartridge-mappa (javaslat: `cartridges/` a felhasználói adatmappában, a
  „Cartridge betétele” ott nyílik — a felhasználó még nem döntött); docs (ui.md: strip, menük; user-guide 4.8 GUI rész);
  monitor/MCP parancs a felvételhez; Windowson kipróbálni (felvétel, ffmpeg.exe).
- **UPM** (forrás: tvc.homeserver.hu/html/hardverrom.html#vtdosupm):
  - Kártya-EPROM: `cas/odor/UPM.FDR` = CRC B3A567AD ✓ (a mi `hbf.rom`-ként is elfogadott UPM EPROM-unk).
  - Cartridge: a `cas/odor/TVCUPM1–4.CAR` összefűzése (2868F331) ROSSZ (a gép elszáll). A JÓ: a gyári EPROM,
    CRC **EFCA95FC** = a felhasználó `~/Downloads/UPM/cpmdos1_1/CUPM_DOS.256` első 16 KiB-ja; várhatóan a
    `bin/rom/romgyujtes/(9)upm.zip`-ben is (letöltendő, ellenőrizendő) → autofetch innen, `data/roms.txt`.
  - Kipróbálva (headless): `--preset 64k-1.2 --disk-rom UPM.FDR --cart <EFCA95FC> --disk A:upmlemez.img --key RETURN@250`
    → `*`, `64K UPM SYS READY`, `A>`. Tesztlemez: `cas/upmlemezek.zip` (3 db 720 KiB, a rendszersávok üresek).
  - `~/Downloads/UPM/cpmdos1_1|2`: a „VT-DOS/UPM átkapcsolás” mod EPROM-jai (32 KiB = 2×16 KiB, A14 vált): C = cartridge
    (UPM + VT-DOS 1.1 ill. 1.2 cartridge), D = lemezkártya (UPM.FDR + VT-DOS 1.1 ill. 1.2). Ugyanez: `cas/odor/UPMDOS.CAR/FDR`.
    Felhasználás: a HBF „Lemezes rendszer” választónál VT-DOS ↔ UPM váltás egy kattintással (ahogy a mod is).
  - Teendő: (9)upm.zip autofetch (cart `upm-cart.rom`, kártya `upm.rom`), `--disk-rom upm` + az UPM cartridge betétele
    egy választással (VT-DOS-ra vissza: a cartridge ki), a menüpont élesítése; RETURN a `*` után (autostart); UPM-lemez
    felismerés (nem FAT12, E5h rendszersávok) → UPM autostart; üres UPM-lemez. Melyik gépen: 64K biztosan (32K ❓).

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
