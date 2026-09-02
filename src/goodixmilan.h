/*
 * Goodix Milan SPI fingerprint driver (GXFP3200) for libfprint
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * FpDevice driver for the Goodix "Milan" area sensor (ACPI GXFP3200), as found
 * in the ASUSPRO/ExpertBook P5440FA. Protocol reverse-engineered from the
 * Windows gfspi.dll (see the README):
 *   - register protocol, opcodes F0 (write / set-address) and F1 (read);
 *   - 108x88 image, 12-bit samples packed six bytes for four pixels;
 *   - framebuffer at register 0xAAAA, no TLS.
 * Matching reuses the local-descriptor (SIFT) matcher in goodix_sift.c rather
 * than NBIS, which does not work on so small a sensor.
 */
#pragma once

#include "drivers_api.h"

/* Sensor geometry (dimension table in gfspi.dll, entry for FPVD==0x22). */
#define GOODIX_IMG_WIDTH   108
#define GOODIX_IMG_HEIGHT  88
#define GOODIX_IMG_PIXELS  (GOODIX_IMG_WIDTH * GOODIX_IMG_HEIGHT)

/* Raw framebuffer read length (4 + 108*88*1.5); last 4 bytes are a trailer. */
#define GXMILAN_FRAMELEN   14260
#define GXMILAN_FB_REG     0xAAAA

/* Reset GPIO: line 271 of the INT34BB gpiochip, from the ACPI _CRS of FPRT. */
#define GXMILAN_RESET_LINE 271
