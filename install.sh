#!/usr/bin/env bash
#
# Installer for the libfprint "goodixmilan" driver for the Goodix GXFP3200
# fingerprint reader ("Milan" family, SPI over ACPI) — Debian/Ubuntu.
#
# What this script does:
#   1. installs build dependencies;
#   2. clones libfprint at a pinned commit and grafts in the driver;
#   3. builds and installs libfprint (with the driver) into /usr/local
#      (takes precedence over the distro's libfprint, without replacing it);
#   4. installs the persistent udev rule (binds spidev to the sensor at boot);
#   5. installs fprintd + libpam-fprintd.
#
# Then: enroll via GNOME Settings -> Users -> Fingerprint (or `fprintd-enroll`),
# and enable PAM with `sudo pam-auth-update`.
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

# libfprint commit this driver was tested against (1.94.100). Override with LIBFPRINT_COMMIT=...
LIBFPRINT_REPO="${LIBFPRINT_REPO:-https://gitlab.freedesktop.org/libfprint/libfprint.git}"
LIBFPRINT_COMMIT="${LIBFPRINT_COMMIT:-1f335cd58a1d9436d800d92e7842c3529aabf83a}"
BUILD_DIR="${BUILD_DIR:-$HOME/.cache/goodix-gxfp3200}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"

log()  { printf '\033[1;32m[+]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "Do NOT run this script as root. It will use sudo when needed."
command -v sudo >/dev/null || die "sudo is required."
[ -f /etc/debian_version ] || warn "Tested on Debian/Ubuntu only; adapt elsewhere."

# --- 1. build dependencies ---------------------------------------------------
log "Installing build dependencies..."
sudo apt-get update
sudo apt-get install -y git meson ninja-build build-essential pkgconf \
     libglib2.0-dev libgusb-dev libnss3-dev libpixman-1-dev libgudev-1.0-dev libcairo2-dev

# --- 2. clone + graft the driver --------------------------------------------
mkdir -p "$BUILD_DIR"
LF="$BUILD_DIR/libfprint"
if [ ! -d "$LF/.git" ]; then
  log "Cloning libfprint..."
  git clone "$LIBFPRINT_REPO" "$LF"
fi
log "Checking out pinned commit $LIBFPRINT_COMMIT..."
git -C "$LF" fetch --depth 1 origin "$LIBFPRINT_COMMIT" 2>/dev/null || git -C "$LF" fetch origin
git -C "$LF" checkout -q "$LIBFPRINT_COMMIT"

log "Copying driver sources..."
cp "$HERE"/src/goodixmilan.c "$HERE"/src/goodixmilan.h \
   "$HERE"/src/goodix_sift.c "$HERE"/src/goodix_sift.h "$LF/libfprint/drivers/"

log "Wiring into the build system (meson)..."
python3 - "$LF" <<'PY'
import sys, pathlib
lf = pathlib.Path(sys.argv[1])
# a) driver_sources in libfprint/meson.build
p = lf / "libfprint" / "meson.build"; s = p.read_text()
if "'goodixmilan'" not in s:
    anchor = "    'elanspi' : files('drivers/elanspi.c'),"
    if anchor not in s:
        sys.exit("Unexpected meson layout (driver_sources) - different libfprint commit?")
    s = s.replace(anchor, anchor + "\n    'goodixmilan' : files('drivers/goodixmilan.c', 'drivers/goodix_sift.c'),")
    p.write_text(s)
# b) drivers_info in the root meson.build
p = lf / "meson.build"; s = p.read_text()
if "'goodixmilan':" not in s:
    anchor = "    'elanspi': { 'spi': true, 'helper': ['udev'], 'optional': not have_spi },"
    if anchor not in s:
        sys.exit("Unexpected meson layout (drivers_info) - different libfprint commit?")
    s = s.replace(anchor, anchor + "\n    'goodixmilan': { 'spi': true, 'helper': ['udev', 'pixman'], 'optional': not have_spi },")
    p.write_text(s)
print("meson patched")
PY

# --- 3. build + install ------------------------------------------------------
log "Configuring + building..."
rm -rf "$LF/build"
meson setup "$LF/build" "$LF" -Ddrivers=goodixmilan -Dintrospection=false -Ddoc=false \
      -Dgtk-examples=false -Dinstalled-tests=false -Dudev_hwdb=disabled \
      -Dudev_rules_dir=/usr/lib/udev/rules.d --prefix="$PREFIX"
ninja -C "$LF/build"
log "Installing libfprint into $PREFIX..."
sudo ninja -C "$LF/build" install
sudo ldconfig

# --- 4. persistent udev rule -------------------------------------------------
log "Installing the persistent udev rule..."
sudo cp "$HERE/udev/70-goodixmilan-spidev.rules" /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=spi || true

# --- 5. fprintd + PAM --------------------------------------------------------
log "Installing fprintd + libpam-fprintd..."
sudo apt-get install -y fprintd libpam-fprintd

log "Done."
cat <<'MSG'

Check that our libfprint takes precedence:
    /sbin/ldconfig -p | grep libfprint-2.so.2      # /usr/local/... must appear first

Enroll a fingerprint:
    - GNOME Settings -> Users -> Fingerprint, OR
    - fprintd-enroll

Enable fingerprint for login/sudo:
    sudo pam-auth-update      # tick "Fingerprint authentication"

Uninstall: ./uninstall.sh
MSG
