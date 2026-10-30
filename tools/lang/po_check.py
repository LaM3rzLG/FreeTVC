#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The GUI's translatable strings (design/ui.md "Language").

  po_check.py pot SRC_DIR OUT.pot     writes a template of every string in SRC_DIR's .cpp/.hpp files (SRC_DIR: src)
  po_check.py check SRC_DIR PO...     fails unless each catalog translates exactly those strings, with matching formats

A string is translatable as the first argument of tr(), N_(), tr_format() or msg(), or the second of a .show() or .show_text() call;
trc(CONTEXT, TEXT) gives it a msgctxt.
"""

import os
import re
import sys

KEYWORDS = {"tr": 0, "N_": 0, "tr_format": 0, "trc": 0, "msg": 0, "show": 1, "show_text": 1}
FORMAT = re.compile(r"%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcspn])")
TOKEN = re.compile(r"""
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:[^"\\\n]|\\.)*")
  | (?P<char>'(?:[^'\\\n]|\\.)*')
  | (?P<name>[A-Za-z_]\w*)
  | (?P<space>\s+)
  | (?P<other>.)
""", re.S | re.X)
HEADER = ('msgid ""\nmsgstr ""\n"Project-Id-Version: freeTVC\\n"\n"Language-Team: freeTVC\\n"\n"Last-Translator: freeTVC\\n"\n'
          '"PO-Revision-Date: \\n"\n"MIME-Version: 1.0\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n'
          '"Content-Transfer-Encoding: 8bit\\n"\n"Language: {language}\\n"\n"X-Language-Name: {name}\\n"\n')
ESCAPES = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11, '"': 34, "\\": 92, "'": 39, "?": 63}


def c_bytes(literal):
    """The bytes of a C string literal's body."""
    out = bytearray()
    i = 0
    while i < len(literal):
        c = literal[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        e = literal[i + 1]
        if e in ESCAPES:
            out.append(ESCAPES[e])
            i += 2
        elif e == "x":
            m = re.match(r"[0-9A-Fa-f]{1,2}", literal[i + 2:])
            out.append(int(m.group(0), 16))
            i += 2 + len(m.group(0))
        else:
            m = re.match(r"[0-7]{1,3}", literal[i + 1:])
            out.append(int(m.group(0), 8))
            i += 1 + len(m.group(0))
    return bytes(out)


def tokens(text):
    line = 1
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind not in ("comment", "space"):
            yield kind, m.group(0), line
        line += m.group(0).count("\n")


def literal(toks, j):
    """The concatenated string literal at toks[j] and the index after it; None if there is none."""
    parts = []
    while j < len(toks) and toks[j][0] == "string":
        parts.append(c_bytes(toks[j][1][1:-1]))
        j += 1
    return (b"".join(parts).decode("utf-8") if parts else None), j


def extract(src_dir):
    """(msgctxt or None, msgid) -> list of "file:line" places, in source order."""
    found = {}
    for root, _, files in os.walk(src_dir):
        for name in sorted(files):
            if not name.endswith((".cpp", ".hpp")):
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8") as f:
                toks = [t for t in tokens(f.read()) if not (t[0] == "other" and t[1] == "#")]
            for i, (kind, text, line) in enumerate(toks):
                if kind != "name" or text not in KEYWORDS or i + 1 >= len(toks) or toks[i + 1][1] != "(":
                    continue
                if KEYWORDS[text] == 1 and (i == 0 or toks[i - 1][1] != "."):
                    continue
                j = i + 2
                if KEYWORDS[text] == 1:
                    depth = 0
                    while j < len(toks) and not (depth == 0 and toks[j][1] == ","):
                        depth += {"(": 1, ")": -1}.get(toks[j][1], 0)
                        if depth < 0:
                            break
                        j += 1
                    j += 1
                context = None
                if text == "trc":
                    context, j = literal(toks, j)
                    if context is None or j >= len(toks) or toks[j][1] != ",":
                        continue
                    j += 1
                msgid, j = literal(toks, j)
                if msgid is not None:
                    places = found.setdefault((context, msgid), [])
                    if f"{os.path.relpath(path)}:{line}" not in places:
                        places.append(f"{os.path.relpath(path)}:{line}")
    return found


def po_quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\n", "\\n")
    return '"' + s + '"'


def po_field(keyword, s):
    pieces = re.findall(r"[^\n]*\n|[^\n]+$", s)
    if len(pieces) <= 1:
        return f"{keyword} {po_quote(s)}\n"
    return f'{keyword} ""\n' + "".join(po_quote(p) + "\n" for p in pieces)


def write_pot(found, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(HEADER.format(language="", name=""))
        for (context, msgid), places in found.items():
            f.write("\n#: " + " ".join(places) + "\n")
            if FORMAT.search(msgid):
                f.write("#, c-format\n")
            if context is not None:
                f.write(po_field("msgctxt", context))
            f.write(po_field("msgid", msgid))
            f.write('msgstr ""\n')


def unquote(s):
    if len(s) < 2 or s[0] != '"' or s[-1] != '"':
        raise ValueError("not a string")
    return c_bytes(s[1:-1]).decode("utf-8")


def read_po(path):
    """(header dict, list of entries {id, str, fuzzy, line}), raising ValueError on malformed lines."""
    entries = []
    entry = None
    field = None
    fuzzy = False
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#~"):
            if not line:
                entry, field = None, None
            continue
        if line.startswith("#"):
            if entry is not None and "str" in entry:
                entry, field = None, None
            if line.startswith("#,") and "fuzzy" in line:
                fuzzy = True
            continue
        try:
            if line.startswith('"'):
                if entry is None or field is None:
                    raise ValueError("a string without a keyword")
                entry[field] += unquote(line)
                continue
            keyword, _, value = line.partition(" ")
            if keyword in ("msgctxt", "msgid") and entry is not None and "str" in entry:
                entry, field = None, None
            if entry is None:
                entry = {"line": number, "fuzzy": fuzzy}
                fuzzy = False
                entries.append(entry)
            if keyword not in ("msgctxt", "msgid", "msgstr"):
                raise ValueError(f"keyword {keyword} is not used here")
            field = {"msgctxt": "ctx", "msgid": "id", "msgstr": "str"}[keyword]
            entry[field] = unquote(value.strip())
        except ValueError as e:
            raise ValueError(f"{path}:{number}: {e}") from None
    header = {}
    if entries and entries[0].get("id") == "":
        for item in entries.pop(0).get("str", "").split("\n"):
            key, _, value = item.partition(":")
            if key.strip():
                header[key.strip()] = value.strip()
    return header, entries


def edge_spaces(s):
    return len(s) - len(s.lstrip()), len(s) - len(s.rstrip())


def check(found, path):
    problems = []
    try:
        header, entries = read_po(path)
    except ValueError as e:
        return [str(e)]
    for key in ("Language", "X-Language-Name"):
        if not header.get(key):
            problems.append(f"{path}: the header has no {key}")
    if "charset=UTF-8" not in header.get("Content-Type", ""):
        problems.append(f"{path}: the header's Content-Type is not charset=UTF-8")
    seen = set()
    for e in entries:
        where = f"{path}:{e['line']}"
        msgid, msgstr = e.get("id"), e.get("str", "")
        if msgid is None:
            problems.append(f"{where}: no msgid")
            continue
        key = (e.get("ctx"), msgid)
        if key in seen:
            problems.append(f"{where}: {msgid!r} again")
        seen.add(key)
        if key not in found:
            problems.append(f"{where}: {msgid!r} is no longer in the sources")
            continue
        if e["fuzzy"]:
            problems.append(f"{where}: {msgid!r} is fuzzy")
        if not msgstr:
            problems.append(f"{where}: {msgid!r} is not translated")
            continue
        if FORMAT.findall(msgid) != FORMAT.findall(msgstr):
            problems.append(f"{where}: {msgid!r}: the formats {FORMAT.findall(msgid)} became {FORMAT.findall(msgstr)}")
        if edge_spaces(msgid) != edge_spaces(msgstr) or msgid.count("\n") != msgstr.count("\n"):
            problems.append(f"{where}: {msgid!r}: leading/trailing spaces or line breaks differ")
    for (context, msgid), places in found.items():
        if (context, msgid) not in seen:
            problems.append(f"{path}: {msgid!r} ({places[0]}) is missing")
    return problems


def main(argv):
    if len(argv) >= 4 and argv[1] == "pot":
        found = extract(argv[2])
        write_pot(found, argv[3])
        print(f"{len(found)} strings")
        return 0
    if len(argv) >= 4 and argv[1] == "check":
        found = extract(argv[2])
        if not found:
            print(f"no strings found in {argv[2]}")
            return 1
        problems = [p for po in argv[3:] for p in check(found, po)]
        for p in problems:
            print(p)
        print(f"{len(found)} strings, {len(argv) - 3} catalogs, {len(problems)} problems")
        return 1 if problems else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
