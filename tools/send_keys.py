#!/usr/bin/env python3
"""Type a string into a running QEMU via its monitor socket using `sendkey`.

Usage: send_keys.py <monitor_socket> "<text>"

Text: printable ASCII; \\n = Enter, \\t = Tab, \\b = Backspace, \\e = Esc.
"""
import socket
import sys
import time

CHAR_TO_KEY = {
    ' ': 'spc', '-': 'minus', '=': 'equal',
    '[': 'bracket_left', ']': 'bracket_right',
    '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe',
    ',': 'comma', '.': 'dot', '/': 'slash',
    '\n': 'ret', '\t': 'tab', '\b': 'backspace', '\e': 'esc',
}


def key_for(ch):
    if ch in CHAR_TO_KEY:
        return CHAR_TO_KEY[ch]
    if ch.isupper():
        return 'shift-' + ch.lower()
    return ch


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    mon_path, text = sys.argv[1], sys.argv[2]
    if not text:
        return 0

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(mon_path)
    time.sleep(0.3)
    s.recv(65536)

    for ch in text:
        s.sendall(('sendkey %s\n' % key_for(ch)).encode())
        time.sleep(0.2)
        s.recv(65536)
    s.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
