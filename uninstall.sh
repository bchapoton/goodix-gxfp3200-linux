#!/usr/bin/env bash
# Uninstalls the goodixmilan driver: the libfprint installed under $PREFIX, the
# udev rule, and the fprintd drop-in. fprintd/libpam-fprintd and the distro's
# own libfprint are NOT touched.
#
# Enrolled fingerprints are kept by default; pass --purge to delete them too.
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
PREFIX="${PREFIX:-/usr/local}"
PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1

log()  { printf '\033[1;32m[+]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
[ "$(id -u)" -ne 0 ] || { echo "Do not run as root (sudo is used when needed)."; exit 1; }

log "Stopping fprintd..."
sudo systemctl stop fprintd.service 2>/dev/null || true

log "Removing the udev rule..."
sudo rm -f /etc/udev/rules.d/70-goodixmilan-spidev.rules
sudo udevadm control --reload || true

log "Removing the fprintd drop-in..."
sudo rm -f /etc/systemd/system/fprintd.service.d/10-goodixmilan-gpio.conf
sudo rmdir --ignore-fail-on-non-empty /etc/systemd/system/fprintd.service.d 2>/dev/null || true
sudo systemctl daemon-reload || true

log "Removing the libfprint installed in $PREFIX..."
# Everything `ninja install` puts there, including the arch-qualified libdir.
sudo rm -f  "$PREFIX"/lib/libfprint-2.so*        "$PREFIX"/lib/*/libfprint-2.so*
sudo rm -f  "$PREFIX"/lib/pkgconfig/libfprint-2.pc "$PREFIX"/lib/*/pkgconfig/libfprint-2.pc
sudo rm -rf "$PREFIX"/include/libfprint-2
sudo rm -f  "$PREFIX"/share/metainfo/org.freedesktop.libfprint.metainfo.xml
sudo rm -rf "$PREFIX"/lib/udev/rules.d/70-libfprint-2.rules
sudo rm -rf "$PREFIX"/share/doc/libfprint-2
# Prune the directories we may have created, only if now empty.
for d in "$PREFIX"/lib/udev/rules.d "$PREFIX"/lib/udev "$PREFIX"/share/metainfo \
         "$PREFIX"/lib/*/pkgconfig; do
  sudo rmdir --ignore-fail-on-non-empty "$d" 2>/dev/null || true
done
sudo ldconfig

# Unbind spidev so the sensor is left as the kernel found it.
DEV=$(ls -d /sys/bus/spi/devices/*GXFP3200* 2>/dev/null | head -1 || true)
if [ -n "$DEV" ] && [ -e "$DEV/driver" ]; then
  log "Unbinding spidev from $(basename "$DEV")..."
  sudo sh -c "echo $(basename "$DEV") > /sys/bus/spi/drivers/spidev/unbind" 2>/dev/null || true
  sudo sh -c "echo > $DEV/driver_override" 2>/dev/null || true
fi

if [ "$PURGE" -eq 1 ]; then
  log "Deleting enrolled fingerprints (--purge)..."
  sudo rm -rf /var/lib/fprint/.goodixmilan-adapt
  # Layout is /var/lib/fprint/<user>/<driver>/<finger>/<id>, so the driver
  # directory sits at depth 2.
  sudo find /var/lib/fprint -mindepth 2 -maxdepth 2 -type d -name goodixmilan \
       -exec rm -rf {} + 2>/dev/null || true
fi

log "Checking the removal..."
if /sbin/ldconfig -p | grep -q "libfprint-2.so.2 (libc6,x86-64) => $PREFIX/"; then
  warn "  $PREFIX still provides libfprint — something was left behind."
else
  log "  libfprint: $PREFIX entry gone"
fi
ls /dev/spidev* >/dev/null 2>&1 && warn "  /dev/spidev* still present" \
                                || log "  spidev: unbound"

log "Removing the build cache..."
rm -rf "${BUILD_DIR:-$HOME/.cache/goodix-gxfp3200}"

if grep -q pam_fprintd /etc/pam.d/common-auth 2>/dev/null; then
  warn "pam_fprintd is still enabled in common-auth (sudo/console login)."
  warn "Run 'sudo pam-auth-update' and untick \"Fingerprint authentication\"."
fi

log "Done."
[ "$PURGE" -eq 1 ] || log "Enrolled fingerprints kept in /var/lib/fprint (use --purge to delete)."
log "To remove the stack too: sudo apt remove fprintd libpam-fprintd"
