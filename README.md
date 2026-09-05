# goodix-gxfp3200-linux

A **working Linux driver** for the **Goodix GXFP3200** fingerprint reader (the "Milan" family, SPI over
ACPI) — a sensor that had **no Linux support**: no driver and no public protocol.

It plugs into **libfprint → fprintd → PAM**, so it works with the GNOME/KDE graphical enrollment and with
session unlock / `sudo` by fingerprint.

> Tested on **ASUSPRO / ExpertBook P5440FA**, Debian 13 (trixie), kernel 6.12, Secure Boot enabled.

## Is this your sensor?

On Linux, run:
```sh
cat /sys/bus/acpi/devices/GXFP3200:00/status 2>/dev/null && echo "GXFP3200 present"
grep -rl GXFP3200 /sys/bus/spi/devices/*/modalias 2>/dev/null
```
If `GXFP3200` shows up (and the reader is **absent from `lsusb`**), this is the sensor. It ships in several
ASUS models (P5440FA, ZenBook Flip S UX370, …) and machines from other vendors. Neighbouring **Goodix Milan**
sensors (GXFP32xx, GDIX…) use a similar protocol, so this driver can serve as a base (see "Adapting").

## What it does, technically

- Talks to the sensor over **SPI** via `spidev` (the register-based "Milan" protocol, opcodes `F0`/`F1`,
  reverse-engineered from the Windows `gfspi.dll`).
- Captures the 108×88 (12-bit) image, denoises it (background subtraction + row/column detrending), and
  **matches** fingerprints with a **SIFT** matcher (`goodix_sift`) — because NBIS is too weak on so small a
  sensor.
- Ships as a **libfprint driver** (`goodixmilan`), i.e. no kernel module → nothing to sign under Secure Boot.

## Installation (Debian/Ubuntu)

```sh
git clone https://github.com/bchapoton/goodix-gxfp3200-linux.git
cd goodix-gxfp3200-linux
./install.sh
```
The installer: build dependencies → clone libfprint (pinned commit) + graft in the driver → build and install
libfprint into `/usr/local` → install the persistent udev rule → install `fprintd` + `libpam-fprintd` →
install a `fprintd.service` drop-in that grants access to `/dev/gpiochip*` → self-check and report what
actually works.

> **What installing into `/usr/local` really means.** The build contains **this driver only**, and
> `/usr/local/lib/…` comes before `/lib/…` in the loader path, so `fprintd` uses this build **instead of**
> the distro's. If the machine has **another** fingerprint reader, it stops working while this is installed.
> The distro package is left untouched on disk, and `./uninstall.sh` restores it.

> **Why the systemd drop-in?** `fprintd.service` ships `DeviceAllow=char-usb_device / char-spi / char-hidraw`.
> As soon as any `DeviceAllow=` is set, systemd switches the unit to a *closed* device policy, so
> `/dev/gpiochip*` is denied — a cgroup filter, so being root does not help. The sensor needs a pulse on its
> reset line to be put back into a known state; without it the sensor still answers on SPI — fprintd keeps
> offering fingerprint auth — but every capture times out once the sensor has lost its configuration,
> typically after a suspend/resume cycle, with no way out short of a reboot.

Then:
1. **Enroll**: *GNOME Settings → Users → Fingerprint*, or `fprintd-enroll`.
2. **Graphical login (GDM/GNOME)**: nothing more to do. `gdm3` already ships
   `/etc/pam.d/gdm-fingerprint`, which loads `pam_fprintd.so`; GNOME offers the fingerprint tile as soon as
   `fprintd` reports an enrolled device. If the tile is missing, the sensor is not being seen — see
   Troubleshooting, and **do not** touch PAM.
3. **`sudo` / console `login` by fingerprint** (optional, and independent of step 2 — these go through
   `common-auth`, which `gdm-fingerprint` does not include):
   ```sh
   sudo pam-auth-update                        # tick "Fingerprint authentication"
   grep pam_fprintd /etc/pam.d/common-auth     # must now print a line
   ```
   The `fprintd` PAM profile ships `Default: no`, so it stays off until you tick it.

Uninstall: `./uninstall.sh` (add `--purge` to delete enrolled fingerprints too).

## Troubleshooting

- **No fingerprint option at all at the login screen** — the sensor is not bound to `spidev`. Check
  `ls /dev/spidev*`; if there is no node, check that udev accepted the rule:
  `sudo journalctl -b | grep 70-goodixmilan` (a rule split over two lines is reported as
  `Invalid key/value pair, ignoring`, and then never runs). Re-trigger with
  `sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=spi`.
  Also confirm our libfprint takes precedence: `/sbin/ldconfig -p | grep libfprint-2.so.2`
  (the `/usr/local/...` entry must be listed first).
- **The option is offered but authentication always fails, especially after a suspend/resume** — the driver
  could not pulse the reset GPIO. Verify the drop-in is in place:
  `systemctl show fprintd.service -p DeviceAllow | grep gpiochip`. If it is missing, reinstall it:
  ```sh
  sudo install -d /etc/systemd/system/fprintd.service.d
  sudo cp systemd/10-goodixmilan-gpio.conf /etc/systemd/system/fprintd.service.d/
  sudo systemctl daemon-reload && sudo systemctl stop fprintd.service
  ```
  With the drop-in installed, the driver now reports the failure instead of silently skipping the reset —
  look for `cannot open a gpiochip` in `journalctl -u fprintd`.
- **False rejects** (your own finger sometimes not recognised): re-enroll cleanly (finger flat, several
  angles). You can also tune, in `src/goodixmilan.c`, the number of passes `GX_ENROLL_STAGES` (8 by default;
  the sensor images only ~10x8 mm, so more passes cover more of the fingertip) and `GX_MATCH_THRESHOLD`,
  then re-run `./install.sh`. Every successful verify also adds its view to the template
  (up to `GX_ADAPT_VIEWS`, in `/var/lib/fprint/.goodixmilan-adapt`), so recognition improves with use.
- **After a distro `libfprint` update**: nothing to do (our version in `/usr/local` stays first; the udev rule
  in `/etc/udev/rules.d/` is not overwritten, and we no longer write into `/usr/lib/udev/rules.d`, which
  belongs to the `libfprint-2-2` package).

## How it was made

Full reverse engineering: ACPI/DSDT diagnosis, reversing the Windows `gfspi.dll` to recover the Milan register
protocol, rebuilding the capture sequence (config, trigger `0xC2`, framebuffer at register `0xAAAA`, 6-bytes →
4-pixels 12-bit decoding), then libfprint integration with a SIFT matcher.

## Credits

- The SIFT matcher `goodix_sift.c` and the FpDevice driver structure are based on
  [Sigfrodr/libfprint-goodixtls](https://github.com/Sigfrodr/libfprint-goodixtls) (GXFP5187).
- The broader Goodix reverse-engineering ecosystem: [goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev).
- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint).

## License

LGPL-2.1-or-later (same as libfprint). See `LICENSE`.

## Disclaimer

Built by reverse engineering for interoperability, without vendor documentation. Provided as-is, without
warranty. A consumer fingerprint sensor is not a high-security device.
