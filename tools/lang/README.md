# tools/lang: UI translations

The GUI's translations are gettext `.po` catalogs in `data/lang/` (one per language; [ui.md](../../docs/design/ui.md), section "Language"). `po_check.py` finds the translatable strings in `src/` and checks the catalogs against them. It needs only Python 3's standard library.

## What counts as a translatable string

`po_check.py` reads every `.cpp` and `.hpp` file under the source folder and takes:
- the first argument of `tr()`, `N_()`, `tr_format()` and `msg()` (`msg()` builds the messages of the shared code in `src/core/` and `src/frontend/common/`);
- the second argument of `.show()` and `.show_text()` (the status strip's notices);
- `trc(CONTEXT, TEXT)`, which gives TEXT a `msgctxt`.

## Commands

From the project root:

```sh
python3 -I tools/lang/po_check.py pot src freetvc.pot
msgmerge -U --backup=none data/lang/hu.po freetvc.pot
python3 -I tools/lang/po_check.py check src data/lang/hu.po
```

- `pot SRC_DIR OUT.pot` writes a template of every string.
- `msgmerge` (from GNU gettext) adds new strings to a catalog, marks changed ones fuzzy and comments out removed ones.
- `check SRC_DIR PO...` fails unless every string is translated, none is fuzzy or stale, and each translation keeps the source's printf conversions in the same order and its leading and trailing spaces and line breaks. A clean run prints a line such as `764 strings, 1 catalogs, 0 problems` and exits with 0.

`check` runs as the CTest test `ui.translation_coverage` (when CMake finds Python 3), so a new UI string without a translation fails the test set. The test `ui.translation` checks the GUI's own `.po` parser on `data/lang/`.

Poedit or any other `.po` editor works on the files; `msgfmt --check -o /dev/null data/lang/hu.po` is a second opinion on the syntax.

## Adding a language

Create `data/lang/<code>.po` from the template, with `Language: <code>` and `X-Language-Name: <the language's own name>` in the header. The build copies `data/lang/` next to the GUI as `lang/`, and the GUI lists every catalog in the main menu's Language section; `--language <code>` and `language` in `freetvc.ini` select it.

## Wording rules for Hungarian

- No article (a/az) and no suffix attached to a value, since both depend on how the value sounds: `kinézet: %s`, `%s kivétele`, `GameCard: %d. kártyahely`.
- Product, chip and monitor command names stay unchanged.
- Check narrow Bench fields with a window grab ([tools/ui/README.md](../ui/README.md)).
