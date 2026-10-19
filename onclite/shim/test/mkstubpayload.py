#!/usr/bin/env python3
"""SWEETXNU payload (tools/mkpayload.py layout) around the shim test stub.

  mkstubpayload.py STUB.bin STUB_VMSIZE ADT OUT [--pad MB]

Segment 0 is the stub at virt_base + 0x4000 (entry), then a --pad MB filler
segment so the LK ramdisk is as large as a real kernel + RAM-disk root.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../tools'))
import mkpayload  # noqa: E402

VIRT_BASE = 0xfffffff006000000	# 32MB aligned, like mkpayload's virt_base
PAGE = 0x4000

ap = argparse.ArgumentParser()
ap.add_argument('stub'); ap.add_argument('vmsize', type=lambda v: int(v, 0))
ap.add_argument('adt'); ap.add_argument('out')
ap.add_argument('--pad', type=int, default=80)
a = ap.parse_args()

stub = open(a.stub, 'rb').read()
adt_blob = open(a.adt, 'rb').read()
phys_base, mem_size = mkpayload.adt_dram(adt_blob)
entry = VIRT_BASE + PAGE
pad_va = VIRT_BASE + 0x1000000
pad = bytes(range(256)) * (a.pad << 12)
pieces = [(entry, (a.vmsize + PAGE - 1) & ~(PAGE - 1), stub), (pad_va, len(pad), pad)]
kernel_end = (pad_va + len(pad) + PAGE - 1) & ~(PAGE - 1)

out = bytearray(mkpayload.SEG_TABLE + 32 * len(pieces))
table = []
for va, size, data in pieces:
	off = len(out)
	out += data
	out += b'\0' * (-len(out) & 0xf)
	table.append((va, size, off, len(data)))
adt_off = len(out)
out += adt_blob
out += b'\0' * (-len(out) & 0xf)
struct.pack_into('<8sII', out, 0, b'SWEETXNU', 1, len(pieces))
struct.pack_into('<7Q', out, 0x10, VIRT_BASE, entry, adt_off, len(adt_blob), kernel_end, phys_base, mem_size)
cl = b'shim-test cpus=4 rd=md0'
out[mkpayload.CMDLINE:mkpayload.CMDLINE + len(cl)] = cl
for i, t in enumerate(table):
	struct.pack_into('<4Q', out, mkpayload.SEG_TABLE + 32 * i, *t)
open(a.out, 'wb').write(out)
print(f'{a.out}: {len(out)} bytes, entry pa {entry - VIRT_BASE + phys_base:#x}, phys {phys_base:#x}+{mem_size:#x}')
