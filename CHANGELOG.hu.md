# Változások

A teljes, angol nyelvű változásnapló: [CHANGELOG.md](CHANGELOG.md).

## 1.3.3 — 2026-11-09

### Változott

- **A lemezeket megjegyzi**: a kilépéskor az A:–D: meghajtókban lévő lemezképek a `freetvc.ini`-be kerülnek (`disk_a` … `disk_d`), és a következő indításkor visszakerülnek a meghajtókba, ha a lemezes kártya bent van. A parancssorban megadott lemezekre (`--disk`, `--disk-card`, indítandó `.dsk`) ez nem vonatkozik. Ha egy lemezfájl közben eltűnt, a meghajtó üres marad, és erről üzenet jelenik meg a képen.

### Letöltések

| Fájl | Mire |
|---|---|
| `freeTVC-1.3.3-windows-x64.zip` | 64 bites Windows: bárhová kicsomagolható, a `freetvc.exe` indítja. A program nincs aláírva, ezért a SmartScreen első indításkor figyelmeztethet. |
| `freeTVC-1.3.3-linux-x86_64.tar.gz` | 64 bites Linux: Ubuntu 24.04 LTS vagy újabb (Debian 13, Fedora 40 vagy újabb is jó): kicsomagolás után terminálban `./freetvc`. |
| `freeTVC-1.3.3-macos-arm64.zip` | macOS 13 vagy újabb, Apple Silicon (M1 és újabb): kicsomagolás után `freetvc`. |
| `freeTVC-1.3.3-macos-x86_64.zip` | macOS 13 vagy újabb, Intel. |
| `SHA256SUMS`, `SHA256SUMS-macos` | az archívumok ellenőrzőösszegei |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | a mellékelt ffmpeg forrásai (GPL) |

## 1.3.2 — 2026-11-08

### Új

- **Képkeverés** nem 50 Hz-es monitorhoz: a TVC másodpercenként 49,76 képet rajzol, ezért 60 Hz-es monitoron minden ötödik kép kétszer látszik, és a scroll másodpercenként tízszer döccen (120, 144, 180 Hz-en kevésbé). A keverés a frissítés körüli két képet összemossa, így a mozgás egyenletes. *Automatikus* módban (alapértelmezés) kever, kivéve 50 vagy 100 Hz-es monitoron, ahol nincs döccenés. Beállítások → Kép → Képkeverés, a `freetvc.ini`-ben `frame_blend`.
- **Hz gomb** az alsó sávban, a hangszóró mellett: világos, ha a keverés be van kapcsolva, halvány, ha nincs; kattintásra be- és kikapcsol. Bekapcsolva jobb kattintásra megnyílik az **Élesség** csúszka (0 %: legsimább; nagyobb: csak a váltás keveredik, a legtöbb kép éles marad; `blend_sharpness`, alapból 50 %).

### Javítva

- **Az alsó sáv gombjai nem reagáltak a kattintásra**, miután a sávot a fölötte lévő barázdával kisebbre húztuk: a barázda láthatatlan fogantyúja a felület legkisebb ablakméretét kapta, és rálógott a gombok felső részére.

### Linux

- **Ubuntu 24.04 LTS** vagy újabb ajánlott (vagy más, 2024-es vagy újabb Linux: Debian 13, Fedora 40). Az Ubuntu 22.04 már túl régi.
- A program a kicsomagolt mappában lévő `freetvc` fájl. A Fájlok alkalmazásban dupla kattintásra nem indul: nyiss terminált a mappában, és írd be: `./freetvc` (lépésenként lent, a Gyors kezdésben és a [README.md](README.md#running-on-linux)-ben).

### Letöltések

| Fájl | Mire |
|---|---|
| `freeTVC-1.3.2-windows-x64.zip` | 64 bites Windows: bárhová kicsomagolható, a `freetvc.exe` indítja. A program nincs aláírva, ezért a SmartScreen első indításkor figyelmeztethet. |
| `freeTVC-1.3.2-linux-x86_64.tar.gz` | 64 bites Linux: Ubuntu 24.04 LTS vagy újabb (Debian 13, Fedora 40 vagy újabb is jó): kicsomagolás után terminálban `./freetvc`. |
| `freeTVC-1.3.2-macos-arm64.zip` | macOS 13 vagy újabb, Apple Silicon (M1 és újabb): kicsomagolás után `freetvc`. |
| `freeTVC-1.3.2-macos-x86_64.zip` | macOS 13 vagy újabb, Intel. |
| `SHA256SUMS`, `SHA256SUMS-macos` | az archívumok ellenőrzőösszegei |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | a mellékelt ffmpeg forrásai (GPL) |

## 1.3.1 — 2026-11-08

### Új

- **Ikon**: a TV Computer fotója sötét, lekerekített alapon; látszik a Dockban, a tálcán, az Alt+Tab / Cmd+Tab váltóban és a Windowsos `freetvc.exe` fájlon.

### Változott

- **Simulékony megjelenítés**: a finom scroll és a mozgás egyenletes, nem ugrik. Eddig minden ~16. TVC-kép kimaradt a képernyőről, mert a gép a hang ütemében csomagokban számol; most minden kép sorba kerül, és a kijelző egyenletes órával, kb. 30 ms késéssel veszi ki őket. A gép sebessége nem változott (49,76 kép/s, ahogy a valódi TVC-n).
- Bármilyen monitorfrekvencián (60, 75, 120, 144, 180 Hz…) működik: indításkor és monitorváltáskor a program 1 másodpercig méri, követi-e a VSync a monitort; ha nem (macOS-en az OpenGL 60 Hz-re fogja a 180 Hz-es monitort is), magától átáll a monitor ütemére, a képhatások maradnak.
- Mérés: `FREETVC_FRAME_LOG=fájl` környezeti változóval a GUI naplózza a kirakott képeket, a `tools/frame_pacing_check.py fájl` kiírja, maradt-e ki kép.

### Letöltések

| Fájl | Mire |
|---|---|
| `freeTVC-1.3.1-windows-x64.zip` | 64 bites Windows: bárhová kicsomagolható, a `freetvc.exe` indítja. A program nincs aláírva, ezért a SmartScreen első indításkor figyelmeztethet. |
| `freeTVC-1.3.1-linux-x86_64.tar.gz` | 64 bites Linux, glibc 2.39 vagy újabb (Ubuntu 24.04, Debian 13, Fedora 40 vagy újabb): kicsomagolás után `./freetvc`. |
| `freeTVC-1.3.1-macos-arm64.zip` | macOS 13 vagy újabb, Apple Silicon (M1 és újabb): kicsomagolás után `freetvc`. |
| `freeTVC-1.3.1-macos-x86_64.zip` | macOS 13 vagy újabb, Intel. |
| `SHA256SUMS`, `SHA256SUMS-macos` | az archívumok ellenőrzőösszegei |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | a mellékelt ffmpeg forrása (GPL) |

## 1.3.0 — 2026-11-07

### Új

- **Játékmentések**: az F9 vagy a TVC gomb melletti új mentés gomb megállítja a gépet, elsötétíti a képet, és bekéri a mentés nevét. A mentés a felhasználói adatmappa `SaveState/<játék>/<név>.tvcs` fájljába kerül, a képernyő képével együtt; meglévő nevet csak rákérdezés után ír felül. A játék az utoljára indított program (vagy a lemez, a cartridge, a NanoSD vagy a pendrive mappája, a GameCard). A Shift+F9 a játék legutóbbi mentését tölti vissza; a gomb jobb klikkes menüje mutatja a játék mentéseit idővel, az összes játék mentéseit, és megnyitja a mappát. A mentés a teljes gépet tartalmazza: a gépet, a ROM-okat, a kártyákat a slotjukban, a hordozókat útvonallal, a NanoSD mappáját és a TVC256k++ pendrive-ját is, így másképp beállított gépen is visszajön. MCP `game_saves`: list, save, check (milyen fájlok kellenek a mentéshez, és megvannak-e), load (áthelyezett mappákkal), delete.
- Az alsó sáv mérete: a fölötte lévő sötét vájat húzható, vagy Beállítások → Felület → Alsó sáv (75–250 %); a gombok vele nőnek, és ha nem férnének ki, az ablak szélesebb lesz.
- A menük, a beállítások ablak és a sáv betűmérete: Beállítások → Felület → Betűk (12–36 pt).

### Változott

- Az üzenetek a sáv helyett a TV-kép felső keretén jelennek meg, sárgával.
- A sebesség (vagy a *szünet*) az ablak címében látszik: `freeTVC · <gép> · 100 %`.
- Kikerült a főmenü Pillanatkép almenüje és az egyetlen `quick.tvcs` gyorsmentés: az F9 és a Shift+F9 játékmentés. A behúzott `.tvcs` fájl továbbra is betöltődik.
- A Beállítások → Gép részben csak a ROM-mappa maradt; a gépet és a kártyákat a sáv menüiben lehet választani. A Beállítások → Kép a képtípusokat a nevükön mutatja (Fekete-fehér TV, CRT TV emuláció, RGB monitor emuláció, Éles kép). A beállítások ablak betűi akkorák, mint a menüké.

### Letöltések

| Fájl | Mire |
|---|---|
| `freeTVC-1.3.0-windows-x64.zip` | 64 bites Windows: bárhová kicsomagolható, a `freetvc.exe` indítja. A program nincs aláírva, ezért a SmartScreen első indításkor figyelmeztethet. |
| `freeTVC-1.3.0-linux-x86_64.tar.gz` | 64 bites Linux, glibc 2.39 vagy újabb (Ubuntu 24.04, Debian 13, Fedora 40 vagy újabb): kicsomagolás után `./freetvc`. |
| `freeTVC-1.3.0-macos-arm64.zip` | macOS 13 vagy újabb, Apple Silicon (M1 és újabb): kicsomagolás után `freetvc`. |
| `freeTVC-1.3.0-macos-x86_64.zip` | macOS 13 vagy újabb, Intel. |
| `SHA256SUMS`, `SHA256SUMS-macos` | az archívumok ellenőrzőösszegei |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | a mellékelt ffmpeg forrása (GPL) |

## 1.2.0 — 2026-11-07

### Új

- Videófelvétel ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): a TV-kép a TV 4:3-as arányában (1534 × 1148) és a hang MP4-be (H.264, AAC), ffmpeg-gel: az alsó sáv REC gombja (kattintás indít és leállít, jobb klikkel a mappa választható), `freetvc-headless --video FÁJL`. A csomagokban egy kis, GPL-es ffmpeg van az `ffmpeg/` almappában (`tools/release/build_ffmpeg.sh`), így semmit nem kell telepíteni; a forrása a kiadás fájljai között van.
- Új alsó sáv rajzolt gombokkal: cartridge a nevével, a gép (verziók és bekapcsolás), a billentyűzet (szimbolikus, pozíciós, kiosztás), a hangszóró (némítva áthúzva), az 5,25"-os floppy, a kazetta lejátszással, számlálóval, stoppal és visszatekeréssel, a nyomtató, REC, a fényképező (4:3-as PNG), a sebesség és a freeTVC felirat a főmenüvel. Amíg forog a szalag, a kazetta helyén a fej alatti jel kis hullámként fut.
- A TVC 64k+ halvány fotója takarja a TV-képet induláskor, bekapcsoláskor és kártyacserénél, amíg a gép nem rajzol.
- Főmenü: *Legutóbbi programok* (az utolsó tíz, a `freetvc.ini`-ben), a pillanatkép, a kinézet és a nyelv almenüje, *ROM-ok letöltése*.
- A floppy menü első pontja a *Program közvetlen betöltése*: a lemez az első üres meghajtóba kerül, és elindul rajta az első program.
- A slot listájában a NanoSD és a TVC256k++ betételkor rögtön kéri az SD-kártya mappáját, a pendrive mappáját vagy a flash képfájlt.

### Változott

- A *Program betöltése* már nem fogad cartridge-et: azt a Cart gombbal lehet betenni.
- A HBF kártya saját menüjében csak a lemezes rendszer maradt; a lemezek a floppy gombon vannak.

### Javítva

- A TVC256k++ overlay 16 színű módban csak minden második pixelt mutatta, ezért a sakktábla-mintás árnyékolás (dikdom Wolf256 címképe) fekete csíkokra esett szét; most minden pixel látszik, ahogy a 2dfx kártyánál.

### Letöltések

| Fájl | Mire |
|---|---|
| `freeTVC-1.2.0-windows-x64.zip` | 64 bites Windows: bárhová kicsomagolható, a `freetvc.exe` indítja. A program nincs aláírva, ezért a SmartScreen első indításkor figyelmeztethet. |
| `freeTVC-1.2.0-linux-x86_64.tar.gz` | 64 bites Linux, glibc 2.39 vagy újabb (Ubuntu 24.04, Debian 13, Fedora 40 vagy újabb): kicsomagolás után `./freetvc`. |
| `freeTVC-1.2.0-macos-arm64.zip` | macOS 13 vagy újabb, Apple Silicon (M1 és újabb): kicsomagolás után `freetvc`. |
| `freeTVC-1.2.0-macos-x86_64.zip` | macOS 13 vagy újabb, Intel Mac. |
| `SHA256SUMS`, `SHA256SUMS-macos` | a csomagok ellenőrzőösszegei |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | a mellékelt ffmpeg forrása (GPL) |

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
