#!/bin/bash
# Build an onclite phone image from an ONCLITE kernel (CI artifact xnu-kernel):
#   onclite/mkboot.sh KERNEL NAME [RAMDISK] [-- EXTRA BOOT-ARGS...]
# -> onclite/out/boot-NAME.img (fastboot boot only). With RAMDISK (for example
# onclite/out/sweet-init, served by mockfs as /sbin/launchd) the kernel roots
# on md0. Console output goes to ramoops; read it from the next Linux boot:
#   adb root; adb exec-out cat /sys/fs/pstore/console-ramoops-0
set -euo pipefail
cd "$(dirname "$0")/.."
K=${1:?kernel}; NAME=${2:?name}; shift 2
RD=
if [ $# -gt 0 ] && [ "$1" != "--" ]; then RD=$1; shift; fi
[ "${1:-}" = "--" ] && shift
# debug=0x8 (kprintf, no DB_NMI): a panic restarts through PS_HOLD back to Linux.
ARGS="debug=0x8 serial=3 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 $*"
# One cpus= only (default 1): xnu takes the first match.
case " $* " in *" cpus="*) ;; *) ARGS="$ARGS cpus=1" ;; esac
[ -n "$RD" ] && ARGS="$ARGS rd=md0"
O=onclite/out
python3 -I tools/mkadt.py $O/onclite.adt --onclite
python3 -I tools/mkpayload.py "$K" $O/onclite.adt $O/payload-$NAME.bin --cmdline "$ARGS" ${RD:+--ramdisk "$RD"}
onclite/shim/build.sh $O/payload-$NAME.bin >/dev/null
cp onclite/shim/out/onclite-boot.img $O/boot-$NAME.img
# The ramdisk inside must be the payload, byte for byte.
python3 -I tools/unpack_bootv2.py $O/boot-$NAME.img $O/check-$NAME >/dev/null
cmp $O/check-$NAME/ramdisk $O/payload-$NAME.bin
echo "$O/boot-$NAME.img: $(stat -f%z $O/boot-$NAME.img) bytes, boot-args: $ARGS"
