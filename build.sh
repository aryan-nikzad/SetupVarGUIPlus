#!/bin/sh
# Build SetupVarGUI.efi using gnu-efi. Works on Debian/Ubuntu with:
#   sudo apt-get install gnu-efi
set -e

ARCH=x86_64
EFIINC=/usr/include/efi
EFILIB=/usr/lib
LDS=$EFILIB/elf_x86_64_efi.lds
CRT0=$EFILIB/crt0-efi-x86_64.o

CC=gcc
OBJCOPY=objcopy

CFLAGS="-I$EFIINC -I$EFIINC/x86_64 -fpic -ffreestanding -fno-stack-protector \
        -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
        -DEFI_FUNCTION_WRAPPER -c"

LDFLAGS="-nostdlib -znocombreloc -T $LDS -shared -Bsymbolic -L$EFILIB \
         $CRT0"

echo "[*] Compiling gfx.c"
$CC $CFLAGS gfx.c -o gfx.o

echo "[*] Compiling setupvargui.c"
$CC $CFLAGS setupvargui.c -o setupvargui.o

echo "[*] Linking"
ld $LDFLAGS gfx.o setupvargui.o -o setupvargui.so -lgnuefi -lefi

echo "[*] Converting to PE32+ .efi"
$OBJCOPY -j .text -j .sdata -j .data -j .dynamic \
         -j .dynsym -j .rel -j .rela -j .reloc \
         --target=efi-app-x86_64 setupvargui.so SetupVarGUI.efi

echo "[+] Built SetupVarGUI.efi"
ls -la SetupVarGUI.efi
