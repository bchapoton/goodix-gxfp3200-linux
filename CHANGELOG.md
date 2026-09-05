# Changelog

## v0.2.0 — 2026-09-05

The first release that survives a full day of normal use: suspend/resume, reboot, and
graphical login by fingerprint.

### Fixed

- **The sensor stopped working after suspend/resume** and stayed dead until the next reboot,
  while `fprintd` kept offering fingerprint authentication. `fprintd.service` ships
  `DeviceAllow=` entries, which switch systemd's device policy to *closed*, so `/dev/gpiochip*`
  was denied (a cgroup filter — being root does not help). The hardware reset was therefore
  never issued, and the failure was swallowed silently. Fixed by a `fprintd.service` drop-in
  (`systemd/10-goodixmilan-gpio.conf`); every reset failure is now logged and fails the open.
- The gpiochip is resolved by its `INT34BB` label instead of a hardcoded `/dev/gpiochip0`,
  whose number is not stable across boots or machines.
- `install.sh` shipped a generated `70-libfprint-2.rules` that has the same name as the distro's
  and is searched first, masking the distro's ELAN rules. Built with `-Dudev_rules=disabled`.
- `g_propagate_error` assertion on every verification retry (a `GError` was reported and then
  reused); a timeout handle could be dropped while still armed.

### Changed

- **Recognition is roughly three times more robust.** The background reference was averaged over
  several frames while the live image — the one keypoints are extracted from — was a single frame,
  so all the sensor noise landed on the image that matters. Averaging it too raises same-press
  repeatability from 30 to 38 keypoints (mean) and the verification score from 20–30 to 45,
  against an unchanged threshold of 15: from a 1.3x margin to a 3x one.
- A verification now takes up to three shots while the finger is still down and keeps the best.
  Previously a press photographed mid-slide failed outright. Measured separation stays clean:
  enrolled finger 21–59, other fingers and edge presses 0–6.
- The adaptive store's learning gate was set above any score the pipeline could reach, so the
  store had never gained a single view; it is now reachable by a confident match. Its voting
  surface is capped, since an unbounded union of views can only raise the false-accept rate.
- Enrolment takes 8 passes instead of 6 — the sensor images only ~10x8 mm.

### Added

- `tools/gx-diag.sh`: interactive check, guided re-enrolment and verification, printing the
  driver's own measurements (descriptor pairs, same-press repeatability) with a legend.
- `install.sh` self-checks its result — libfprint precedence, udev rule accepted by
  `udevadm verify`, sensor actually bound, gpiochip reachable under a sandbox — and exits
  non-zero when a check fails instead of always printing "Done".
- `uninstall.sh` removes the drop-in, the metainfo, the generated udev rules and the build cache,
  unbinds spidev, and takes `--purge` for enrolled prints.
- README: states plainly that this build replaces the distro's libfprint at runtime, separates
  GDM login from `sudo` (`gdm-fingerprint` does not include `common-auth`, so `pam-auth-update`
  is only needed for `sudo`), and documents both failure modes seen here.

### Verified

On ASUSPRO/ExpertBook P5440FA, Debian 13, kernel 6.12, Secure Boot on: installed from a clean
machine with `./install.sh` alone, then reboot — `/dev/spidev0.0` appears with no manual action —
and GDM unlock by fingerprint on the first press.

### Known limitations

- Multi-pose assembly at enrolment does not work: every pose after the first is rejected as
  disjoint, so the template is a set of independent views rather than a stitched one. Matching
  works because the views are compared individually; raising the pass count gains little until
  this is fixed.
- Four impostor samples are too few to move the decision threshold; it stays at 15, although
  genuine scores now sit well above it.

## v0.1.0 — 2026-09-02

First working release. Milan register protocol (`F0`/`F1`) reverse-engineered from the Windows
`gfspi.dll`, 108x88 12-bit capture with background subtraction, SIFT matcher, libfprint driver
wired through fprintd to PAM, one-command install and a persistent udev rule.
