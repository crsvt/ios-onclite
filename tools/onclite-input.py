#!/usr/bin/env python3
"""Drive the iOS screen on the Redmi 7 from the Mac over the USB network.

  tools/onclite-input.py tap X Y
  tools/onclite-input.py swipe X0 Y0 X1 Y1 [MS]
  tools/onclite-input.py home                    (swipe up from the bottom edge)
  tools/onclite-input.py key power|up|down [press|down|up]
  tools/onclite-input.py reboot fastboot|recovery|normal

Coordinates are iOS canvas pixels (1125x2436, the screenshot size). Needs a
kernel with the remote input responder (sweet_usb.c, UDP 192.168.7.2:5141).
"""
import socket, struct, sys

PHONE = ('192.168.7.2', 5141)
KEYS = {'power': 0, 'up': 1, 'down': 2}


def send(payload):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(payload, PHONE)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], [a for a in argv[1:]]
    if cmd == 'tap' and len(args) == 2:
        send(b'TWAP' + struct.pack('<II', int(args[0]), int(args[1])))
    elif cmd == 'swipe' and len(args) in (4, 5):
        ms = int(args[4]) if len(args) == 5 else 300
        send(b'SWPE' + struct.pack('<IIIII', *map(int, args[:4]), ms))
    elif cmd == 'home':
        send(b'SWPE' + struct.pack('<IIIII', 562, 2430, 562, 1500, 300))
    elif cmd == 'key' and args and args[0] in KEYS:
        how = args[1] if len(args) > 1 else 'press'
        k = KEYS[args[0]]
        if how in ('press', 'down'):
            send(b'KEY ' + struct.pack('<II', k, 1))
        if how in ('press', 'up'):
            if how == 'press':
                import time
                time.sleep(0.15)
            send(b'KEY ' + struct.pack('<II', k, 0))
    elif cmd == 'reboot' and args and args[0] in ('normal', 'recovery', 'fastboot'):
        mode = {'normal': 0, 'recovery': 1, 'fastboot': 2}[args[0]]
        send(b'BOOT' + b'CNIO' + struct.pack('<I', mode))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
