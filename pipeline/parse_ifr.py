#!/usr/bin/env python3
"""
parse_ifr.py - Parse IFRExtractor-RS .uefi.ifr.txt output file(s) into a
flat CSV of (store, offset, size, label, source) entries.

IFRExtractor-RS output shape (v1.6.x), relevant lines:

    FormSet Guid: ..., Title: "...", Help: "..."
        VarStore Guid: <GUID>, VarStoreId: 0x1, Size: 0x24, Name: "MainFormState"
        Form FormId: 0x1, Title: "..."
            String Prompt: "...", Help: "...", QuestionFlags: 0x1,
                QuestionId: 0x1, VarStoreId: 0x1, VarStoreInfo: 0x0, ...
            Checkbox Prompt: "...", ..., VarStoreId: 0x1, VarStoreInfo: 0x5, ...

VarStoreId numbering (and the Name it maps to) is local to each FormSet
block, so the VarStoreId->Name map is reset every time a new "FormSet"
line is seen. Entries with VarStoreId 0x0 or VarStoreInfo 0xFFFF are
"no backing storage" (buttons, etc.) and are skipped.

An older IFRExtractor-RS output style also exists:
    VarStore: VarStoreId: 0x1 [GUID], Size: 0x219, Name: Setup {...}
    Checkbox: Label, VarStoreInfo (VarOffset/VarName): 0x5, VarStore: 0x1, ...
Both styles are supported.
"""

import argparse
import csv
import os
import re
import sys

RE_FORMSET = re.compile(r'^\s*FormSet\b')

RE_VARSTORE_NEW = re.compile(
    r'VarStore\s+Guid:\s*[0-9A-Fa-f-]+,\s*VarStoreId:\s*(0x[0-9A-Fa-f]+),'
    r'\s*Size:\s*(0x[0-9A-Fa-f]+),\s*Name:\s*"([^"]*)"'
)
RE_VARSTORE_OLD = re.compile(
    r'VarStore:\s*VarStoreId:\s*(0x[0-9A-Fa-f]+)\s*\[[0-9A-Fa-f-]+\],'
    r'\s*Size:\s*(0x[0-9A-Fa-f]+),\s*Name:\s*"?([A-Za-z0-9_]+)"?'
)

RE_PROMPT = re.compile(r'Prompt:\s*"([^"]*)"')
RE_QID = re.compile(r'QuestionId:\s*(0x[0-9A-Fa-f]+)')

# new style: separate VarStoreId + (VarStoreInfo | VarOffset) fields.
# String/Password-type opcodes use "VarStoreInfo: 0xNN" (byte offset into
# the buffer); Numeric/OneOf/CheckBox/Date/Time use "VarOffset: 0xNN" plus
# a decimal bit-width "Size: N" field on the same line.
RE_VSID_NEW = re.compile(r'(?<!Default)VarStoreId:\s*(0x[0-9A-Fa-f]+)')
RE_VSINFO_NEW = re.compile(r'VarStoreInfo:\s*(0x[0-9A-Fa-f]+)')
RE_VAROFFSET_NEW = re.compile(r'VarOffset:\s*(0x[0-9A-Fa-f]+)')
RE_BITSIZE = re.compile(r'(?<![A-Za-z])Size:\s*(\d+)\b')

# old style: combined "VarStoreInfo (VarOffset/VarName): 0xNN, VarStore: 0xNN"
RE_VSINFO_OLD = re.compile(
    r'VarStoreInfo\s*\(VarOffset/VarName\):\s*(0x[0-9A-Fa-f]+),\s*VarStore:\s*(0x[0-9A-Fa-f]+)'
)


def parse_file(path):
    """Return list of dicts: store, offset(int), size(int), label, source."""
    results = []
    varstores = {}  # id(int) -> (name, size)
    driver_name = path
    base = os.path.basename(path)
    if "__" in base:
        # build_all.sh flattens every .ifr.txt into one directory and
        # encodes the originating driver's name as a "safe_name__..."
        # filename prefix (see build_all.sh) since the real directory
        # structure doesn't survive that flattening.
        driver_name = base.split("__", 1)[0]
    elif os.sep in path:
        # direct invocation against a file still under UEFIExtract's own
        # dump tree (driver folder is three levels up from the .ifr.txt)
        up3 = os.path.dirname(os.path.dirname(os.path.dirname(path)))
        dirbase = os.path.basename(up3)
        driver_name = re.sub(r'^\d+\s+', '', dirbase) if dirbase else path

    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return results

    for line in lines:
        if RE_FORMSET.search(line):
            varstores = {}
            continue

        m = RE_VARSTORE_NEW.search(line) or RE_VARSTORE_OLD.search(line)
        if m:
            vid = int(m.group(1), 16)
            size = int(m.group(2), 16)
            name = m.group(3).strip() or "Setup"
            varstores[vid] = (name, size)
            continue

        pm = RE_PROMPT.search(line)
        if not pm:
            continue
        label = pm.group(1).strip()
        if not label:
            continue

        # try new style first: VarStoreId is always present; the offset is
        # given either as VarStoreInfo (buffer-offset opcodes) or VarOffset
        # (value opcodes, which also carry a decimal bit-width Size field)
        vsid_m = RE_VSID_NEW.search(line)
        vsid = offset = None
        bit_size = None
        if vsid_m:
            info_m = RE_VSINFO_NEW.search(line)
            voff_m = RE_VAROFFSET_NEW.search(line)
            if info_m:
                vsid = int(vsid_m.group(1), 16)
                offset = int(info_m.group(1), 16)
            elif voff_m:
                vsid = int(vsid_m.group(1), 16)
                offset = int(voff_m.group(1), 16)
                bsz_m = RE_BITSIZE.search(line)
                if bsz_m:
                    bit_size = int(bsz_m.group(1))

        if vsid is None or offset is None:
            old_m = RE_VSINFO_OLD.search(line)
            if old_m:
                offset = int(old_m.group(1), 16)
                vsid = int(old_m.group(2), 16)

        if vsid is None or offset is None:
            continue
        if vsid == 0 or offset == 0xFFFF:
            continue  # no backing storage (button/action)

        store_name, store_size = varstores.get(vsid, ("Setup", None))

        if bit_size:
            size_bytes = max(1, (bit_size + 7) // 8)
            if size_bytes not in (1, 2, 4):
                size_bytes = 4 if size_bytes > 2 else 2
        else:
            size_bytes = 1  # String/Password buffer offsets etc: no fixed
                             # scalar width given; 1 is a safe default and
                             # is editable in-app or via the custom CSV

        results.append({
            "store": store_name,
            "offset": offset,
            "size": size_bytes,
            "label": label,
            "source": driver_name,
        })

    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inputs", nargs="+",
                     help="one or more IFRExtractor-RS .txt output files, "
                          "or directories to search recursively for them")
    ap.add_argument("-o", "--output", default="extracted_offsets.csv")
    args = ap.parse_args()

    files = []
    for inp in args.inputs:
        if os.path.isdir(inp):
            for root, _dirs, names in os.walk(inp):
                for n in names:
                    if n.endswith(".ifr.txt"):
                        files.append(os.path.join(root, n))
        elif os.path.isfile(inp):
            files.append(inp)

    all_rows = []
    seen = set()
    for f in sorted(files):
        for row in parse_file(f):
            key = (row["store"], row["offset"], row["label"])
            if key in seen:
                continue
            seen.add(key)
            all_rows.append(row)

    with open(args.output, "w", newline="", encoding="utf-8") as out:
        w = csv.writer(out)
        w.writerow(["store", "offset", "size", "label", "source"])
        for row in all_rows:
            w.writerow([
                row["store"],
                "0x%X" % row["offset"],
                row["size"],
                row["label"],
                row["source"],
            ])

    print("Parsed %d file(s), found %d unique offset entries -> %s" %
          (len(files), len(all_rows), args.output), file=sys.stderr)


if __name__ == "__main__":
    main()
