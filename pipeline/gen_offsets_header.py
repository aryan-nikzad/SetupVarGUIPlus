#!/usr/bin/env python3
"""
gen_offsets_header.py - turn one or two CSV files (auto-extracted offsets
from a BIOS scan, and/or a hand-written custom offsets list) into a C
header (offsets_data.h) that setupvargui.c #includes, giving the GUI two
built-in menus: "Known Offsets (from BIOS scan)" and "Custom Offsets".

CSV columns expected: store,offset,size,label[,source]
  - store  : NVRAM variable name, e.g. "Setup"          (required)
  - offset : hex like 0x46 or plain decimal              (required)
  - size   : 1, 2 or 4                                    (optional, default 1)
  - label  : free text shown in the menu                  (required)
  - source : ignored by the generator, informational only

Custom CSV (user-authored) may omit columns from the right, e.g. just:
    0x495,above 4g decode
in which case store defaults to "Setup" and size defaults to 1.
"""

import argparse
import csv
import os
import sys

MAX_STORE = 63     # matches CHAR16 StoreName[64] room in the C struct
MAX_LABEL = 127     # matches CHAR16 Desc[128]


def esc_c16(s, maxlen):
    """Escape a python string into the body of a CHAR16 L"..." literal."""
    s = s[:maxlen]
    out = []
    for ch in s:
        cp = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif 0x20 <= cp < 0x7F:
            out.append(ch)
        else:
            out.append('?')  # our text console is ASCII-only
    return ''.join(out)


def parse_int(s):
    s = s.strip()
    if not s:
        return 0
    try:
        return int(s, 16) if s.lower().startswith("0x") else int(s, 10)
    except ValueError:
        return 0


def looks_like_offset(s):
    s = s.strip()
    if not s:
        return False
    if s.lower().startswith("0x"):
        return len(s) > 2 and all(c in "0123456789abcdefABCDEF" for c in s[2:])
    return s.isdigit()


def load_csv(path, default_store="Setup", strict=False, default_source="Custom"):
    """strict=True: parse our own parse_ifr.py output (always properly
    CSV-quoted, fixed column order store,offset,size,label,source) - take
    the first 5 columns.

    strict=False: parse a hand-written custom CSV, which may be loosely
    formatted (2, 3, or 4 columns; possibly with an unquoted comma inside
    the label). Detect the offset column and rejoin everything after
    offset[,size] as the label instead of guessing a fixed column count.
    Source is always default_source ("Custom") for these.
    """
    rows = []
    if not path or not os.path.isfile(path):
        return rows
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
        reader = csv.reader(f)
        first_real_row = True
        for row in reader:
            if not row or (row[0].strip().startswith("#")):
                continue
            if first_real_row:
                first_real_row = False
                joined = ",".join(row).lower()
                if "offset" in joined and "label" in joined:
                    continue  # header row
            row = [c.strip() for c in row]

            if strict:
                if len(row) < 4:
                    continue
                store, offset, size, label = row[0], row[1], row[2], row[3]
                source = row[4] if len(row) >= 5 and row[4] else "BIOS scan"
            else:
                # Locate the offset column (col 0 for "offset,label,...",
                # col 1 for "store,offset,..."), then rejoin every
                # remaining column with commas into the label, so a label
                # that itself contains a comma but wasn't quoted in the
                # source CSV still comes through whole instead of
                # silently truncating and spilling its tail elsewhere.
                if looks_like_offset(row[0]):
                    store, off_idx = default_store, 0
                elif len(row) >= 2 and looks_like_offset(row[1]):
                    store, off_idx = (row[0] or default_store), 1
                else:
                    continue  # no recognizable offset column; skip

                offset = row[off_idx]
                rest = row[off_idx + 1:]
                if not rest:
                    continue

                if len(rest) >= 2 and rest[0] in ("1", "2", "4", "0x1", "0x2", "0x4"):
                    size, label = rest[0], ", ".join(rest[1:])
                else:
                    size, label = "1", ", ".join(rest)
                source = default_source

            off = parse_int(offset)
            sz = parse_int(size) or 1
            if sz not in (1, 2, 4):
                sz = 1
            label = label.strip()
            if not label:
                continue
            rows.append((store, off, sz, label, source))
    return rows


def emit_array(rows, c_name):
    lines = []
    lines.append("static const PREFILLED_ENTRY %s[] = {" % c_name)
    if not rows:
        lines.append('    { L"", 0, 0, L"", L"" },')
    else:
        for store, off, sz, label, source in rows:
            store_lit = esc_c16(store, MAX_STORE)
            label_lit = esc_c16(label, MAX_LABEL)
            source_lit = esc_c16(source, MAX_LABEL)
            lines.append('    { L"%s", 0x%X, %d, L"%s", L"%s" },' %
                          (store_lit, off, sz, label_lit, source_lit))
    lines.append("};")
    lines.append("#define %s_COUNT %d" % (c_name, len(rows)))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--known", help="CSV of auto-extracted offsets (from parse_ifr.py)")
    ap.add_argument("--custom", help="CSV of hand-written custom offsets")
    ap.add_argument("-o", "--output", default="offsets_data.h")
    args = ap.parse_args()

    known_rows = load_csv(args.known, strict=True)
    custom_rows = load_csv(args.custom, strict=False)

    header = []
    header.append("/* Auto-generated by gen_offsets_header.py - do not hand-edit. */")
    header.append("#ifndef OFFSETS_DATA_H")
    header.append("#define OFFSETS_DATA_H")
    header.append("")
    header.append("typedef struct {")
    header.append("    CONST CHAR16 *StoreName;")
    header.append("    UINTN Offset;")
    header.append("    UINTN Size;")
    header.append("    CONST CHAR16 *Desc;")
    header.append("    CONST CHAR16 *Source;")
    header.append("} PREFILLED_ENTRY;")
    header.append("")
    header.append(emit_array(known_rows, "KNOWN_OFFSETS"))
    header.append("")
    header.append(emit_array(custom_rows, "CUSTOM_OFFSETS"))
    header.append("")
    header.append("#endif /* OFFSETS_DATA_H */")

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(header) + "\n")

    print("Wrote %d known + %d custom entries -> %s" %
          (len(known_rows), len(custom_rows), args.output), file=sys.stderr)


if __name__ == "__main__":
    main()
