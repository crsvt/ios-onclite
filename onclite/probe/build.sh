#!/bin/bash
# Build the onclite probe and wrap it like the stock kernel: Image.gz + the
# stock DTB appended, in a boot.img v1 that mirrors the stock header.
# Needs backup/boot.img (stock) and dtb/boot_unpacked/dtb (tools/unpack_bootv2.py).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
TC=${TC:-$ROOT/.claude/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-aarch64-none-elf/bin/aarch64-none-elf-}
CC=${TC}gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c probe.c -o out/probe.o
# The image runs wherever LK puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${TC}readelf -r out/probe.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in probe.o" >&2; exit 1; }
$CC $CFLAGS -static -T probe.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/probe.o -lgcc -o out/probe.elf
${TC}objcopy -O binary out/probe.elf out/Image
# Pad to image_size so .bss/stacks exist as zeroed bytes after decompression.
size=$(${TC}nm out/probe.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz $ROOT/dtb/boot_unpacked/dtb > out/Image.gz-dtb
# LK dislikes a zero-size ramdisk: an empty gzip'd newc cpio.
python3 -I -c 'import gzip,sys; e=b"070701"+b"0"*8*11+b"0000000B"+b"0"*8+b"TRAILER!!!\0"; e+=b"\0"*(-len(e)%4); sys.stdout.buffer.write(gzip.compress(e.ljust(512, b"\0"), 9, mtime=0))' > out/ramdisk.gz
python3 -I $ROOT/tools/mkbootimg-v1.py --like $ROOT/backup/boot.img \
	--kernel out/Image.gz-dtb --ramdisk out/ramdisk.gz -o out/probe-boot.img
ls -l out/Image out/probe-boot.img
