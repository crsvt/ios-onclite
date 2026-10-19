#!/bin/bash
# Shim test: stand-in kernel + 80 MB filler as the payload, then the shim
# boot image around it (out/onclite-boot.img). Needs onclite/out/onclite.adt.
set -euo pipefail
cd "$(dirname "$0")"
TC=${TC:-../../../.claude/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-aarch64-none-elf/bin/aarch64-none-elf-}
F="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
O=../out/test
mkdir -p $O
${TC}gcc $F -c stub_start.S -o $O/stub_start.o
${TC}gcc $F -c stub.c -o $O/stub.o
abs=$(${TC}readelf -r $O/stub.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: absolute relocations in stub.o" >&2; exit 1; }
${TC}gcc $F -static -T stub.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	$O/stub_start.o $O/stub.o -lgcc -o $O/stub.elf
${TC}objcopy -O binary $O/stub.elf $O/stub.bin
vms=0x$(${TC}nm $O/stub.elf | awk '$3=="_stack_top"{print $1}')
python3 -I mkstubpayload.py $O/stub.bin $vms ../../out/onclite.adt $O/stub-payload.bin --pad ${PAD_MB:-80}
../build.sh $O/stub-payload.bin
