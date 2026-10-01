#!/bin/sh
set -e

if [ "$(id -u)" -ne 0 ]; then
  echo "Error: please run as root: sudo ./install.sh" >&2
  exit 1
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="HP_LaserJet_Professional_M1136"
FILTER="/Library/Printers/rastertoxqx"
PPD="$DIR/HP_LaserJet_Professional_M1136.ppd"

echo "Building rastertoxqx filter..."
clang -O2 -Wall "$DIR/rastertoxqx.c" "$DIR/jbig/jbig.c" "$DIR/jbig/jbig_ar.c" -I"$DIR" -I"$DIR/jbig" -lcups -lpthread -o "$FILTER"
chown root:wheel "$FILTER"
chmod 755 "$FILTER"

echo "Detecting printer..."
URI=$(/usr/libexec/cups/backend/usb 2>/dev/null | awk '/M1136/ {print $2; exit}')
if [ -z "$URI" ]; then
  URI="usb://Hewlett-Packard/HP%20LaserJet%20Professional%20M1136%20MFP?serial=000000000QHB0HHLPR1a"
fi

echo "Configuring CUPS queue ($NAME)..."
lpadmin -x M1136 2>/dev/null || true
lpadmin -x M1136_Test 2>/dev/null || true
lpadmin -p "$NAME" -E -v "$URI" -P "$PPD" -D "HP LaserJet Professional M1136 MFP" -L "USB"
cupsenable "$NAME" 2>/dev/null || true
cupsaccept "$NAME" 2>/dev/null || true
lpoptions -d "$NAME" 2>/dev/null || true

echo "Installed successfully. Printer is ready to use."
