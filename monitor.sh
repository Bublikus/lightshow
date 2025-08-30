#!/bin/bash

# Optional: auto-detect directory of script
cd "$(dirname "$0")"

# Upload with custom baud rate and serial port
pio device monitor