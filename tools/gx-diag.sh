#!/usr/bin/env bash
#
# Interactive check for the goodixmilan driver: verifies the install, optionally
# re-enrols a finger, verifies it, and prints the driver's own measurements.
#
# Usage:  ./tools/gx-diag.sh            # check + verify
#         ./tools/gx-diag.sh --enroll   # check + re-enrol (wipes the finger) + verify
#         ./tools/gx-diag.sh --watch    # live finger-detection meter, no fprintd
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -uo pipefail

FINGER="${FINGER:-right-index-finger}"
USER_NAME="${USER_NAME:-$USER}"
MODE="${1:-}"

B=$'\033[1m'; R=$'\033[0m'; G=$'\033[1;32m'; Y=$'\033[1;33m'; E=$'\033[1;31m'
ok()   { printf '%s  ✓%s %s\n' "$G" "$R" "$*"; }
bad()  { printf '%s  ✗%s %s\n' "$E" "$R" "$*"; FAIL=1; }
note() { printf '%s  •%s %s\n' "$Y" "$R" "$*"; }
head1(){ printf '\n%s=== %s ===%s\n' "$B" "$*" "$R"; }
FAIL=0

[ "$(id -u)" -eq 0 ] && { echo "Run as your normal user, not root."; exit 1; }
sudo -v || exit 1

# ---------------------------------------------------------------- 1. install
head1 "1. Installation"

if /sbin/ldconfig -p | grep -q "libfprint-2.so.2 (libc6,x86-64) => /usr/local/"; then
  ok "libfprint: the /usr/local build takes precedence"
else
  bad "libfprint: /usr/local does not take precedence — fprintd will use the distro build"
fi

if [ -e /sys/bus/spi/devices/spi-GXFP3200:00/driver ]; then
  ok "spidev: bound ($(ls /dev/spidev* 2>/dev/null | tr '\n' ' '))"
else
  bad "spidev: the sensor is NOT bound — no /dev/spidev node, fprintd will see no device"
  note "try: sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=spi --settle"
fi

if udevadm verify /etc/udev/rules.d/70-goodixmilan-spidev.rules >/dev/null 2>&1; then
  ok "udev rule: parses cleanly"
else
  bad "udev rule: rejected by udev (or missing) — it will never run"
fi

if systemctl show fprintd.service -p DeviceAllow | grep -q gpiochip; then
  ok "fprintd sandbox: /dev/gpiochip* allowed (hardware reset can happen)"
else
  bad "fprintd sandbox: gpiochip DENIED — the reset never fires, the sensor dies after a suspend"
  note "fix: sudo cp systemd/10-goodixmilan-gpio.conf /etc/systemd/system/fprintd.service.d/ && sudo systemctl daemon-reload"
fi

if [ -e /etc/systemd/system/fprintd.service.d/99-debug.conf ]; then
  note "debug drop-in still installed (99-debug.conf) — verbose journal"
fi

DEVS=$(timeout 30 fprintd-list "$USER_NAME" 2>&1)
if grep -q "found 1 devices\|found [1-9] devices" <<<"$DEVS"; then
  ok "fprintd: sensor detected"
  grep -E "^ - #" <<<"$DEVS" | sed 's/^/      /'
else
  bad "fprintd: no device"
  sed 's/^/      /' <<<"$DEVS"
fi

[ "$FAIL" -ne 0 ] && printf '\n%sSome checks failed — fix those before testing the finger.%s\n' "$Y" "$R"

# ------------------------------------------------------- 2. live detection
if [ "$MODE" = "--watch" ]; then
  head1 "2. Live finger meter (Ctrl-C to stop)"
  echo "Put your finger on and off the sensor; std must climb well above the threshold."
  sudo -n systemctl stop fprintd.service 2>/dev/null
  ( timeout 60 fprintd-verify "$USER_NAME" >/dev/null 2>&1 & )
  sleep 2
  sudo journalctl -u fprintd -f -o cat 2>/dev/null | grep --line-buffered -E "detect:|Finger present"
  exit 0
fi

# ------------------------------------------------------------- 3. enrolment
if [ "$MODE" = "--enroll" ]; then
  head1 "2. Re-enrolment"
  printf 'This DELETES the enrolled %s of user %s. Continue? [y/N] ' "$FINGER" "$USER_NAME"
  read -r a; [ "$a" = "y" ] || [ "$a" = "Y" ] || { echo "Aborted."; exit 0; }
  sudo rm -rf "/var/lib/fprint/$USER_NAME/goodixmilan" /var/lib/fprint/.goodixmilan-adapt
  sudo systemctl stop fprintd.service 2>/dev/null

  cat <<'MSG'

  How to place your finger — this matters more than anything else here:
    - press flat, covering as much of the sensor as you can;
    - shift slightly between passes: centre, then up, down, left, right,
      then the tip and each side of the fingertip;
    - hold still until the pass is accepted, and keep the finger dry.

MSG
  printf 'Press Enter to start... '; read -r _
  fprintd-enroll -f "$FINGER" "$USER_NAME"
  echo "(enroll exit code $?)"
fi

# ---------------------------------------------------------------- 4. verify
head1 "3. Verification"
echo "Put the SAME finger on the sensor when asked."
printf 'Press Enter to start... '; read -r _
timeout 60 fprintd-verify -f "$FINGER" "$USER_NAME"
RC=$?
echo "(verify exit code $RC)"

# --------------------------------------------------------- 5. measurements
head1 "4. What the driver measured"
sudo journalctl -u fprintd --no-pager --since "-5 min" -o cat 2>/dev/null \
  | grep -E "chip-id|session ready|detect:|enroll:|verify:|repeatability:|cannot (open|claim|pulse)" \
  | tail -40

cat <<'MSG'

  Reading these lines:
    chip-id       the sensor answers on SPI (all-zero would mean it is mute)
    detect: std   contrast between the live frame and the background. Below the
                  threshold no finger is seen; a real press should be far above.
    verify: N matches / threshold
                  N is how many keypoints of the probe were matched by the
                  stored views. 0 with a good image means the template no longer
                  describes what the sensor produces: re-enrol (--enroll).
    repeatability the score between two captures of the SAME press, taken
                  milliseconds apart with the finger still. This is the ceiling
                  of the whole pipeline: if it is low, the images themselves are
                  not repeatable and no amount of enrolment coverage will help.
    pairs / descriptor pairs
                  candidate matches before the geometric check. Compare with the
                  score that follows: a large gap means geometry is discarding
                  them, a small pairs count means the descriptors never matched.
MSG
