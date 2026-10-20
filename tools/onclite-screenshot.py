#!/usr/bin/env python3
"""Screenshot of the iOS screen on the Redmi 7 over the USB network.

  tools/onclite-screenshot.py [OUTDIR]        (default SS/)

Needs a kernel with the screenshot responder (sweet_usb.c, sweetusb=2) and the
Mac side of the USB network up (tools/onclite-usbnet-mac.sh: 192.168.7.1).
Asks 192.168.7.2:5141 for the 1125x2436 BGRA canvas, re-requests lost chunks
and writes OUTDIR/ss-YYYYmmdd-HHMMSS.png (no third-party modules).
"""
import os, socket, struct, sys, time, zlib

PHONE = ('192.168.7.2', 5141)
W, H = 1125, 2436
TOTAL = W * H * 4
CHUNK = 1384


def receive(sock, buf, have, frame, deadline):
    while time.time() < deadline:
        try:
            pkt, _ = sock.recvfrom(2048)
        except socket.timeout:
            break
        if len(pkt) < 16:
            continue
        magic, fr, off, total = struct.unpack('<IIII', pkt[:16])
        if magic != 0x544f4853 or total != TOTAL or (frame[0] is not None and fr != frame[0]):
            continue
        frame[0] = fr
        data = pkt[16:]
        buf[off:off + len(data)] = data
        have.add(off)
        deadline = time.time() + 0.5     # keep going while chunks arrive


def png(path, bgra):
    raw = bytearray()
    for y in range(H):
        row = bgra[y * W * 4:(y + 1) * W * 4]
        rgb = bytearray(W * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        raw += b'\0' + rgb
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 6)))
        f.write(chunk(b'IEND', b''))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'SS'
    os.makedirs(outdir, exist_ok=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 << 20)
    sock.bind(('192.168.7.1', 0))
    sock.settimeout(0.5)
    buf = bytearray(TOTAL)
    have, frame = set(), [None]
    want = set(range(0, TOTAL, CHUNK))
    # Ask in blocks: one 10.5 MiB burst overruns the USB link and the host
    # socket (only ~25 % arrived). Block 0 starts a new frame on the phone.
    BLOCK = 128 * CHUNK
    for start in range(0, TOTAL, BLOCK):
        end = min(start + BLOCK, TOTAL)
        for _ in range(4):
            sock.sendto(b'SHOT' + struct.pack('<II', start, end), PHONE)
            receive(sock, buf, have, frame, time.time() + 1.5)
            if all(o in have for o in range(start, end, CHUNK)):
                break
    for _ in range(20):
        missing = sorted(want - have)
        if not missing:
            break
        # ask again for each missing run of chunks
        runs, start, prev = [], missing[0], missing[0]
        for off in missing[1:]:
            if off != prev + CHUNK:
                runs.append((start, prev + CHUNK))
                start = off
            prev = off
        runs.append((start, prev + CHUNK))
        for a, b in runs[:64]:
            sock.sendto(b'SHOT' + struct.pack('<II', a, min(b, TOTAL)), PHONE)
        receive(sock, buf, have, frame, time.time() + 3)
    missing = len(want - have)
    if frame[0] is None:
        sys.exit('no answer from 192.168.7.2:5141 (kernel with the responder? USB network up?)')
    path = os.path.join(outdir, time.strftime('ss-%Y%m%d-%H%M%S.png'))
    png(path, buf)
    print(f'{path}: frame {frame[0]}, {len(have)}/{len(want)} chunks' + (f' ({missing} missing, black)' if missing else ''))


if __name__ == '__main__':
    main()
