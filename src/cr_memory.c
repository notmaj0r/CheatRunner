#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <ps5/kernel.h>

#include "cr_log.h"
#include "cr_config.h"
#include "cr_memory.h"
#include "ps5sdk_compat.h"
#include "pt.h"

#define ROUND_PG_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)0x3fff)
#define ROUND_PG_UP(x) (((uintptr_t)(x) + 0x3fff) & ~(uintptr_t)0x3fff)

static int
hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

int
parse_offset_hex_checked(const char *s, uint64_t *out) {
  if (!s || !*s || !out) return -1;
  while (*s == ' ' || *s == '\t') s++;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (!*s) return -1;
  for (const char *p = s; *p; p++) {
    if (!isxdigit((unsigned char)*p)) return -1;
  }
  errno = 0;
  unsigned long long v = strtoull(s, NULL, 16);
  if (errno != 0) return -1;
  *out = (uint64_t)v;
  return 0;
}

int
parse_hex_bytes_checked(const char *s, uint8_t *out, size_t out_cap, size_t *out_len) {
  if (!s || !out || !out_len || out_cap == 0) return -1;
  size_t w = 0;
  int high = -1;
  for (const char *p = s; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '-' || *p == ',' || *p == ':') continue;
    int n = hex_nibble(*p);
    if (n < 0) return -1;
    if (high < 0) {
      high = n;
    } else {
      if (w >= out_cap) return -1;
      out[w++] = (uint8_t)((high << 4) | n);
      high = -1;
    }
  }
  if (high >= 0 || w == 0) return -1;
  *out_len = w;
  return 0;
}

int
read_process_memory(pid_t pid, intptr_t addr, uint8_t *out, size_t len) {
  if (!out || len == 0) return -1;
  if (pt_copyout(pid, addr, out, len) < 0) return -1;
  return 0;
}

static void
fmt_hex16(const uint8_t *b, size_t len, char *buf, size_t buf_sz) {
  size_t n = len < 16 ? len : 16;
  size_t pos = 0;
  for (size_t i = 0; i < n && pos + 4 < buf_sz; i++) {
    pos += (size_t)snprintf(buf + pos, buf_sz - pos, "%02x ", b[i]);
  }
  if (len > 16 && pos + 4 < buf_sz) {
    snprintf(buf + pos, buf_sz - pos, "...");
  }
}

int
write_process_memory(pid_t pid, intptr_t addr, const uint8_t *data, size_t len) {
  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) - (uintptr_t)page);

  int orig_prot = kernel_get_vmem_protection(pid, page, span);
  if (orig_prot < 0) orig_prot = PROT_READ | PROT_EXEC;

  int mrc = kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
  cr_log("info", "cheats.mem", "mprotect_rwx page=0x%lx span=0x%zx orig_prot=%d rc=%d",
         (long)page, span, orig_prot, mrc);
  if (mrc != 0) return -4;

  int wrc = pt_copyin(pid, data, addr, len);
  cr_log("info", "cheats.mem", "copyin addr=0x%lx len=%zu rc=%d", (long)addr, len, wrc);
  if (wrc < 0) {
    kernel_mprotect(pid, page, span, orig_prot);
    return -1;
  }

  kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);

  uint8_t vbuf[256];
  size_t off = 0;
  while (off < len) {
    size_t chunk = len - off;
    if (chunk > sizeof(vbuf)) chunk = sizeof(vbuf);
    int rrc = pt_copyout(pid, addr + (intptr_t)off, vbuf, chunk);
    if (rrc < 0) {
      cr_log("warn", "cheats.mem", "int_verify_read failed addr=0x%lx off=%zu rc=%d restore_prot=%d",
             (long)addr, off, rrc, orig_prot);
      kernel_mprotect(pid, page, span, orig_prot);
      return -2;
    }
    if (memcmp(vbuf, data + off, chunk) != 0) {
      char exp_h[52] = {0}, got_h[52] = {0};
      fmt_hex16(data + off, chunk, exp_h, sizeof(exp_h));
      fmt_hex16(vbuf, chunk, got_h, sizeof(got_h));
      cr_log("warn", "cheats.mem", "int_verify_mismatch addr=0x%lx off=%zu exp=[%s] got=[%s] restore_prot=%d",
             (long)addr, off, exp_h, got_h, orig_prot);
      kernel_mprotect(pid, page, span, orig_prot);
      return -3;
    }
    off += chunk;
  }

  cr_log("info", "cheats.mem", "int_verify ok addr=0x%lx len=%zu page_rwx", (long)addr, len);
  return 0;
}

intptr_t
cheat_resolve_write_addr(pid_t pid, intptr_t base, uint64_t off_u,
                         int abs_flag, int is_non_json, int auto_detect,
                         const uint8_t *on_b, size_t byte_len,
                         const uint8_t *expect_b) {
  intptr_t abs_addr = (intptr_t)off_u;
  intptr_t rel_addr = base + (intptr_t)off_u;

  if (!is_non_json || abs_flag) {
    return abs_flag ? abs_addr : rel_addr;
  }
  if (!auto_detect || off_u < 0x200000ULL || pid <= 0 || byte_len == 0 || byte_len > 128) {
    return rel_addr;
  }

  uint8_t probe[128];
  if (read_process_memory(pid, abs_addr, probe, byte_len) == 0) {
    if (memcmp(probe, on_b, byte_len) == 0 || (expect_b && memcmp(probe, expect_b, byte_len) == 0)) {
      return abs_addr;
    }
  }
  if (read_process_memory(pid, rel_addr, probe, byte_len) == 0) {
    if (memcmp(probe, on_b, byte_len) == 0 || (expect_b && memcmp(probe, expect_b, byte_len) == 0)) {
      return rel_addr;
    }
  }
  return abs_addr;
}

void
bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!bytes || len == 0) return;
  size_t pos = 0;
  for (size_t i = 0; i < len; i++) {
    int n = snprintf(out + pos, out_size - pos, "%02X", bytes[i]);
    if (n <= 0 || (size_t)n >= out_size - pos) break;
    pos += (size_t)n;
  }
}

int
resolve_module_base(pid_t pid, const char *module_name, intptr_t eboot_base, intptr_t *out_base) {
  if (!out_base) return -1;
  if (!module_name || !module_name[0]) {
    *out_base = eboot_base;
    return 0;
  }
  uint32_t handle = 0;
  if (kernel_dynlib_handle(pid, module_name, &handle) != 0) {
    cr_log("warn", "cheats.mem", "resolve_module_base: module '%s' not loaded in pid=%d", module_name, (int)pid);
    return -1;
  }
  intptr_t mbase = kernel_dynlib_mapbase_addr(pid, handle);
  if (mbase <= 0) {
    cr_log("warn", "cheats.mem", "resolve_module_base: mapbase invalid for '%s' handle=%u base=0x%lx",
           module_name, handle, (long)mbase);
    return -1;
  }
  cr_log("info", "cheats.mem", "resolve_module_base: '%s' base=0x%lx", module_name, (long)mbase);
  *out_base = mbase;
  return 0;
}

int
process_is_ps2_emu(pid_t pid) {
  uint32_t handle = 0;
  return kernel_dynlib_handle(pid, "libScePs2EmuMenuDialog.sprx", &handle) == 0;
}

int
write_via_codecave(pid_t pid, intptr_t addr, const uint8_t *data, size_t len) {
  if (!data || len == 0 || len > 128) return -1;
  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t   span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) - (uintptr_t)page);

  /* Save original page content before replacing the mapping */
  uint8_t *orig = malloc(span);
  if (!orig) return -1;
  if (pt_copyout(pid, page, orig, span) < 0) {
    cr_log("warn", "cheats.cave", "cave read orig failed addr=0x%lx span=%zu", (long)page, span);
    free(orig);
    return -1;
  }

  /* Replace the existing mapping with an anonymous RW page at the same address */
  intptr_t cave = pt_mmap(pid, page, span,
                          PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
  if (cave <= 0) {
    cr_log("warn", "cheats.cave", "cave pt_mmap failed addr=0x%lx span=%zu rc=%ld", (long)page, span, (long)cave);
    free(orig);
    return -1;
  }

  /* Restore original bytes then apply the patch */
  pt_copyin(pid, orig, page, span);
  free(orig);
  pt_copyin(pid, data, addr, len);

  /* Make the page RX */
  kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);

  /* Verify */
  uint8_t vbuf[128];
  if (pt_copyout(pid, addr, vbuf, len) < 0 || memcmp(vbuf, data, len) != 0) {
    cr_log("warn", "cheats.cave", "cave verify failed addr=0x%lx len=%zu", (long)addr, len);
    return -1;
  }
  cr_log("info", "cheats.cave", "cave write ok addr=0x%lx len=%zu", (long)addr, len);
  return 0;
}

void
get_cheat_addr_flags(int kind, int entry_abs_flag, int auto_detect,
                     int *abs_flag_out, int *is_non_json_out, int *auto_detect_out) {
  if (entry_abs_flag) {
    *abs_flag_out = 1; *is_non_json_out = 0; *auto_detect_out = 0;
    return;
  }
  char mode[16] = "auto";
  pthread_mutex_lock(&g_cfg_lock);
  if (kind == 2) snprintf(mode, sizeof(mode), "%s", g_cfg.cheat_shn_address_mode);
  else if (kind == 3) snprintf(mode, sizeof(mode), "%s", g_cfg.cheat_mc4_address_mode);
  pthread_mutex_unlock(&g_cfg_lock);
  if (strcmp(mode, "absolute") == 0) {
    *abs_flag_out = 1; *is_non_json_out = 0; *auto_detect_out = 0;
  } else if (strcmp(mode, "relative") == 0) {
    *abs_flag_out = 0; *is_non_json_out = 0; *auto_detect_out = 0;
  } else {
    *abs_flag_out = 0; *is_non_json_out = (kind != 1); *auto_detect_out = auto_detect;
  }
}
