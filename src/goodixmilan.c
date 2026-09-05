/*
 * Goodix Milan SPI fingerprint driver (GXFP3200) for libfprint.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Sensor I/O reverse-engineered from the Windows gfspi.dll (see the README).
 * Matching and the enrol/verify state machine are adapted from the
 * Sigfrodr/libfprint-goodixtls GXFP5187 driver (same Milan family), with its
 * TLS-PSK transport and native FDT finger detection replaced by this sensor's
 * plain F0/F1 register protocol and a contrast-based finger detection.
 */

#define FP_COMPONENT "goodixmilan"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>

#include "drivers_api.h"
#include "fpi-spi-transfer.h"
#include "goodixmilan.h"
#include "goodix_sift.h"

/* Frames averaged for the background reference. */
#define GOODIX_BG_FRAMES   3
/* Contrast (std of the preprocessed image) below which a capture has no finger.
 * The keypoint detector returns maxima even on noise, so contrast — not point
 * count — is what separates a real press (~150+) from an empty frame (~5). */
#define GOODIX_FINGER_STD  120.0
/* Finger detection threshold: std of (background - live frame), raw samples.
 * Measured on this sensor: idle ~13, finger ~150. */
#define GX_DETECT_STD      60.0

struct _FpiDeviceGoodixMilan
{
  FpDevice  parent;

  int       spi_fd;
  guint16  *bg_frame;      /* averaged background, NULL until calibrated */
  guint     poll_id;       /* finger-detection timeout source */
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixMilan, fpi_device_goodixmilan, FPI,
                      DEVICE_GOODIXMILAN, FpDevice)
G_DEFINE_TYPE (FpiDeviceGoodixMilan, fpi_device_goodixmilan, FP_TYPE_DEVICE)

static const FpIdEntry goodixmilan_id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_id = "GXFP3200" },
  { .udev_types = 0 }
};

/* Register blobs reverse-engineered from Milan_DlCfg in gfspi.dll. */
typedef struct { guint16 reg, val; } GxReg;

static const GxReg GX_CFG_NORMAL[] = {
  {0x0204, 0x0000}, {0x00ba, 0x8001}, {0x00ca, 0x0000}, {0x0084, 0xb3c0},
  {0x0086, 0xc4bb}, {0x0088, 0xbaba}, {0x008a, 0xb2b2}, {0x008c, 0xaaaa},
  {0x008e, 0xc1c1}, {0x0090, 0xbbbb}, {0x0092, 0xb1b1}, {0x0094, 0xa800},
  {0x0096, 0xb600}, {0x0098, 0xbf00}, {0x009a, 0xba00}, {0x0050, 0x0501},
  {0x00d0, 0x0000}, {0x0070, 0x0000}, {0x0072, 0x5678}, {0x0074, 0x1234},
  {0x0026, 0x1200}, {0x0020, 0x4010}, {0x0012, 0x0403},
};
static const GxReg GX_CFG_CAPTURE[] = {
  {0x005c, 0x0080}, {0x012a, 0x0008}, {0x0052, 0x0008}, {0x0054, 0x0100},
};

/* ------------------------------------------------------------------ */
/*  Milan SPI transport: opcode F0 write / set-address, F1 read        */
/* ------------------------------------------------------------------ */

static gboolean
gxm_write_reg (FpiDeviceGoodixMilan *self, guint16 reg, guint16 val)
{
  g_autoptr(FpiSpiTransfer) t = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (t, 7);
  t->buffer_wr[0] = 0xF0;
  t->buffer_wr[1] = (reg >> 8) & 0xFF;
  t->buffer_wr[2] = reg & 0xFF;
  t->buffer_wr[3] = 0x00;
  t->buffer_wr[4] = 0x01;
  t->buffer_wr[5] = (val >> 8) & 0xFF;
  t->buffer_wr[6] = val & 0xFF;
  return fpi_spi_transfer_submit_sync (t, NULL);
}

static gboolean
gxm_send_cmd (FpiDeviceGoodixMilan *self, guint8 cmd)
{
  g_autoptr(FpiSpiTransfer) t = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (t, 1);
  t->buffer_wr[0] = cmd;
  return fpi_spi_transfer_submit_sync (t, NULL);
}

static gboolean
gxm_set_addr (FpiDeviceGoodixMilan *self, guint16 reg)
{
  g_autoptr(FpiSpiTransfer) t = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (t, 3);
  t->buffer_wr[0] = 0xF0;
  t->buffer_wr[1] = (reg >> 8) & 0xFF;
  t->buffer_wr[2] = reg & 0xFF;
  return fpi_spi_transfer_submit_sync (t, NULL);
}

/* F1 + read n bytes (n <= ~4000 to stay under the spidev buffer size). */
static gboolean
gxm_read_chunk (FpiDeviceGoodixMilan *self, guint8 *out, gsize n)
{
  g_autoptr(FpiSpiTransfer) t = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (t, 1);
  t->buffer_wr[0] = 0xF1;
  fpi_spi_transfer_read (t, n);
  if (!fpi_spi_transfer_submit_sync (t, NULL))
    return FALSE;
  memcpy (out, t->buffer_rd, n);
  return TRUE;
}

static gboolean
gxm_cfg (FpiDeviceGoodixMilan *self, const GxReg *regs, gsize count)
{
  for (gsize i = 0; i < count; i++)
    if (!gxm_write_reg (self, regs[i].reg, regs[i].val))
      return FALSE;
  return TRUE;
}

/* Opens the gpiochip that carries the reset line. The /dev/gpiochipN number is
 * not stable across boots or machines, so match on the controller label and
 * only fall back to gpiochip0. */
static int
gx_open_gpiochip (void)
{
  int n;

  for (n = 0; n < 16; n++)
    {
      struct gpiochip_info info = { 0 };
      g_autofree gchar *path = g_strdup_printf ("/dev/gpiochip%d", n);
      int fd = open (path, O_RDWR | O_CLOEXEC);

      if (fd < 0)
        continue;
      if (ioctl (fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
          g_str_has_prefix (info.label, GXMILAN_GPIO_LABEL) &&
          info.lines > GXMILAN_RESET_LINE)
        {
          fp_dbg ("reset gpiochip: %s (label %s, %u lines)",
                  path, info.label, info.lines);
          return fd;
        }
      close (fd);
    }
  return open ("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
}

/* Hardware reset: a short pulse on GPIO line 271 (ACPI _CRS).
 *
 * Returns FALSE if the pulse could not be issued. That is not cosmetic: without
 * a hardware reset the sensor cannot be brought back once it has lost its
 * configuration (typically across a suspend/resume cycle), and every capture
 * then times out until the next reboot. The usual cause is fprintd.service's
 * DeviceAllow= sandbox, which denies /dev/gpiochip* — see the drop-in shipped
 * in systemd/10-goodixmilan-gpio.conf. */
static gboolean
gx_gpio_reset (FpiDeviceGoodixMilan *self)
{
  struct gpio_v2_line_request req = { 0 };
  struct gpio_v2_line_values val = { 0 };
  int chip;

  (void) self;
  chip = gx_open_gpiochip ();
  if (chip < 0)
    {
      fp_warn ("cannot open a gpiochip for the reset line: %s. Under fprintd "
               "this means the DeviceAllow= sandbox is denying /dev/gpiochip*; "
               "install systemd/10-goodixmilan-gpio.conf. Without a reset the "
               "sensor cannot recover after a suspend/resume cycle.",
               g_strerror (errno));
      return FALSE;
    }
  req.num_lines = 1;
  req.offsets[0] = GXMILAN_RESET_LINE;
  req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
  g_strlcpy (req.consumer, "goodixmilan", sizeof req.consumer);
  if (ioctl (chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0 || req.fd < 0)
    {
      fp_warn ("cannot claim reset GPIO line %d: %s", GXMILAN_RESET_LINE,
               g_strerror (errno));
      close (chip);
      return FALSE;
    }
  val.mask = 1;
  val.bits = 0;                          /* assert reset */
  ioctl (req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &val);
  g_usleep (10000);
  val.bits = 1;                          /* release */
  ioctl (req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &val);
  g_usleep (120000);
  close (req.fd);
  close (chip);
  return TRUE;
}

/* Six bytes carry four interleaved 12-bit pixels (Goodix packing). */
static void
gx_decode_12bit (const guint8 *data, gsize len, guint16 *out, gsize n)
{
  gsize i, o = 0;

  for (i = 0; i + 6 <= len && o + 4 <= n; i += 6)
    {
      const guint8 *c = data + i;
      out[o++] = ((c[0] & 0xf) << 8) | c[1];
      out[o++] = (c[3] << 4) | (c[0] >> 4);
      out[o++] = ((c[5] & 0xf) << 8) | c[2];
      out[o++] = (c[4] << 4) | (c[5] >> 4);
    }
}

/* Sensor init after a reset: wake, chip-id probe, then normal config. */
static gboolean
gx_sensor_init (FpiDeviceGoodixMilan *self)
{
  guint8 id[4];

  if (!gx_gpio_reset (self))
    return FALSE;
  if (!gxm_write_reg (self, 0x0124, 0x0D00))
    return FALSE;
  if (!gxm_write_reg (self, 0x0204, 0x0000))
    return FALSE;
  if (!gxm_send_cmd (self, 0xC0))
    return FALSE;
  g_usleep (10000);
  gxm_set_addr (self, 0x0000);
  gxm_read_chunk (self, id, 4);          /* chip-id, informational */
  fp_dbg ("chip-id: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
  return gxm_cfg (self, GX_CFG_NORMAL, G_N_ELEMENTS (GX_CFG_NORMAL));
}

/* Captures one full image into px[GOODIX_IMG_PIXELS]. */
static gboolean
gx_capture_frame (FpiDeviceGoodixMilan *self, guint16 *px)
{
  g_autofree guint8 *raw = g_malloc (GXMILAN_FRAMELEN);
  gsize off = 0;

  if (!gxm_cfg (self, GX_CFG_CAPTURE, G_N_ELEMENTS (GX_CFG_CAPTURE)))
    return FALSE;
  if (!gxm_send_cmd (self, 0xC2))        /* trigger capture */
    return FALSE;
  if (!gxm_write_reg (self, 0x0054, 0x0110))
    return FALSE;
  g_usleep (50000);

  if (!gxm_set_addr (self, GXMILAN_FB_REG))
    return FALSE;
  while (off < GXMILAN_FRAMELEN)
    {
      gsize n = MIN (4000, GXMILAN_FRAMELEN - off);
      if (!gxm_read_chunk (self, raw + off, n))
        return FALSE;
      off += n;
    }
  gx_decode_12bit (raw, GXMILAN_FRAMELEN, px, GOODIX_IMG_PIXELS);
  return TRUE;
}

static int
gx_cmp_dbl (const void *a, const void *b)
{
  double x = *(const double *) a - *(const double *) b;
  return (x > 0) - (x < 0);
}

/* Averages n frames into a pixel buffer. */
static gboolean
gx_capture_avg (FpiDeviceGoodixMilan *self, int nframes, guint16 *avg)
{
  g_autofree guint32 *acc = g_malloc0 (sizeof (guint32) * GOODIX_IMG_PIXELS);
  guint16 px[GOODIX_IMG_PIXELS];
  int got = 0, f, i;

  for (f = 0; f < nframes; f++)
    {
      if (!gx_capture_frame (self, px))
        continue;
      for (i = 0; i < GOODIX_IMG_PIXELS; i++)
        acc[i] += px[i];
      got++;
    }
  if (!got)
    return FALSE;
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    avg[i] = acc[i] / got;
  return TRUE;
}

/* Is a finger down? Captures a frame and measures the spatial std of the
 * difference to the background — a finger adds texture, a DC drift does not. */
static gboolean
gx_finger_present (FpiDeviceGoodixMilan *self)
{
  guint16 px[GOODIX_IMG_PIXELS];
  double m = 0, v = 0;
  int i;

  if (!self->bg_frame || !gx_capture_frame (self, px))
    return FALSE;
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    m += (double) self->bg_frame[i] - px[i];
  m /= GOODIX_IMG_PIXELS;
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    {
      double d = ((double) self->bg_frame[i] - px[i]) - m;
      v += d * d;
    }
  {
    static int tick = 0;
    double std = sqrt (v / GOODIX_IMG_PIXELS);

    /* One line every ~1 s: the delta statistics that drive detection, plus the
     * raw frame range, so a dead or saturated sensor is told apart from a
     * mis-tuned threshold. */
    if ((tick++ % 10) == 0)
      {
        guint16 lo = 0xffff, hi = 0, blo = 0xffff, bhi = 0;

        for (i = 0; i < GOODIX_IMG_PIXELS; i++)
          {
            lo = MIN (lo, px[i]); hi = MAX (hi, px[i]);
            blo = MIN (blo, self->bg_frame[i]); bhi = MAX (bhi, self->bg_frame[i]);
          }
        fp_dbg ("detect: std=%.1f (threshold %.1f) mean_delta=%.1f "
                 "frame=[%u..%u] bg=[%u..%u]",
                 std, (double) GX_DETECT_STD, m, lo, hi, blo, bhi);
      }
    return std > GX_DETECT_STD;
  }
}

/* Init the sensor and take a fresh background (finger assumed lifted). */
static gboolean
gx_session_start (FpiDeviceGoodixMilan *self)
{
  guint16 warm[GOODIX_IMG_PIXELS];

  if (!gx_sensor_init (self))
    return FALSE;
  gx_capture_frame (self, warm);          /* discard settling frames */
  gx_capture_frame (self, warm);

  if (!self->bg_frame)
    self->bg_frame = g_malloc (sizeof (guint16) * GOODIX_IMG_PIXELS);
  if (!gx_capture_avg (self, GOODIX_BG_FRAMES, self->bg_frame))
    return FALSE;
  fp_info ("session ready (background captured)");
  return TRUE;
}

/* Pre-processing: subtract the background, then remove row and column banding
 * with a median. */
static double *
gx_preprocess (FpiDeviceGoodixMilan *self, const guint16 *px)
{
  const int W = GOODIX_IMG_WIDTH, H = GOODIX_IMG_HEIGHT;
  double *d = g_malloc (sizeof (double) * GOODIX_IMG_PIXELS);
  g_autofree double *buf = g_malloc (sizeof (double) * MAX (W, H));
  int x, y, i;

  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    d[i] = (double) self->bg_frame[i] - px[i];
  for (y = 0; y < H; y++)
    {
      for (x = 0; x < W; x++) buf[x] = d[y * W + x];
      qsort (buf, W, sizeof (double), gx_cmp_dbl);
      { double m = buf[W / 2]; for (x = 0; x < W; x++) d[y * W + x] -= m; }
    }
  for (x = 0; x < W; x++)
    {
      for (y = 0; y < H; y++) buf[y] = d[y * W + x];
      qsort (buf, H, sizeof (double), gx_cmp_dbl);
      { double m = buf[H / 2]; for (y = 0; y < H; y++) d[y * W + x] -= m; }
    }
  return d;
}

/* Dumps captures for offline evaluation when GX_DUMP_DIR exists. */
#define GX_DUMP_DIR "/run/goodixmilan/dump"

static void
gx_dump_capture (FpiDeviceGoodixMilan *self, const guint16 *px)
{
  static int seq = 0;
  g_autofree gchar *p = NULL, *b = NULL;

  if (!g_file_test (GX_DUMP_DIR, G_FILE_TEST_IS_DIR) || !self->bg_frame)
    return;
  do
    {
      g_free (p);
      p = g_strdup_printf ("%s/p%03d.bin", GX_DUMP_DIR, ++seq);
    }
  while (g_file_test (p, G_FILE_TEST_EXISTS) && seq < 9999);

  b = g_strdup_printf ("%s.bg", p);
  if (g_file_set_contents (p, (const gchar *) px,
                           sizeof (guint16) * GOODIX_IMG_PIXELS, NULL) &&
      g_file_set_contents (b, (const gchar *) self->bg_frame,
                           sizeof (guint16) * GOODIX_IMG_PIXELS, NULL))
    fp_info ("capture saved: %s", p);
}

/* Captures one press and extracts its descriptors. */
static GxSiftFeatures *
gx_capture_features (FpiDeviceGoodixMilan *self)
{
  guint16 px[GOODIX_IMG_PIXELS];
  g_autofree double *img = NULL;
  double m = 0, v = 0;
  int i;

  if (!gx_capture_frame (self, px))
    return NULL;
  gx_dump_capture (self, px);
  img = gx_preprocess (self, px);

  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    m += img[i];
  m /= GOODIX_IMG_PIXELS;
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    { double d = img[i] - m; v += d * d; }
  v = sqrt (v / GOODIX_IMG_PIXELS);
  if (v < GOODIX_FINGER_STD / 3.0)
    {
      fp_info ("capture rejected: contrast %.0f (no finger?)", v);
      return NULL;
    }
  return gx_sift_extract (img, GOODIX_IMG_WIDTH, GOODIX_IMG_HEIGHT);
}

/* ------------------------------------------------------------------ */
/*  Template (de)serialisation                                         */
/* ------------------------------------------------------------------ */

static GVariant *
gx_views_to_variant (GPtrArray *views)
{
  GVariantBuilder b;
  guint i;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));
  for (i = 0; i < views->len; i++)
    {
      g_autoptr(GByteArray) ba = gx_sift_serialize (g_ptr_array_index (views, i));
      g_variant_builder_add_value (
        &b, g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, ba->data, ba->len, 1));
    }
  return g_variant_new ("aay", &b);
}

static GPtrArray *
gx_views_from_print (FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  GPtrArray *views = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
  GVariantIter it;
  GVariant *child;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("aay")))
    return views;
  g_variant_iter_init (&it, data);
  while ((child = g_variant_iter_next_value (&it)))
    {
      gsize len = 0;
      const guint8 *raw = g_variant_get_fixed_array (child, &len, 1);
      GxSiftFeatures *f = gx_sift_deserialize (raw, len);
      if (f)
        g_ptr_array_add (views, f);
      g_variant_unref (child);
    }
  return views;
}

/* ------------------------------------------------------------------ */
/*  Adaptive store: widens coverage as the sensor is used              */
/* ------------------------------------------------------------------ */

#define GX_ADAPT_MIN      30
#define GX_ADAPT_MAX_FRAC 0.60
#define GX_ADAPT_VIEWS    20
/* Adaptive views actually consulted (the most recent ones). The score is a
 * monotone union of matched-point bits, so every extra view can only raise it,
 * never lower it: consulting the whole store would let the false-accept rate
 * drift upwards with use. Bound, not calibrated — worth measuring on real data. */
#define GX_ADAPT_USE      8
#define GX_ADAPT_DIR      "/var/lib/fprint/.goodixmilan-adapt"

static gchar *
gx_adapt_path (FpPrint *print)
{
  const gchar *user = print ? fp_print_get_username (print) : NULL;

  if (!user || !*user || strchr (user, '/') || g_str_has_prefix (user, "."))
    return NULL;
  return g_strdup_printf ("%s/%s.%d.views", GX_ADAPT_DIR, user,
                          (int) fp_print_get_finger (print));
}

static GPtrArray *
gx_adapt_load (const gchar *path)
{
  GPtrArray *views = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
  g_autofree gchar *raw = NULL;
  gsize len = 0, off = 0;

  if (!path || !g_file_get_contents (path, &raw, &len, NULL))
    return views;
  while (off + 4 <= len)
    {
      guint32 sz;
      GxSiftFeatures *f;

      memcpy (&sz, raw + off, 4);
      off += 4;
      if (sz > len - off)
        break;
      f = gx_sift_deserialize ((const guint8 *) raw + off, sz);
      if (f)
        g_ptr_array_add (views, f);
      off += sz;
    }
  return views;
}

static gboolean
gx_adapt_append (const gchar *path, const GxSiftFeatures *f)
{
  g_autoptr(GByteArray) blob = NULL;
  guint32 sz;
  gboolean ok;
  FILE *fh;

  if (!path)
    return FALSE;
  blob = gx_sift_serialize (f);
  if (!blob)
    return FALSE;
  if (g_mkdir_with_parents (GX_ADAPT_DIR, 0700) != 0)
    return FALSE;
  fh = fopen (path, "ab");
  if (!fh)
    return FALSE;
  sz = blob->len;
  ok = (fwrite (&sz, 4, 1, fh) == 1 &&
        fwrite (blob->data, 1, blob->len, fh) == blob->len);
  if (fclose (fh) != 0)
    ok = FALSE;
  g_chmod (path, 0600);
  return ok;
}

static void
gx_adapt_clear (FpPrint *print)
{
  g_autofree gchar *path = gx_adapt_path (print);

  if (path && g_unlink (path) == 0)
    fp_info ("adaptive store cleared (%s)", path);
}

static void
gx_adapt_sweep (void)
{
  GDir *d = g_dir_open (GX_ADAPT_DIR, 0, NULL);
  const gchar *name;

  if (!d)
    return;
  while ((name = g_dir_read_name (d)))
    {
      g_autofree gchar *stem = NULL, *user = NULL, *userdir = NULL;
      const gchar *finger;
      gboolean found = FALSE;
      gchar *dot;
      GDir *dd;

      if (!g_str_has_suffix (name, ".views"))
        continue;
      stem = g_strndup (name, strlen (name) - strlen (".views"));
      dot = strrchr (stem, '.');
      if (!dot || !dot[1])
        continue;
      *dot = '\0';
      user = g_strdup (stem);
      finger = dot + 1;
      userdir = g_build_filename ("/var/lib/fprint", user, "goodixmilan", NULL);
      dd = g_dir_open (userdir, 0, NULL);
      if (dd)
        {
          const gchar *devid;
          while (!found && (devid = g_dir_read_name (dd)))
            {
              g_autofree gchar *p = g_build_filename (userdir, devid, finger, NULL);
              found = g_file_test (p, G_FILE_TEST_EXISTS);
            }
          g_dir_close (dd);
        }
      if (!found)
        {
          g_autofree gchar *victim = g_build_filename (GX_ADAPT_DIR, name, NULL);
          if (g_unlink (victim) == 0)
            fp_info ("removed orphaned adaptive store %s", name);
        }
    }
  g_dir_close (d);
}

/* ------------------------------------------------------------------ */
/*  Enrolment / verification via local descriptors                     */
/* ------------------------------------------------------------------ */

#define GX_MATCH_THRESHOLD 15
/* Passes asked of the user at enrolment, reported to fprintd/GNOME through
 * nr_enroll_stages. The sensor images only ~10x8 mm, so a single pass covers a
 * small part of the fingertip; eight passes (x GX_VIEWS_PER_STAGE views each)
 * are needed to cover the edges well enough to keep false rejects low. */
#define GX_ENROLL_STAGES   8
#define GX_VIEWS_PER_STAGE 2

typedef struct
{
  GPtrArray     *views;
  GxSiftFeatures *probe;
  int            best;
  int            tries;
  int            stage;
  int            polls;
  gboolean       verifying;
  GxSiftIsland  *island;
} GxTask;

enum {
  GX_ST_SESSION,
  GX_ST_WAIT_ON,
  GX_ST_CAPTURE,
  GX_ST_WAIT_OFF,
  GX_ST_DONE,
  GX_ST_NUM,
};

#define GX_POLL_MS   100
#define GX_POLL_MAX  120
#define GX_POLL_OFF  100

typedef struct
{
  FpiSsm         *ssm;
  FpDevice       *dev;
  GxSiftFeatures *feat;
  GPtrArray      *extra;
  gboolean        ok;
} GxWork;

static void
gx_work_free (GxWork *w)
{
  g_clear_pointer (&w->feat, gx_sift_free);
  if (w->extra)
    g_ptr_array_free (w->extra, TRUE);
  g_free (w);
}

static void
gx_session_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  GxWork *w = data;

  (void) src; (void) c;
  w->ok = gx_session_start (FPI_DEVICE_GOODIXMILAN (w->dev));
  g_task_return_boolean (task, TRUE);
}

static void
gx_capture_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  GxWork *w = data;
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (w->dev);
  GxTask *t = fpi_ssm_get_data (w->ssm);

  (void) src; (void) c;
  w->feat = gx_capture_features (self);
  w->ok = (w->feat != NULL);

  if (w->ok && w->feat->n >= 8 && !t->verifying)
    {
      int extra;

      w->extra = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
      for (extra = 1; extra < GX_VIEWS_PER_STAGE; extra++)
        {
          GxSiftFeatures *g;

          if (!gx_finger_present (self))
            break;
          g = gx_capture_features (self);
          if (g && g->n >= 8)
            g_ptr_array_add (w->extra, g);
          else
            g_clear_pointer (&g, gx_sift_free);
        }
    }
  g_task_return_boolean (task, TRUE);
}

static void
gx_session_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GxWork *w = g_task_get_task_data (G_TASK (res));

  (void) src; (void) user_data;
  if (!w->ok)
    {
      fpi_ssm_mark_failed (w->ssm, fpi_device_error_new_msg (
        FP_DEVICE_ERROR_PROTO, "sensor initialisation failed"));
      return;
    }
  fpi_ssm_next_state (w->ssm);
}

static void
gx_run_async (FpiSsm *ssm, FpDevice *dev, GTaskThreadFunc fn,
              GAsyncReadyCallback done)
{
  GTask *task = g_task_new (dev, NULL, done, NULL);
  GxWork *w = g_new0 (GxWork, 1);

  w->ssm = ssm;
  w->dev = dev;
  g_task_set_task_data (task, w, (GDestroyNotify) gx_work_free);
  g_task_run_in_thread (task, fn);
  g_object_unref (task);
}

static void
gx_task_free (GxTask *t)
{
  if (!t)
    return;
  if (t->views)
    g_ptr_array_free (t->views, TRUE);
  g_clear_pointer (&t->probe, gx_sift_free);
  g_clear_pointer (&t->island, gx_sift_island_free);
  g_free (t);
}

static gboolean
gx_cancelled (FpDevice *dev, FpiSsm *ssm)
{
  GCancellable *c = fpi_device_get_cancellable (dev);

  if (!c || !g_cancellable_is_cancelled (c))
    return FALSE;
  fp_info ("operation cancelled by the caller");
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
  fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                         "operation cancelled"));
  return TRUE;
}

static gboolean
gx_poll_on (gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDevice *dev = fpi_ssm_get_device (ssm);
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (dev);
  GxTask *t = fpi_ssm_get_data (ssm);

  if (gx_cancelled (dev, ssm))
    { self->poll_id = 0; return G_SOURCE_REMOVE; }

  if (gx_finger_present (self))
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED |
                                            FP_FINGER_STATUS_PRESENT);
      self->poll_id = 0;
      fpi_ssm_jump_to_state (ssm, GX_ST_CAPTURE);
      return G_SOURCE_REMOVE;
    }
  if (++t->polls > GX_POLL_MAX)
    {
      self->poll_id = 0;
      fpi_ssm_mark_failed (ssm, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
gx_poll_off (gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDevice *dev = fpi_ssm_get_device (ssm);
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (dev);
  GxTask *t = fpi_ssm_get_data (ssm);

  if (gx_cancelled (dev, ssm))
    { self->poll_id = 0; return G_SOURCE_REMOVE; }

  if (!gx_finger_present (self) || ++t->polls > GX_POLL_OFF)
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      /* Clear before jumping: the jump can rearm a timeout, and assigning 0
       * afterwards would drop the fresh handle on the floor. */
      self->poll_id = 0;
      if (t->verifying || t->stage >= GX_ENROLL_STAGES)
        fpi_ssm_jump_to_state (ssm, GX_ST_DONE);
      else
        fpi_ssm_jump_to_state (ssm, GX_ST_WAIT_ON);
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
gx_capture_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GxWork *w = g_task_get_task_data (G_TASK (res));
  FpiSsm *ssm = w->ssm;
  FpDevice *dev = w->dev;
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (dev);
  GxTask *t = fpi_ssm_get_data (ssm);
  GxSiftFeatures *f = g_steal_pointer (&w->feat);

  (void) src; (void) user_data;
  for (guint e = 0; w->extra && e < w->extra->len; e++)
    g_ptr_array_add (t->views, g_ptr_array_index (w->extra, e));
  if (w->extra)
    g_ptr_array_set_free_func (w->extra, NULL);

  if (!f || f->n < 8)
    {
      FpDeviceRetry why = gx_finger_present (self)
                            ? FP_DEVICE_RETRY_CENTER_FINGER
                            : FP_DEVICE_RETRY_TOO_SHORT;

      g_clear_pointer (&f, gx_sift_free);
      if (t->verifying)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_retry_new (why));
          return;
        }
      fpi_device_enroll_progress (dev, t->stage, NULL,
                                  fpi_device_retry_new (why));
    }
  else if (t->verifying)
    {
      FpPrint *tmpl = NULL;
      g_autoptr(GPtrArray) views = NULL;
      g_autoptr(GPtrArray) extra = NULL;
      g_autofree gchar *apath = NULL;
      guint i;

      fpi_device_get_verify_data (dev, &tmpl);
      views = gx_views_from_print (tmpl);
      apath = gx_adapt_path (tmpl);
      extra = gx_adapt_load (apath);

      {
        g_autofree guint8 *seen = g_new0 (guint8, f->n ? f->n : 1);
        int fused = 0;

        for (i = 0; i < views->len; i++)
          gx_sift_match_mask (g_ptr_array_index (views, i), f, seen);
        for (i = extra->len > GX_ADAPT_USE ? extra->len - GX_ADAPT_USE : 0;
             i < extra->len; i++)
          gx_sift_match_mask (g_ptr_array_index (extra, i), f, seen);
        for (i = 0; i < f->n; i++)
          fused += seen[i];
        if (fused > t->best)
          t->best = fused;
      }
      t->tries++;
      g_clear_pointer (&t->probe, gx_sift_free);
      t->probe = f;
      fp_info ("verify: attempt %d -> %d matches (threshold %d)",
               t->tries, t->best, GX_MATCH_THRESHOLD);
    }
  else
    {
      int dx = 0, dy = 0, sc = 0, x0, y0, x1, y1;
      guint np = 0;
      gboolean placed;

      g_ptr_array_add (t->views, f);
      t->stage++;
      if (!t->island)
        t->island = gx_sift_island_new ();
      placed = gx_sift_island_add (t->island, f, &dx, &dy, &sc);
      gx_sift_island_extent (t->island, &x0, &y0, &x1, &y1, &np);
      fp_info ("enroll: stage=%d overlap=%d extent=%dx%d points=%u%s",
               t->stage, sc, x1 - x0, y1 - y0, np, placed ? "" : " (disjoint)");
      fpi_device_enroll_progress (dev, t->stage, NULL, NULL);
    }
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
  t->polls = 0;
  self->poll_id = g_timeout_add (GX_POLL_MS, gx_poll_off, ssm);
}

static void
gx_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (dev);
  GxTask *t = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case GX_ST_SESSION:
      gx_run_async (ssm, dev, gx_session_thread, gx_session_done);
      break;
    case GX_ST_WAIT_ON:
      t->polls = 0;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      self->poll_id = g_timeout_add (GX_POLL_MS, gx_poll_on, ssm);
      break;
    case GX_ST_CAPTURE:
      gx_run_async (ssm, dev, gx_capture_thread, gx_capture_done);
      break;
    case GX_ST_WAIT_OFF:
      break;
    case GX_ST_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
gx_enroll_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  GxTask *t = fpi_ssm_get_data (ssm);
  FpPrint *print = NULL;

  if (error)
    {
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }
  if (!t->views || t->views->len < 3)
    {
      fpi_device_enroll_complete (dev, NULL,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "too few usable views"));
      return;
    }
  fpi_device_get_enroll_data (dev, &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, "fpi-data", gx_views_to_variant (t->views), NULL);
  gx_adapt_clear (print);
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
gx_verify_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  GxTask *t = fpi_ssm_get_data (ssm);
  FpPrint *template = NULL;
  g_autoptr(GPtrArray) views = NULL;
  int best;

  if (error)
    {
      /* A retry is reported on the verify itself and the operation then
       * completes cleanly; any other error aborts the operation. Note that
       * reporting transfers the error, so it must not be passed on again. */
      if (error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                                    g_steal_pointer (&error));
          fpi_device_verify_complete (dev, NULL);
        }
      else
        {
          fpi_device_verify_complete (dev, g_steal_pointer (&error));
        }
      return;
    }
  if (!t->probe)
    {
      fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
        fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      fpi_device_verify_complete (dev, NULL);
      return;
    }
  best = t->best;
  fpi_device_get_verify_data (dev, &template);
  views = gx_views_from_print (template);
  {
    g_autofree gchar *apath = gx_adapt_path (template);
    g_autoptr(GPtrArray) extra = gx_adapt_load (apath);

    fp_info ("verify: %d matches (threshold %d, %u views + %u acquired, "
             "probe had %d keypoints)",
             best, GX_MATCH_THRESHOLD, views->len, extra->len,
             t->probe ? t->probe->n : -1);
    if (best >= GX_ADAPT_MIN &&
        best <= (int) (GX_ADAPT_MAX_FRAC * t->probe->n) &&
        extra->len < GX_ADAPT_VIEWS)
      {
        if (gx_adapt_append (apath, t->probe))
          fp_info ("adaptive store: view acquired (%u/%d)",
                   extra->len + 1, GX_ADAPT_VIEWS);
      }
  }
  fpi_device_verify_report (dev,
                            best >= GX_MATCH_THRESHOLD ? FPI_MATCH_SUCCESS
                                                       : FPI_MATCH_FAIL,
                            NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}

/* ------------------------------------------------------------------ */
/*  Device life cycle                                                  */
/* ------------------------------------------------------------------ */

static void
gx_dev_open (FpDevice *dev)
{
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (dev);
  GError *err = NULL;
  const gchar *path;

  gx_adapt_sweep ();

  path = fpi_device_get_udev_data (dev, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);
  self->spi_fd = open (path, O_RDWR);
  if (self->spi_fd < 0)
    {
      g_set_error (&err, G_IO_ERROR, g_io_error_from_errno (errno),
                   "cannot open spidev node %s", path);
      fpi_device_open_complete (dev, err);
      return;
    }
  if (flock (self->spi_fd, LOCK_EX | LOCK_NB) != 0)
    {
      close (self->spi_fd);
      self->spi_fd = -1;
      fpi_device_open_complete (dev, fpi_device_error_new_msg (
        FP_DEVICE_ERROR_BUSY, "sensor already in use by another process"));
      return;
    }
  {
    guint8 mode = SPI_MODE_0, bits = 8;
    guint32 speed = 4000000;
    ioctl (self->spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
  }
  if (!gx_gpio_reset (self))
    {
      close (self->spi_fd);
      self->spi_fd = -1;
      fpi_device_open_complete (dev, fpi_device_error_new_msg (
        FP_DEVICE_ERROR_GENERAL,
        "cannot pulse the reset GPIO (%s). Install the systemd drop-in "
        "systemd/10-goodixmilan-gpio.conf: fprintd's DeviceAllow= sandbox "
        "denies /dev/gpiochip* by default.", g_strerror (errno)));
      return;
    }
  fpi_device_open_complete (dev, NULL);
}

static void
gx_dev_close (FpDevice *dev)
{
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (dev);

  g_clear_handle_id (&self->poll_id, g_source_remove);
  if (self->spi_fd >= 0)
    {
      close (self->spi_fd);
      self->spi_fd = -1;
    }
  g_clear_pointer (&self->bg_frame, g_free);
  fpi_device_close_complete (dev, NULL);
}

static void
gx_dev_enroll (FpDevice *dev)
{
  FpiSsm *ssm = fpi_ssm_new (dev, gx_run_state, GX_ST_NUM);
  GxTask *t = g_new0 (GxTask, 1);
  FpPrint *print = NULL;
  FpiPrintType ptype = FPI_PRINT_UNDEFINED;

  t->views = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
  fpi_device_get_enroll_data (dev, &print);
  if (print)
    g_object_get (print, "fpi-type", &ptype, NULL);
  if (ptype != FPI_PRINT_UNDEFINED)
    {
      g_autoptr(GPtrArray) old = gx_views_from_print (print);
      guint i;
      for (i = 0; i < old->len; i++)
        g_ptr_array_add (t->views, g_ptr_array_index (old, i));
      g_ptr_array_set_free_func (old, NULL);
    }
  fpi_ssm_set_data (ssm, t, (GDestroyNotify) gx_task_free);
  fpi_ssm_start (ssm, gx_enroll_done);
}

static void
gx_dev_verify (FpDevice *dev)
{
  FpiSsm *ssm = fpi_ssm_new (dev, gx_run_state, GX_ST_NUM);
  GxTask *t = g_new0 (GxTask, 1);

  t->verifying = TRUE;
  fpi_ssm_set_data (ssm, t, (GDestroyNotify) gx_task_free);
  fpi_ssm_start (ssm, gx_verify_done);
}

static void
fpi_device_goodixmilan_init (FpiDeviceGoodixMilan *self)
{
  self->spi_fd = -1;
}

static void
fpi_device_goodixmilan_finalize (GObject *object)
{
  FpiDeviceGoodixMilan *self = FPI_DEVICE_GOODIXMILAN (object);

  g_clear_handle_id (&self->poll_id, g_source_remove);
  if (self->spi_fd >= 0)
    close (self->spi_fd);
  g_clear_pointer (&self->bg_frame, g_free);
  G_OBJECT_CLASS (fpi_device_goodixmilan_parent_class)->finalize (object);
}

static void
fpi_device_goodixmilan_class_init (FpiDeviceGoodixMilanClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "goodixmilan";
  dev_class->full_name = "Goodix Milan SPI Fingerprint Sensor (GXFP3200)";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = goodixmilan_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = GX_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = gx_dev_open;
  dev_class->close = gx_dev_close;
  dev_class->enroll = gx_dev_enroll;
  dev_class->verify = gx_dev_verify;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_goodixmilan_finalize;

  fpi_device_class_auto_initialize_features (dev_class);
}
