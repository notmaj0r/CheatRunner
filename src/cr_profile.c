#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ps5sdk_compat.h"
#include "third_party/cJSON.h"
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "cr_api_internal.h"
#include "cr_log.h"
#include "cr_paths.h"
#include "cr_profile.h"

#define AVATAR_WORK "/data/cheatrunner/avatar"

/* The four sizes the PS5 profile cache expects, in this order. */
static const int AVATAR_SIZES[] = {64, 128, 260, 440};
#define AVATAR_NSIZES ((int)(sizeof(AVATAR_SIZES) / sizeof(AVATAR_SIZES[0])))

/* ----------------------------------------------------------------------- */
/* Small helpers                                                           */

static void
send_obj(int fd, int status, cJSON *root) {
  if (!root) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) { http_send_json(fd, 500, "{\"ok\":false,\"error\":\"alloc\"}"); return; }
  http_send_json(fd, status, txt);
  free(txt);
}

static void
send_err(int fd, int status, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  if (o) { cJSON_AddBoolToObject(o, "ok", 0); cJSON_AddStringToObject(o, "error", msg); }
  send_obj(fd, status, o);
}

static int
ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
  return mkdir(path, 0755);
}

/* Foreground user id (0 on failure) + optional name. */
static uint32_t
get_fg_user(char *name_out, size_t name_out_size) {
  uint32_t uid = 0;
  if (sceUserServiceGetForegroundUser(&uid) != 0 || uid == 0 ||
      uid == 0xFFFFFFFFu) {
    if (name_out && name_out_size > 0) name_out[0] = 0;
    return 0;
  }
  if (name_out && name_out_size > 0) {
    char tmp[17] = {0};
    if (sceUserServiceGetUserName((int32_t)uid, tmp, sizeof(tmp)) != 0) tmp[0] = 0;
    size_t n = strlen(tmp);
    if (n >= name_out_size) n = name_out_size - 1;
    memcpy(name_out, tmp, n);
    name_out[n] = 0;
  }
  return uid;
}

/* ----------------------------------------------------------------------- */
/* Bilinear RGBA resize + centre-crop (ported from elf-arsenal avatar.c)   */

static uint8_t *
resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh) {
  uint8_t *dst = malloc((size_t)dw * dh * 4);
  if (!dst) return NULL;
  for (int y = 0; y < dh; y++) {
    double ys = (double)y * sh / dh;
    int y0 = (int)ys, y1 = y0 + 1; if (y1 >= sh) y1 = sh - 1;
    double yF = ys - y0;
    for (int x = 0; x < dw; x++) {
      double xs = (double)x * sw / dw;
      int x0 = (int)xs, x1 = x0 + 1; if (x1 >= sw) x1 = sw - 1;
      double xF = xs - x0;
      const uint8_t *p00 = &src[(y0 * sw + x0) * 4];
      const uint8_t *p10 = &src[(y0 * sw + x1) * 4];
      const uint8_t *p01 = &src[(y1 * sw + x0) * 4];
      const uint8_t *p11 = &src[(y1 * sw + x1) * 4];
      uint8_t *o = &dst[(y * dw + x) * 4];
      for (int c = 0; c < 4; c++) {
        double v = p00[c] * (1.0 - xF) * (1.0 - yF) + p10[c] * xF * (1.0 - yF) +
                   p01[c] * (1.0 - xF) * yF + p11[c] * xF * yF;
        if (v < 0) v = 0; if (v > 255) v = 255;
        o[c] = (uint8_t)(v + 0.5);
      }
    }
  }
  return dst;
}

static uint8_t *
center_crop_square(const uint8_t *src, int sw, int sh, int *side_out) {
  int side = sw < sh ? sw : sh;
  int x0 = (sw - side) / 2, y0 = (sh - side) / 2;
  uint8_t *dst = malloc((size_t)side * side * 4);
  if (!dst) return NULL;
  for (int y = 0; y < side; y++) {
    memcpy(dst + (size_t)y * side * 4,
           src + ((y0 + y) * sw + x0) * 4, (size_t)side * 4);
  }
  *side_out = side;
  return dst;
}

/* ----------------------------------------------------------------------- */
/* DXT5 / DDS encoder (ported from elf-arsenal avatar.c)                   */

static uint16_t rgb888_to_565(int r, int g, int b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static void rgb565_to_888(uint16_t c, int out[3]) {
  out[0] = (((c >> 11) & 0x1f) << 3);
  out[1] = (((c >> 5) & 0x3f) << 2);
  out[2] = ((c & 0x1f) << 3);
}
static int color_distance(const int a[3], const int b[3]) {
  int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
  return dr * dr + dg * dg + db * db;
}

static void
compress_dxt5_block(const uint8_t pixels[64], uint8_t out[16]) {
  int a_min = 255, a_max = 0;
  for (int i = 0; i < 16; i++) {
    int a = pixels[i * 4 + 3];
    if (a < a_min) a_min = a;
    if (a > a_max) a_max = a;
  }
  uint8_t alpha0 = (uint8_t)a_max, alpha1 = (uint8_t)a_min;
  uint8_t apal[8];
  apal[0] = alpha0; apal[1] = alpha1;
  if (alpha0 > alpha1) {
    for (int i = 0; i < 6; i++) apal[2 + i] = (uint8_t)(((6 - i) * alpha0 + (1 + i) * alpha1) / 7);
  } else {
    for (int i = 0; i < 4; i++) apal[2 + i] = (uint8_t)(((4 - i) * alpha0 + (1 + i) * alpha1) / 5);
    apal[6] = 0; apal[7] = 255;
  }
  uint64_t aindex = 0;
  for (int i = 0; i < 16; i++) {
    int a = pixels[i * 4 + 3];
    int best = 0, dist = 256;
    for (int j = 0; j < 8; j++) {
      int d = a - apal[j]; if (d < 0) d = -d;
      if (d < dist) { dist = d; best = j; }
    }
    aindex |= ((uint64_t)best) << (i * 3);
  }
  out[0] = alpha0; out[1] = alpha1;
  for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)((aindex >> (i * 8)) & 0xff);

  int min_c[3] = {255, 255, 255}, max_c[3] = {0, 0, 0};
  for (int i = 0; i < 16; i++) {
    for (int c = 0; c < 3; c++) {
      int v = pixels[i * 4 + c];
      if (v < min_c[c]) min_c[c] = v;
      if (v > max_c[c]) max_c[c] = v;
    }
  }
  uint16_t color0 = rgb888_to_565(max_c[0], max_c[1], max_c[2]);
  uint16_t color1 = rgb888_to_565(min_c[0], min_c[1], min_c[2]);
  if (color0 < color1) {
    uint16_t t = color0; color0 = color1; color1 = t;
    int tmp[3] = {min_c[0], min_c[1], min_c[2]};
    min_c[0] = max_c[0]; min_c[1] = max_c[1]; min_c[2] = max_c[2];
    max_c[0] = tmp[0]; max_c[1] = tmp[1]; max_c[2] = tmp[2];
  } else if (color0 == color1) {
    if (color0 < 0xffff) color0++;
  }
  int c0[3], c1[3];
  rgb565_to_888(color0, c0);
  rgb565_to_888(color1, c1);
  int pal[4][3];
  pal[0][0] = c0[0]; pal[0][1] = c0[1]; pal[0][2] = c0[2];
  pal[1][0] = c1[0]; pal[1][1] = c1[1]; pal[1][2] = c1[2];
  for (int c = 0; c < 3; c++) {
    pal[2][c] = (2 * c0[c] + c1[c]) / 3;
    pal[3][c] = (c0[c] + 2 * c1[c]) / 3;
  }
  uint32_t cindex = 0;
  for (int i = 0; i < 16; i++) {
    int p[3] = {pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]};
    int best = 0, dist = color_distance(p, pal[0]);
    for (int j = 1; j < 4; j++) {
      int d = color_distance(p, pal[j]);
      if (d < dist) { dist = d; best = j; }
    }
    cindex |= ((uint32_t)best) << (i * 2);
  }
  out[8] = color0 & 0xff; out[9] = (color0 >> 8) & 0xff;
  out[10] = color1 & 0xff; out[11] = (color1 >> 8) & 0xff;
  out[12] = cindex & 0xff; out[13] = (cindex >> 8) & 0xff;
  out[14] = (cindex >> 16) & 0xff; out[15] = (cindex >> 24) & 0xff;
}

static int
write_dxt5_dds(const uint8_t *rgba, int w, int h, const char *path) {
  int bw = (w + 3) / 4, bh = (h + 3) / 4;
  size_t comp_size = (size_t)bw * bh * 16;
  uint8_t *buf = malloc(128 + comp_size);
  if (!buf) return -1;
  memset(buf, 0, 128 + comp_size);

  memcpy(buf, "DDS ", 4);
  buf[4] = 124;
  uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
  buf[8] = flags & 0xff; buf[9] = (flags >> 8) & 0xff;
  buf[10] = (flags >> 16) & 0xff; buf[11] = (flags >> 24) & 0xff;
  buf[12] = h & 0xff; buf[13] = (h >> 8) & 0xff;
  buf[14] = (h >> 16) & 0xff; buf[15] = (h >> 24) & 0xff;
  buf[16] = w & 0xff; buf[17] = (w >> 8) & 0xff;
  buf[18] = (w >> 16) & 0xff; buf[19] = (w >> 24) & 0xff;
  uint32_t lsz = (uint32_t)comp_size;
  buf[20] = lsz & 0xff; buf[21] = (lsz >> 8) & 0xff;
  buf[22] = (lsz >> 16) & 0xff; buf[23] = (lsz >> 24) & 0xff;
  buf[76] = 32;
  buf[80] = 4;
  memcpy(buf + 84, "DXT5", 4);
  buf[108] = 0; buf[109] = 0x10;

  uint8_t block[64];
  uint8_t *p = buf + 128;
  for (int by = 0; by < bh; by++) {
    for (int bx = 0; bx < bw; bx++) {
      for (int y = 0; y < 4; y++) {
        int py = by * 4 + y; if (py >= h) py = h - 1;
        for (int x = 0; x < 4; x++) {
          int px = bx * 4 + x; if (px >= w) px = w - 1;
          const uint8_t *src = &rgba[(py * w + px) * 4];
          uint8_t *dst = &block[(y * 4 + x) * 4];
          dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
      }
      compress_dxt5_block(block, p);
      p += 16;
    }
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { free(buf); return -1; }
  ssize_t n = write(fd, buf, 128 + comp_size);
  close(fd);
  free(buf);
  return (n == (ssize_t)(128 + comp_size)) ? 0 : -1;
}

/* Build all four DDS sizes + PNG previews into AVATAR_WORK.
 * mode: "fit" pads to a transparent square; anything else centre-crops. */
static int
build_all_sizes(const uint8_t *rgba, int w, int h, const char *mode,
                char *err, size_t err_size) {
  ensure_dir(CHEATRUNNER_DATA_DIR);
  ensure_dir(AVATAR_WORK);

  uint8_t *square = NULL;
  int side = 0;
  if (mode && !strcasecmp(mode, "fit")) {
    side = w > h ? w : h;
    square = calloc((size_t)side * side, 4);
    if (square) {
      int x0 = (side - w) / 2, y0 = (side - h) / 2;
      for (int y = 0; y < h; y++)
        memcpy(square + ((y0 + y) * side + x0) * 4, rgba + (y * w) * 4, (size_t)w * 4);
    }
  } else {
    square = center_crop_square(rgba, w, h, &side);
  }
  if (!square) { snprintf(err, err_size, "could not square the source image"); return -1; }

  for (int i = 0; i < AVATAR_NSIZES; i++) {
    int sz = AVATAR_SIZES[i];
    uint8_t *resized = (sz == side) ? square : resize_rgba(square, side, side, sz, sz);
    if (!resized) {
      free(square);
      snprintf(err, err_size, "resize failed for %dx%d", sz, sz);
      return -1;
    }
    char dds_path[256], png_path[256];
    snprintf(dds_path, sizeof(dds_path), "%s/avatar%d.dds", AVATAR_WORK, sz);
    snprintf(png_path, sizeof(png_path), "%s/avatar%d.png", AVATAR_WORK, sz);
    if (write_dxt5_dds(resized, sz, sz, dds_path) != 0) {
      if (resized != square) free(resized);
      free(square);
      snprintf(err, err_size, "DDS write failed: %s", dds_path);
      return -1;
    }
    stbi_write_png(png_path, sz, sz, 4, resized, sz * 4);
    if (resized != square) free(resized);
  }
  free(square);
  return 0;
}

/* ----------------------------------------------------------------------- */
/* File I/O                                                                */

static int
read_all(const char *path, uint8_t **out, size_t *out_len) {
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return -1;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  uint8_t *buf = malloc(st.st_size);
  if (!buf) { close(fd); return -1; }
  ssize_t n = read(fd, buf, st.st_size);
  close(fd);
  if (n != st.st_size) { free(buf); return -1; }
  *out = buf; *out_len = (size_t)n;
  return 0;
}

static int
copy_file(const char *src, const char *dst) {
  int sfd = open(src, O_RDONLY);
  if (sfd < 0) return -1;
  int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dfd < 0) { close(sfd); return -1; }
  uint8_t buf[8192];
  ssize_t n;
  while ((n = read(sfd, buf, sizeof(buf))) > 0) {
    if (write(dfd, buf, n) != n) { close(sfd); close(dfd); unlink(dst); return -1; }
  }
  close(sfd);
  close(dfd);
  return 0;
}

/* ----------------------------------------------------------------------- */
/* Handlers                                                                */

static void
whoami_request(int fd) {
  char name[24] = {0};
  uint32_t uid = get_fg_user(name, sizeof(name));
  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  if (uid) {
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08X", uid);
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddNumberToObject(r, "userId", (double)uid);
    cJSON_AddStringToObject(r, "userIdHex", hex);
    cJSON_AddStringToObject(r, "userName", name[0] ? name : "?");
  } else {
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", "no foreground user — sign in to a profile first");
  }
  send_obj(fd, 200, r);
}

static void
set_name_request(int fd, const char *query) {
  char raw[64] = {0};
  if (query_value(query, "name", raw, sizeof(raw)) != 0 || !raw[0]) {
    send_err(fd, 400, "missing 'name'");
    return;
  }
  /* PS5 online IDs/names cap at 16 chars. */
  if (strlen(raw) > 16) { send_err(fd, 400, "name too long (max 16)"); return; }

  char name[24] = {0};
  uint32_t uid = get_fg_user(name, sizeof(name));
  if (!uid) { send_err(fd, 412, "no foreground user — sign in to a profile first"); return; }

  int rc = sceUserServiceSetUserName((int32_t)uid, raw);
  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  if (rc != 0) {
    char err[96];
    snprintf(err, sizeof(err), "sceUserServiceSetUserName failed rc=0x%08X", (unsigned)rc);
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", err);
    send_obj(fd, 500, r);
    return;
  }
  cr_log("info", "profile", "renamed user 0x%08X to '%s'", uid, raw);
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "userName", raw);
  send_obj(fd, 200, r);
}

/* POST body = raw image bytes. Builds preview/DDS sizes; does not apply yet. */
static void
avatar_build_request(int fd, const char *query, const char *body, size_t body_len) {
  if (!body || body_len < 16) { send_err(fd, 400, "empty image upload"); return; }

  char mode[16] = {0};
  if (query_value(query, "mode", mode, sizeof(mode)) != 0) mode[0] = 0;

  int w = 0, h = 0, c = 0;
  uint8_t *rgba = stbi_load_from_memory((const uint8_t *)body, (int)body_len, &w, &h, &c, 4);
  if (!rgba || w <= 0 || h <= 0) {
    char err[160];
    snprintf(err, sizeof(err), "decode failed: %s", stbi_failure_reason() ? stbi_failure_reason() : "bad image");
    if (rgba) stbi_image_free(rgba);
    send_err(fd, 400, err);
    return;
  }

  char err[256] = {0};
  int rc = build_all_sizes(rgba, w, h, mode[0] ? mode : "crop", err, sizeof(err));
  stbi_image_free(rgba);
  if (rc != 0) { send_err(fd, 500, err[0] ? err : "build failed"); return; }

  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddNumberToObject(r, "sourceWidth", w);
  cJSON_AddNumberToObject(r, "sourceHeight", h);
  cJSON_AddStringToObject(r, "mode", mode[0] ? mode : "crop");
  cJSON *sizes = cJSON_AddArrayToObject(r, "sizes");
  for (int i = 0; i < AVATAR_NSIZES; i++) cJSON_AddItemToArray(sizes, cJSON_CreateNumber(AVATAR_SIZES[i]));
  send_obj(fd, 200, r);
}

/* Copy the built DDS/PNG + a generated online.json into the foreground
 * user's profile cache dir. */
static void
avatar_apply_request(int fd) {
  char name[24] = {0};
  uint32_t uid = get_fg_user(name, sizeof(name));
  if (!uid) { send_err(fd, 412, "no foreground user — sign in to a profile first"); return; }

  /* Verify the build step ran. */
  {
    char probe[256];
    snprintf(probe, sizeof(probe), "%s/avatar%d.dds", AVATAR_WORK, AVATAR_SIZES[0]);
    struct stat st;
    if (stat(probe, &st) != 0) { send_err(fd, 412, "no built avatar — upload an image first"); return; }
  }

  char dir[128];
  snprintf(dir, sizeof(dir), "/system_data/priv/cache/profile/0x%08X", uid);
  mkdir("/system_data/priv/cache", 0755);
  mkdir("/system_data/priv/cache/profile", 0755);
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    char err[160];
    snprintf(err, sizeof(err), "mkdir %s: %s", dir, strerror(errno));
    send_err(fd, 500, err);
    return;
  }

  int copied = 0, failed = 0;
  static const char *const KINDS[] = {"avatar", "picture"};
  for (int k = 0; k < 2; k++) {
    for (int i = 0; i < AVATAR_NSIZES; i++) {
      int sz = AVATAR_SIZES[i];
      char src[256], dst[256];
      snprintf(src, sizeof(src), "%s/avatar%d.dds", AVATAR_WORK, sz);
      snprintf(dst, sizeof(dst), "%s/%s%d.dds", dir, KINDS[k], sz);
      if (copy_file(src, dst) == 0) copied++; else failed++;
    }
    /* A PNG copy too (the 440 preview), under both kinds. */
    char psrc[256], pdst[256];
    snprintf(psrc, sizeof(psrc), "%s/avatar%d.png", AVATAR_WORK, AVATAR_SIZES[AVATAR_NSIZES - 1]);
    snprintf(pdst, sizeof(pdst), "%s/%s.png", dir, KINDS[k]);
    if (copy_file(psrc, pdst) == 0) copied++; else failed++;
  }

  /* online.json — mirrors the profile-cache shape; firstName = the name. */
  {
    cJSON *oj = cJSON_CreateObject();
    if (oj) {
      cJSON_AddStringToObject(oj, "firstName", name[0] ? name : "");
      cJSON_AddStringToObject(oj, "lastName", "");
      cJSON_AddStringToObject(oj, "isOfficiallyVerified", "true");
      char *body = cJSON_PrintUnformatted(oj);
      cJSON_Delete(oj);
      if (body) {
        char online[256];
        snprintf(online, sizeof(online), "%s/online.json", dir);
        int ofd = open(online, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (ofd >= 0) {
          size_t blen = strlen(body), off = 0;
          while (off < blen) { ssize_t w = write(ofd, body + off, blen - off); if (w <= 0) break; off += (size_t)w; }
          close(ofd);
          if (off == blen) copied++; else failed++;
        } else failed++;
        free(body);
      }
    }
  }

  cr_log("info", "profile", "applied avatar to user 0x%08X (copied=%d failed=%d)", uid, copied, failed);
  cJSON *r = cJSON_CreateObject();
  if (!r) { http_send_json(fd, 500, "{\"ok\":false}"); return; }
  cJSON_AddBoolToObject(r, "ok", failed == 0);
  cJSON_AddStringToObject(r, "dest", dir);
  cJSON_AddNumberToObject(r, "copied", copied);
  cJSON_AddNumberToObject(r, "failed", failed);
  cJSON_AddStringToObject(r, "note", "Re-open the user switcher (or reboot) to see the new picture.");
  send_obj(fd, failed == 0 ? 200 : 500, r);
}

static void
avatar_preview_request(int fd, const char *query) {
  char size_s[8] = {0};
  int size = (query_value(query, "size", size_s, sizeof(size_s)) == 0) ? atoi(size_s) : 128;
  int ok = 0;
  for (int i = 0; i < AVATAR_NSIZES; i++) if (AVATAR_SIZES[i] == size) { ok = 1; break; }
  if (!ok) size = 128;

  char path[256];
  snprintf(path, sizeof(path), "%s/avatar%d.png", AVATAR_WORK, size);
  uint8_t *body = NULL;
  size_t blen = 0;
  if (read_all(path, &body, &blen) != 0) {
    send_err(fd, 404, "no preview — upload an image first");
    return;
  }
  http_send_response(fd, 200, "image/png", body, blen);
  free(body);
}

int
cr_api_profile_handle(int fd, const char *method, const char *path,
                      const char *query, const char *body, size_t body_len) {
  (void)method;
  if (!strcmp(path, "/api/profile") || !strcmp(path, "/api/profile/whoami")) {
    whoami_request(fd); return 1;
  }
  if (!strcmp(path, "/api/profile/name")) { set_name_request(fd, query); return 1; }
  if (!strcmp(path, "/api/profile/avatar")) { avatar_build_request(fd, query, body, body_len); return 1; }
  if (!strcmp(path, "/api/profile/avatar/apply")) { avatar_apply_request(fd); return 1; }
  if (!strcmp(path, "/api/profile/avatar/preview")) { avatar_preview_request(fd, query); return 1; }
  return 0;
}
