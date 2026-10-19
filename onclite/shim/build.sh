#!/bin/bash
# Build the onclite shim and wrap it like the stock kernel: Image.gz + the
# stock DTB appended, in a boot.img v1 that mirrors the stock header, with
# the xnu payload (tools/mkpayload.py) as the ramdisk:
#   onclite/shim/build.sh PAYLOAD
# RDADDR (default 0x44000000) is the ramdisk load address: the stock 0x81000000 leaves only 58 MB
# before the modem carve-out at 0x84a00000 (NOTES.md memory map).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
PAYLOAD=$(cd "$OLDPWD" && realpath "${1:?usage: build.sh PAYLOAD}")
TC=${TC:-$ROOT/.claude/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-aarch64-none-elf/bin/aarch64-none-elf-}
CC=${TC}gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c shim.c -o out/shim.o
$CC $CFLAGS -c $ROOT/shim/fdt.c -o out/fdt.o
# The image runs wherever LK puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${TC}readelf -r out/shim.o out/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/fdt.o -lgcc -o out/shim.elf
${TC}objcopy -O binary out/shim.elf out/Image
size=$(${TC}nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz $ROOT/dtb/boot_unpacked/dtb > out/Image.gz-dtb
python3 -I $ROOT/tools/mkbootimg-v1.py --like $ROOT/backup/boot.img --ramdisk-addr ${RDADDR:-0x44000000} \
	--kernel out/Image.gz-dtb --ramdisk "$PAYLOAD" -o out/onclite-boot.img
ls -l out/Image out/onclite-boot.img
