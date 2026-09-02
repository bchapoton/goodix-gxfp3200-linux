#!/usr/bin/env bash
# Uninstalls the goodixmilan driver (removes the /usr/local libfprint + the udev rule).
# fprintd/libpam-fprintd and the distro's libfprint are NOT touched.
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
PREFIX="${PREFIX:-/usr/local}"
log() { printf '\033[1;32m[+]\033[0m %s\n' "$*"; }
[ "$(id -u)" -ne 0 ] || { echo "Do not run as root (sudo is used when needed)."; exit 1; }

log "Removing the udev rule..."
sudo rm -f /etc/udev/rules.d/70-goodixmilan-spidev.rules
sudo udevadm control --reload || true

log "Removing the libfprint installed in $PREFIX..."
sudo rm -f "$PREFIX"/lib/*/libfprint-2.so* "$PREFIX"/lib/libfprint-2.so* 2>/dev/null || true
sudo rm -rf "$PREFIX"/include/libfprint-2 2>/dev/null || true
sudo rm -f "$PREFIX"/lib/*/pkgconfig/libfprint-2.pc 2>/dev/null || true
sudo ldconfig

log "Done. (To remove everything: sudo apt remove fprintd libpam-fprintd libfprint-2-2)"
log "Note: enrolled fingerprints remain in /var/lib/fprint/; delete them if needed."
