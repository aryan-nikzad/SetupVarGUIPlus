
<h2>Screenshots</h2>

<table>
  <tr>
    <td><img src="SetupVarGUI_screenshots/01_gfx_engine_first_test.png"></td>
    <td><img src="SetupVarGUI_screenshots/02_splash_disclaimer.png"></td>
    <td><img src="SetupVarGUI_screenshots/03_aio_tab_default_list.png"></td>
  </tr>
  <tr>
    <td><img src="SetupVarGUI_screenshots/04_aio_tab_live_search.png"></td>
    <td><img src="SetupVarGUI_screenshots/05_editor_sriov_not_found_in_qemu.png"></td>
    <td><img src="SetupVarGUI_screenshots/06_menu_tab_categories_before_fix.png"></td>
  </tr>
  <tr>
    <td><img src="SetupVarGUI_screenshots/07_menu_drilled_spacing_bug.png"></td>
    <td><img src="SetupVarGUI_screenshots/08_more_tab_action_list.png"></td>
    <td><img src="SetupVarGUI_screenshots/09_menu_drilled_spacing_fixed.png"></td>
  </tr>
  <tr>
    <td><img src="SetupVarGUI_screenshots/10_browse_live_nvram_variables.png"></td>
    <td><img src="SetupVarGUI_screenshots/11_about_screen.png"></td>
    <td><img src="SetupVarGUI_screenshots/12_manual_edit_store_name_input.png"></td>
  </tr>
  <tr>
    <td><img src="SetupVarGUI_screenshots/13_editor_real_timeout_variable.png"></td>
    <td><img src="SetupVarGUI_screenshots/14_write_confirmation_dialog.png"></td>
    <td><img src="SetupVarGUI_screenshots/15_write_succeeded_real_nvram_write.png"></td>
  </tr>
  <tr>
    <td><img src="SetupVarGUI_screenshots/16_aio_full_5267_entries.png"></td>
    <td><img src="SetupVarGUI_screenshots/17_menu_categories_before_driver_name_fix.png"></td>
    <td><img src="SetupVarGUI_screenshots/18_menu_categories_after_driver_name_fix.png"></td>
  </tr>
  <tr>
    <td><img src="SetupVarGUI_screenshots/19_menu_categories_final_clean_names.png"></td>
    <td></td>
    <td></td>
  </tr>
</table>




# SetupVar GUI

A native UEFI application (single `.efi` binary, no OS required) with a
real graphical interface — white background, anti-aliased text, rounded
cards, live search — for reading and writing firmware "Setup" NVRAM
variables (hidden BIOS settings).

> **This copy is pre-built for the Gigabyte X99 P SLI BIOS (X99PSLI.25b).**
> `SetupVarGUI.efi` has 5267 offsets baked in: 5266 auto-extracted from a
> full scan of that firmware, grouped into real categories (**Setup** 516,
> **Platform** 4715, two GUID-named drivers, **ReFlash** 2), plus 1 custom
> entry. Just copy `SetupVarGUI.efi` to `EFI/BOOT/BOOTX64.EFI` on a FAT32
> USB stick and boot it.
>
> Two settings worth knowing before you touch anything:
> - **SR-IOV Support** — `Setup:0x496(1)`, confirmed straight from this
>   BIOS's own IFR form data (`Min: 0x0, Max: 0x1`). Verified, safe to trust.
> - **"ABOVE 4G DECODING"** — `Setup:0x495(1)`, added at request but
>   **not confirmed**: no visible Setup-menu question in this firmware
>   points at that offset (4G decoding is commonly absent/hidden on
>   Haswell-E boards like X99). Treat it as a guess until verified another way.

> ⚠️ **This can permanently brick your motherboard if misused.** Writing
> the wrong value to the wrong offset can leave a device unbootable, with
> no software recovery on some boards. Only write values you have
> verified for your exact firmware. Use at your own risk.

## What it looks like

Boot-tested end to end in QEMU/OVMF, including a full real
read → edit → confirm → write cycle against live NVRAM. Three ways to
find a setting:

- **AIO** — one flat, live-searchable list of every baked-in setting.
  Just start typing (matches name, offset, or driver); Up/Down + Enter
  to open, Esc to clear the search.
- **Menu** — the same settings grouped by which BIOS driver they came
  from (e.g. "Setup", "Platform"). Enter a category to drill in, with
  its own filter box; Esc goes back up a level.
- **More** — manual edit by name/offset, a live browser of every NVRAM
  variable on the machine, the batch config-file loader, and About.

`Tab` cycles between AIO / Menu / More at any time. Opening any entry
goes to the same editor screen: current value, a hex input for the new
value, and a confirmation dialog before anything is written.

## Files

| File / folder                    | Purpose                                              |
|-----------------------------------|-------------------------------------------------------|
| `SetupVarGUI.efi`                  | Prebuilt binary (see note above for what's baked in)   |
| `setupvargui.c`                    | Application logic and screens                          |
| `gfx.c` / `gfx.h`                  | The GOP graphics engine (framebuffer, primitives, text) |
| `font_data.h`                      | Rasterized DejaVu Sans glyph bitmaps (generated)        |
| `offsets_data.h`                   | Currently-embedded offset tables (generated)            |
| `build.sh`                         | Compiles `gfx.c` + `setupvargui.c` -> `SetupVarGUI.efi` |
| `build_all.sh`                     | One-shot pipeline: BIOS file -> scan -> embed -> build  |
| `pipeline/parse_ifr.py`            | IFR-text -> CSV parser                                  |
| `pipeline/gen_offsets_header.py`   | CSV(s) -> `offsets_data.h` generator                     |
| `fontgen/gen_font.py`              | TTF -> `font_data.h` rasterizer (only needed to change fonts/sizes) |
| `extracted_offsets_X99PSLI.csv`    | The 5266 auto-extracted entries, for reference          |
| `custom_offsets_X99PSLI.csv`       | The 1 custom entry used in this build                   |
| `custom_offsets.csv.example`       | Template for your own custom offsets                    |
| `setupvar.cfg.example`             | Template for the runtime batch-load feature             |
| `tools/uefiextract/`, `tools/ifrextractor/` | Bundled extraction tools                       |

## Using the built binary

1. Copy `SetupVarGUI.efi` onto a **FAT32** USB stick as `EFI/BOOT/BOOTX64.EFI`.
2. Boot the USB stick from your firmware's boot menu (F9/F10/F11/F12 at
   power-on, or via the UEFI boot manager). Disable Secure Boot first if
   needed. A Graphics Output Protocol (GOP) display is required — this is
   standard on essentially all UEFI systems since ~2010.
3. Navigate with arrow keys, type to search on the AIO tab, Enter to
   select, Esc to go back, Tab to switch modes.

## Rebuilding for a different BIOS

Requires `gnu-efi` and Python 3 (with Pillow only if you also want to
regenerate the font, which you don't need to do normally):

```bash
sudo apt-get install gnu-efi python3
./build_all.sh path/to/your_bios.rom [custom_offsets.csv] [name-filter]
```

- `your_bios.rom` — your motherboard's BIOS/UEFI firmware image.
- `custom_offsets.csv` *(optional)* — your own offset/label entries. See
  `custom_offsets.csv.example`.
- `name-filter` *(optional)* — restrict the scan to drivers whose name
  contains this text (e.g. `Setup`), for speed. Default is a full scan.

This produces `build/SetupVarGUI.efi` and `build/extracted_offsets.csv`
(the intermediate table, worth a look — it's what "Known offsets" pulls
from). A plain `./build.sh` recompiles without touching `offsets_data.h`,
if you've hand-edited it.

## How the pipeline works

1. **UEFIExtract** unpacks the BIOS file's firmware-volume tree.
2. **IFRExtractor-RS** runs against every PE32 driver with HII forms,
   decoding the embedded IFR bytecode into human-readable text.
3. **`pipeline/parse_ifr.py`** parses `Prompt:` / `VarStoreInfo:` /
   `VarOffset:` / `Size:` fields and the `VarStore` name table into a CSV
   of `store,offset,size,label,source`.
4. **`pipeline/gen_offsets_header.py`** turns that CSV (+ your custom CSV)
   into `offsets_data.h`, compiled straight into the binary.

Both `UEFIExtract` and `IFRExtractor-RS` (BSD-licensed, from
[LongSoft](https://github.com/LongSoft)) are bundled under `tools/`;
`build_all.sh` auto-downloads them if missing.

## How the graphics work

No OS, no GUI toolkit — this talks straight to firmware. `gfx.c` locates
the Graphics Output Protocol, picks the highest available resolution,
and draws into an off-screen buffer that gets `Blt`'d to the screen each
frame (no flicker). Text is anti-aliased: `fontgen/gen_font.py` rasterizes
DejaVu Sans (Bitstream Vera / DejaVu license — embedding explicitly
permitted) into 8-bit coverage bitmaps at four sizes, and `gfx.c` alpha-
blends each glyph onto whatever's already drawn. Rounded rectangles use
2x2 supersampling on the corners for soft edges. There's no text-rendering
API in UEFI at all below this — everything here is hand-rolled.

## Offset-table format notes

CSV columns: `store,offset,size,label[,source]`

- `store` — NVRAM variable name (almost always `Setup`)
- `offset` — hex byte offset into that variable's data
- `size` — 1, 2 or 4 bytes (from the IFR bit-width where available;
  defaults to 1 otherwise)
- `label` — human-readable setting name from the BIOS's own string table
- `source` — which driver it came from (this is what the Menu tab groups by)

`custom_offsets.csv` accepts a looser 2-column form:
`0x495,above 4g decode` (store defaults to `Setup`, size to 1, source
shows as "Custom"). A label containing a comma is fine either quoted or
not — the parser reassembles it either way.

## Known limitations

- Offset **sizes** aren't always explicit in the IFR data; where missing,
  the pipeline defaults to 1 byte.
- The scan finds *labels and offsets*, not what specific values mean —
  cross-check against the real BIOS setup menu, or the source `.ifr.txt`
  files under `build/` (kept after a `build_all.sh` run) for full
  `OneOfOption` value lists.
- A handful of drivers resolve to a raw GUID instead of a friendly name
  in the Menu tab (UEFIExtract couldn't resolve a name for them) — still
  fully usable, just less readable as a category label.
- Full-firmware scans can take several seconds on large images; use the
  `name-filter` argument once you know which driver you care about.



  ### Made by Claude Assistance, Thank you buddy ❤️
