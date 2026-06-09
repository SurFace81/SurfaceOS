#!/bin/bash

PORT=/dev/ttyUSB0
BAUD=115200

# sudo lsof /dev/ttyUSB0

sudo stty -F "$PORT" "$BAUD" raw -echo
exec sudo picocom -b "$BAUD" --imap lfcrlf "$PORT"
