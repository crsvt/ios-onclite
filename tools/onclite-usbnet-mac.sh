#!/bin/sh
# macOS host side of the phone's USB Ethernet (kernel boot-arg sweetusb=2).
#   tools/onclite-usbnet-mac.sh [LOGFILE]     (default logs/live-onclite.txt)
# Waits for the phone's CDC-ECM interface (macOS drives CDC-ECM natively; the
# interface carries the host MAC 02:00:5e:77:ee:01 from the phone's descriptor),
# gives it 192.168.7.1/24 (phone = 192.168.7.2), then prints the kernel console
# the phone streams to UDP 5140 and appends it to LOGFILE. Ctrl-C stops.
set -eu
mac=02:00:5e:77:ee:01
log=${1:-logs/live-onclite.txt}
mkdir -p "$(dirname "$log")"

find_if() {
	for i in $(ifconfig -l); do
		ifconfig "$i" 2>/dev/null | grep -qi "ether $mac" && { echo "$i"; return 0; }
	done
	return 1
}

echo "waiting for the phone's USB Ethernet ($mac)..." >&2
until ifname=$(find_if); do sleep 1; done
echo "phone interface: $ifname" >&2

until ifconfig "$ifname" | grep -q "inet 192.168.7.1 "; do
	cmd="ifconfig $ifname inet 192.168.7.1 netmask 255.255.255.0 alias up"
	if [ "$(id -u)" = 0 ]; then
		$cmd
	elif sudo -n true 2>/dev/null; then
		sudo $cmd
	else
		echo "needs root; run this in Claude Code (or a terminal), the script keeps waiting:" >&2
		echo "  ! sudo $cmd" >&2
		sleep 5
	fi
done
echo "$ifname: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140 -> $log" >&2

exec python3 -u -c '
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 5140))
with open(sys.argv[1], "ab", buffering=0) as f:
    while True:
        data, _ = s.recvfrom(65536)
        f.write(data)
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
' "$log"
