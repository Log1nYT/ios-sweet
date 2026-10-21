#!/usr/bin/env python3
"""Receive the phone's kernel console (UDP 5140) and append it to a file.

macOS "nc -ukl 5140" connects to the first sender and then ignores every
other source port, so after a reboot the new boot's log is silently
dropped.  This script never connects: it just binds the port and prints
whatever arrives, so it survives reboots.

  venus-udplog.py LOGFILE
"""
import socket
import sys

log = open(sys.argv[1], 'ab', buffering=0) if len(sys.argv) > 1 else None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', 5140))
print('listening on udp/5140', flush=True)
while True:
    try:
        data, addr = s.recvfrom(65536)
    except Exception as e:
        print('recv error:', e, flush=True)
        continue
    try:
        if log is not None:
            log.write(data)
        else:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
    except Exception as e:
        print('write error:', e, flush=True)
