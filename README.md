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
git clone https://github.com/<you>/goodix-gxfp3200-linux.git
cd goodix-gxfp3200-linux
./install.sh
```
The installer: build dependencies → clone libfprint (pinned commit) + graft in the driver → build and install
libfprint into `/usr/local` (takes precedence over, **without replacing**, the distro's libfprint) → install
the persistent udev rule → install `fprintd` + `libpam-fprintd`.

Then:
1. **Enroll**: *GNOME Settings → Users → Fingerprint*, or `fprintd-enroll`.
2. **Enable** for login/`sudo`: `sudo pam-auth-update` (tick "Fingerprint authentication").

Uninstall: `./uninstall.sh`.

## Troubleshooting

- **"No devices available"**: check that `spidev` is bound — `ls /dev/spidev*`; otherwise
  `sudo udevadm trigger --subsystem-match=spi`. Also confirm our libfprint takes precedence:
  `/sbin/ldconfig -p | grep libfprint-2.so.2` (the `/usr/local/...` entry must be listed first).
- **False rejects** (your own finger sometimes not recognised): re-enroll cleanly (finger flat, several
  angles); you can also tune `GX_MATCH_THRESHOLD` / the number of passes `GX_ENROLL_STAGES` in
  `src/goodixmilan.c` and re-run `./install.sh`.
- **After a distro `libfprint` update**: nothing to do (our version in `/usr/local` stays first; the udev rule
  in `/etc/udev/rules.d/` is not overwritten).

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
