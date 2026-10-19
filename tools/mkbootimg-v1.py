#!/usr/bin/env python3
"""Pack an Android boot image header v1 the way LK (aboot) on onclite expects it.

Usage: mkbootimg-v1.py --like STOCK_BOOT.img --kernel K --ramdisk R -o OUT [--cmdline S]
Load addresses, page size, os_version, name and cmdline are copied from the
stock image, so only the payload differs. The kernel is an Image.gz with the
DTB appended (LK finds it after the gzip stream).
"""
import argparse, hashlib, struct

p = argparse.ArgumentParser()
p.add_argument('--like', required=True)
p.add_argument('--kernel', required=True)
p.add_argument('--ramdisk', required=True)
p.add_argument('--cmdline')
p.add_argument('--ramdisk-addr', type=lambda v: int(v, 0),
	       help='load address of the ramdisk (default: the stock one)')
p.add_argument('-o', '--out', required=True)
a = p.parse_args()

stock = open(a.like, 'rb').read(4096)
assert stock[:8] == b'ANDROID!'
(_, kaddr, _, raddr, _, saddr, taddr, page, ver, osv) = struct.unpack_from('<10I', stock, 8)
assert ver == 1, f'stock header version {ver}, expected 1'
if a.ramdisk_addr is not None:
	raddr = a.ramdisk_addr
name = stock[48:64]
cmd = a.cmdline.encode() if a.cmdline is not None else stock[64:64 + 512].split(b'\0')[0]
extra = b''
if len(cmd) > 512:
	cmd, extra = cmd[:512], cmd[512:]
assert len(extra) < 1024

kernel = open(a.kernel, 'rb').read()
ramdisk = open(a.ramdisk, 'rb').read()
sha = hashlib.sha1()
for blob in (kernel, ramdisk, b'', b''):	# kernel, ramdisk, second, recovery_dtbo
	sha.update(blob)
	sha.update(struct.pack('<I', len(blob)))

hdr = bytearray(page)
struct.pack_into('<8s10I', hdr, 0, b'ANDROID!', len(kernel), kaddr, len(ramdisk), raddr,
		 0, saddr, taddr, page, 1, osv)
hdr[48:64] = name
hdr[64:64 + len(cmd)] = cmd
hdr[576:576 + 20] = sha.digest()
hdr[608:608 + len(extra)] = extra
struct.pack_into('<IQI', hdr, 1632, 0, 0, 1648)

pad = lambda b: b + b'\0' * (-len(b) % page)
with open(a.out, 'wb') as f:
	f.write(hdr + pad(kernel) + pad(ramdisk))
print(f'{a.out}: kernel {len(kernel)} @ {kaddr:#x}, ramdisk {len(ramdisk)} @ {raddr:#x}, '
      f'tags {taddr:#x}, page {page}')
