#!/bin/bash

PORT=/dev/ttyUSB0
BAUD=115200

sudo screen "$PORT" "$BAUD"
