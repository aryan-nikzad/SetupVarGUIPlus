#!/bin/bash
# build_all.sh - one-shot pipeline:
#   your BIOS file -> UEFIExtract -> IFRExtractor-RS (run against every
#   PE32 driver that has HII forms) -> parsed offset/label CSV -> embedded
#   into the GUI source -> compiled into a single SetupVarGUI.efi
#
# Usage:
#   ./build_all.sh path/to/bios.rom [path/to/custom_offsets.csv] [name-filter]
#
#   path/to/bios.rom            required. Your motherboard's BIOS/UEFI
#                                firmware image (the same file you'd flash,
#                                or a dump/backup of it).
#   path/to/custom_offsets.csv  optional. Your own offset,label entries
#                                (see custom_offsets.csv.example).
#   name-filter                 optional. If given, only PE32 drivers whose
#                                UEFIExtract folder name contains this text
#                                (case-insensitive) are scanned - e.g. pass
#                                "Setup" to only scan drivers literally named
#                                like the "Setup" driver, which is much
#                                faster on firmware with hundreds of drivers.
#                                Default: scan everything (slower, complete).
#
# Output: build/SetupVarGUI.efi  (plus build/extracted_offsets.csv, the
#         intermediate offset table, for your own inspection)
#
# Safety: this only READS your BIOS file to build a lookup table of
# offsets/labels; it does not modify the BIOS file itself. Writing NVRAM
# variables only happens later, when YOU choose to, inside the built GUI
# on the actual machine.

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIOS_FILE="$1"
CUSTOM_CSV="$2"
NAME_FILTER="$3"

if [ -z "$BIOS_FILE" ] || [ ! -f "$BIOS_FILE" ]; then
    echo "Usage: $0 path/to/bios.rom [custom_offsets.csv] [name-filter]"
    echo
    echo "  bios.rom            required, your firmware image"
    echo "  custom_offsets.csv  optional, your own offset/label list"
    echo "  name-filter         optional, e.g. \"Setup\" to only scan"
    echo "                      drivers whose name contains this text"
    exit 1
fi

BUILD_DIR="$HERE/build"
DUMP_DIR="$BUILD_DIR/dump"
SCAN_DIR="$BUILD_DIR/ifr_txt"
rm -rf "$BUILD_DIR"
mkdir -p "$DUMP_DIR" "$SCAN_DIR"

UEFIEXTRACT="$HERE/tools/uefiextract/uefiextract"
IFREXTRACTOR="$HERE/tools/ifrextractor/ifrextractor"

download_tools() {
    echo "[*] Downloading UEFIExtract / IFRExtractor-RS (not found locally)..."
    mkdir -p "$HERE/tools"
    if [ ! -x "$UEFIEXTRACT" ]; then
        mkdir -p "$HERE/tools/uefiextract"
        TAG=$(curl -s https://api.github.com/repos/LongSoft/UEFITool/releases/latest | python3 -c "import json,sys;print(json.load(sys.stdin)['tag_name'])")
        curl -sL -o /tmp/uefiextract.zip "https://github.com/LongSoft/UEFITool/releases/download/${TAG}/UEFIExtract_NE_${TAG}_x64_linux.zip"
        unzip -oq /tmp/uefiextract.zip -d "$HERE/tools/uefiextract"
        chmod +x "$UEFIEXTRACT"
    fi
    if [ ! -x "$IFREXTRACTOR" ]; then
        mkdir -p "$HERE/tools/ifrextractor"
        VER=$(curl -s https://api.github.com/repos/LongSoft/IFRExtractor-RS/releases | python3 -c "import json,sys;print(json.load(sys.stdin)[0]['tag_name'])")
        curl -sL -o /tmp/ifrextractor.zip "https://github.com/LongSoft/IFRExtractor-RS/releases/download/${VER}/ifrextractor_${VER#v}_linux.zip"
        unzip -oq /tmp/ifrextractor.zip -d "$HERE/tools/ifrextractor"
        chmod +x "$IFREXTRACTOR"
    fi
}

if [ ! -x "$UEFIEXTRACT" ] || [ ! -x "$IFREXTRACTOR" ]; then
    download_tools
fi

echo "[1/5] Extracting firmware volume tree from $BIOS_FILE ..."
cp "$BIOS_FILE" "$DUMP_DIR/input.bin"
( cd "$DUMP_DIR" && "$UEFIEXTRACT" input.bin all >uefiextract.log 2>&1 ) || {
    echo "!!! UEFIExtract failed, see $DUMP_DIR/uefiextract.log"
    tail -20 "$DUMP_DIR/uefiextract.log"
    exit 1
}

if [ -n "$NAME_FILTER" ]; then
    echo "[2/5] Scanning PE32 drivers matching name filter \"$NAME_FILTER\" for HII forms ..."
else
    echo "[2/5] Scanning ALL PE32 drivers for HII forms (this can take a while on large firmware) ..."
fi

count=0
scanned=0
while IFS= read -r -d '' f; do
    scanned=$((scanned + 1))
    drivername=$(basename "$(dirname "$(dirname "$(dirname "$f")")")")
    drivername=$(echo "$drivername" | sed 's/^[0-9]\+ //')
    if [ -n "$NAME_FILTER" ]; then
        case "${drivername,,}" in
            *"${NAME_FILTER,,}"*) : ;;
            *) continue ;;
        esac
    fi
    outdir=$(dirname "$f")
    ( cd "$outdir" && "$IFREXTRACTOR" "$(basename "$f")" >/dev/null 2>&1 ) || true
    # sanitize driver name for use as a filename prefix, and encode it with
    # a delimiter parse_ifr.py can split on (directory depth is lost once
    # every file is copied into one flat SCAN_DIR, so the driver name has
    # to travel some other way - it's smuggled into the filename itself)
    safe_name=$(echo "$drivername" | sed 's/[^A-Za-z0-9._-]/_/g')
    for txt in "$outdir"/*.ifr.txt; do
        [ -e "$txt" ] || continue
        count=$((count + 1))
        cp "$txt" "$SCAN_DIR/${safe_name}__$(basename "$txt")" 2>/dev/null || true
    done
done < <(find "$DUMP_DIR" -path "*PE32 image section/body.bin" -print0)

echo "    scanned $scanned PE32 sections, collected $count .ifr.txt output(s)"

echo "[3/5] Parsing IFR text into an offset/label table ..."
python3 "$HERE/pipeline/parse_ifr.py" "$SCAN_DIR" -o "$BUILD_DIR/extracted_offsets.csv"

echo "[4/5] Generating embedded C header ..."
GEN_ARGS=(--known "$BUILD_DIR/extracted_offsets.csv" -o "$HERE/offsets_data.h")
if [ -n "$CUSTOM_CSV" ] && [ -f "$CUSTOM_CSV" ]; then
    GEN_ARGS+=(--custom "$CUSTOM_CSV")
    echo "    including custom offsets from $CUSTOM_CSV"
fi
python3 "$HERE/pipeline/gen_offsets_header.py" "${GEN_ARGS[@]}"

echo "[5/5] Building SetupVarGUI.efi ..."
( cd "$HERE" && ./build.sh )

cp "$HERE/SetupVarGUI.efi" "$BUILD_DIR/SetupVarGUI.efi"

echo
echo "=================================================================="
echo " Done. Final binary: $BUILD_DIR/SetupVarGUI.efi"
echo " Offset table used:  $BUILD_DIR/extracted_offsets.csv"
echo "=================================================================="
echo " Copy SetupVarGUI.efi to a FAT32 USB drive as EFI/BOOT/BOOTX64.EFI"
echo " and boot it from your firmware's boot menu."
echo
echo " Remember: writing the wrong value to the wrong offset can brick"
echo " your motherboard. Verify labels/offsets before writing anything."
echo "=================================================================="
