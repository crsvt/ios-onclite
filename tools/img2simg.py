#!/usr/bin/env python3
"""Raw image -> Android sparse image (libsparse format v1.0) for fastboot.

  img2simg.py RAW.img OUT.simg [--block 4096] [--max-chunk-mb 64]

Data blocks become RAW chunks (at most --max-chunk-mb each, so fastboot can
split the image into transfers of its max-download-size). All-zero blocks
become FILL chunks with value 0, never DONT_CARE: the flashed partition then
holds exactly the raw image (unused HFS+ space is zeroed, not left with the
previous file system's data).
"""
import argparse, os, struct, sys

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW, CHUNK_FILL = 0xCAC1, 0xCAC2
FILE_HDR, CHUNK_HDR = 28, 12

ap = argparse.ArgumentParser()
ap.add_argument('raw')
ap.add_argument('out')
ap.add_argument('--block', type=int, default=4096)
ap.add_argument('--max-chunk-mb', type=int, default=64)
a = ap.parse_args()

bs = a.block
size = os.path.getsize(a.raw)
if size % bs:
	sys.exit(f'{a.raw}: size {size} is not a multiple of {bs}')
total_blocks = size // bs
max_raw = (a.max_chunk_mb << 20) // bs
zero = bytes(bs)

chunks = 0
with open(a.raw, 'rb') as src, open(a.out, 'wb') as out:
	out.write(bytes(FILE_HDR))		# header filled in at the end
	run_kind, run_blocks, run_data = None, 0, []

	def flush():
		global chunks, run_kind, run_blocks, run_data
		if not run_blocks:
			return
		if run_kind == 'raw':
			data = b''.join(run_data)
			out.write(struct.pack('<HHII', CHUNK_RAW, 0, run_blocks, CHUNK_HDR + len(data)))
			out.write(data)
		else:
			out.write(struct.pack('<HHII', CHUNK_FILL, 0, run_blocks, CHUNK_HDR + 4))
			out.write(struct.pack('<I', 0))
		chunks += 1
		run_kind, run_blocks, run_data = None, 0, []

	done = 0
	while True:
		buf = src.read(bs * 256)
		if not buf:
			break
		for off in range(0, len(buf), bs):
			blk = buf[off:off + bs]
			kind = 'fill' if blk == zero else 'raw'
			if kind != run_kind or (kind == 'raw' and run_blocks >= max_raw):
				flush()
				run_kind = kind
			run_blocks += 1
			if kind == 'raw':
				run_data.append(blk)
		done += len(buf)
		if done % (1 << 30) < len(buf):
			print(f'{done >> 30} GiB', file=sys.stderr, flush=True)
	flush()
	out.seek(0)
	out.write(struct.pack('<IHHHHIIII', SPARSE_MAGIC, 1, 0, FILE_HDR, CHUNK_HDR, bs, total_blocks, chunks, 0))
print(f'{a.out}: {total_blocks} blocks of {bs} in {chunks} chunks, {os.path.getsize(a.out)} bytes')
