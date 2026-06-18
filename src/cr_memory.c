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
#include "cr_mdbg.h"
#include "cr_memory.h"
#include "ps5sdk_compat.h"

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
  /* No XOM fallback anymore — kernel_get_vmem_protection/kernel_mprotect on a
   * wrong/special address can hang or panic, same risk already fixed on the
   * write side. An execute-only page with no PROT_READ just reads as failed. */
  return mdbg_io_copyout(pid, addr, out, len) == 0 ? 0 : -1;
}


/* Returns 1 if the first two bytes look like a plausible x86-64 instruction
 * start — used to guess abs vs relative offsets when no expected bytes exist. */
static int
x86_looks_like_code(const uint8_t *b, size_t len) {
  if (!b || len == 0) return 0;
  uint8_t b0 = b[0];

  /* Single-byte unambiguous instructions: PUSH/POP reg, NOP, RET */
  if ((b0 >= 0x50 && b0 <= 0x5F) || b0 == 0x90 || b0 == 0xC3) return 1;

  /* Unambiguous control-flow: CALL rel32, JMP rel32/rel8, Jcc rel8 */
  if (b0 == 0xE8 || b0 == 0xE9 || b0 == 0xEB) return 1;
  if (b0 >= 0x72 && b0 <= 0x7F) return 1;

  /* All remaining checks require a second byte. */
  if (len < 2) return 0;
  uint8_t b1 = b[1];

  /* REX prefix (0x40-0x4F) must be followed by a known opcode. */
  if (b0 >= 0x40 && b0 <= 0x4F) {
    switch (b1) {
      case 0x01: case 0x03: case 0x09: case 0x0B: case 0x0F:
      case 0x21: case 0x23: case 0x29: case 0x2B:
      case 0x31: case 0x33: case 0x39: case 0x3B:
      case 0x63: case 0x69: case 0x6B:
      case 0x81: case 0x83: case 0x85: case 0x87: case 0x89: case 0x8B: case 0x8D:
      case 0xC1: case 0xC7: case 0xD3: case 0xF7: case 0xFF:
        return 1;
      default: return 0;
    }
  }

  /* 2-byte escape: 0x0F + known second byte */
  if (b0 == 0x0F) {
    switch (b1) {
      case 0x10: case 0x11: case 0x1F:
      case 0x28: case 0x29:
      case 0x44: case 0x45: case 0x84: case 0x85: case 0x86: case 0x87:
      case 0xAF: case 0xB6: case 0xB7: case 0xBE: case 0xBF:
        return 1;
      default: return 0;
    }
  }

  /* Operand-size prefix + real opcode */
  if (b0 == 0x66) {
    switch (b1) {
      case 0x0F: case 0x89: case 0x8B: case 0xC7: case 0xFF:
        return 1;
      default: return 0;
    }
  }

  /* MOV/LEA/CMP/ADD/SUB/XOR/AND/TEST + ModRM.
   * Exclude ModRM=0x00 ([rax]+no displacement) — too common in zeroed data. */
  switch (b0) {
    case 0x29: case 0x2B: case 0x31: case 0x33:
    case 0x81: case 0x83: case 0x85:
    case 0x89: case 0x8B: case 0x8D:
    case 0xC7: case 0xFF:
      return b1 != 0x00;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      return 1; /* MOV reg, imm64 */
    case 0xF2: case 0xF3:
      return (b1 == 0x0F || (b1 >= 0xA4 && b1 <= 0xAF)) ? 1 : 0;
    default:
      return 0;
  }
}


intptr_t
cheat_resolve_write_addr_ex(pid_t pid, intptr_t base, uint64_t off_u,
                             int abs_flag, int is_non_json, int auto_detect,
                             const uint8_t *on_b, size_t byte_len,
                             const uint8_t *expect_b, int expected_reliable,
                             cr_addr_fallback_policy_t fallback_policy,
                             cr_addr_resolve_status_t *resolve_status,
                             int silent) {
  if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_VERIFIED;
  /* Unsigned add avoids signed-overflow UB on a huge/garbage off_u. */
  intptr_t abs_addr = (intptr_t)off_u;
  intptr_t rel_addr = (intptr_t)((uint64_t)base + off_u);

  /* JSON or explicit absolute flag: address is determined without heuristics. */
  if (!is_non_json || abs_flag) {
    return abs_flag ? abs_addr : rel_addr;
  }

  /* Non-JSON without reliable expected bytes: try several probes before falling back. */
  if (!expected_reliable) {
    /* Tier 1: exact off_bytes match against both candidates — most reliable,
     * covers hooks and caves alike (unlike the x86 heuristic below). */
    if (expect_b != NULL && auto_detect && pid > 0 && off_u >= 0x1000ULL &&
        byte_len > 0 && byte_len <= 128) {
      uint8_t probe_off[128];
      /* Relative first — the raw absolute offset can map to PS5 GPU/MMIO and
       * freeze the game on read. */
      int rel_off_match = (ADDR_IN_USER_RANGE(rel_addr) &&
                           read_process_memory(pid, rel_addr, probe_off, byte_len) == 0 &&
                           memcmp(probe_off, expect_b, byte_len) == 0);
      int abs_off_match = (!rel_off_match &&
                           ADDR_IN_USER_RANGE(abs_addr) &&
                           read_process_memory(pid, abs_addr, probe_off, byte_len) == 0 &&
                           memcmp(probe_off, expect_b, byte_len) == 0);
      if (rel_off_match) {
        if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_OFFBYTES_PROBE;
        if (!silent)
          cr_log("info", "cheats.mem",
                 "addr_offbytes_probe off=0x%llx → relative=0x%lx (off_bytes match)",
                 (unsigned long long)off_u, (long)rel_addr);
        return rel_addr;
      }
      if (abs_off_match) {
        if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_OFFBYTES_PROBE;
        if (!silent)
          cr_log("info", "cheats.mem",
                 "addr_offbytes_probe off=0x%llx → absolute=0x%lx (off_bytes match)",
                 (unsigned long long)off_u, (long)abs_addr);
        return abs_addr;
      }
      /* off_bytes not found at either candidate (ValueOff may be NOPs/placeholders).
       * Fall through to x86_probe for hooks, then to policy-based fallback. */
      if (!silent)
        cr_log("debug", "cheats.mem",
               "addr_offbytes_probe off=0x%llx no match at abs=0x%lx or rel=0x%lx — trying x86_probe",
               (unsigned long long)off_u, (long)abs_addr, (long)rel_addr);
    }

    /* Tier 2: x86 instruction-prefix heuristic, hooks only — weaker than Tier 1
     * but useful when ValueOff is generic (NOPs/zeros). */
    if (auto_detect && pid > 0 && off_u >= 0x1000ULL && byte_len > 0 && byte_len < 16) {
      uint8_t pb_rel[4] = {0}, pb_abs[4] = {0};
      /* Relative first again — same GPU/MMIO freeze risk on the raw offset. */
      int rel_read = (ADDR_IN_USER_RANGE(rel_addr) &&
                      read_process_memory(pid, rel_addr, pb_rel, 4) == 0);
      if (rel_read && x86_looks_like_code(pb_rel, 4)) {
        if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_X86_PROBE;
        if (!silent)
          cr_log("info", "cheats.mem",
                 "addr_x86_probe off=0x%llx → relative=0x%lx (x86 match)",
                 (unsigned long long)off_u, (long)rel_addr);
        return rel_addr;
      }
      int abs_read = (ADDR_IN_USER_RANGE(abs_addr) &&
                      read_process_memory(pid, abs_addr, pb_abs, 4) == 0);
      if (abs_read && x86_looks_like_code(pb_abs, 4)) {
        if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_X86_PROBE;
        if (!silent)
          cr_log("info", "cheats.mem",
                 "addr_x86_probe off=0x%llx → absolute=0x%lx (x86 match) rel=0x%lx (%s)",
                 (unsigned long long)off_u, (long)abs_addr, (long)rel_addr,
                 rel_read ? "non-x86" : "unreadable");
        return abs_addr;
      }
      /* Neither candidate looks like x86 — fall through to policy-based fallback */
      if (!silent && (rel_read || abs_read))
        cr_log("debug", "cheats.mem",
               "addr_x86_probe off=0x%llx ambiguous: rel=0x%lx (%s) abs=0x%lx (%s) — using policy",
               (unsigned long long)off_u,
               (long)rel_addr, rel_read ? "non-x86" : "unreadable",
               (long)abs_addr, abs_read ? "non-x86" : "unreadable");
    }

    intptr_t fallback;
    cr_addr_resolve_status_t st;
    const char *mode_str;
    switch (fallback_policy) {
      case CR_ADDR_FALLBACK_BLOCK:
        if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_BLOCKED_NO_BASELINE;
        if (!silent)
          cr_log("warn", "cheats.mem",
                 "addr_blocked_no_baseline off=0x%llx base=0x%lx abs=0x%lx rel=0x%lx policy=block",
                 (unsigned long long)off_u, (long)base, (long)abs_addr, (long)rel_addr);
        return 0;
      case CR_ADDR_FALLBACK_ABSOLUTE:
        fallback = abs_addr;
        st       = CR_ADDR_RESOLVE_OK_UNVERIFIED_ABSOLUTE;
        mode_str = "absolute";
        break;
      case CR_ADDR_FALLBACK_LEGACY:
        /* Legacy magnitude heuristic: large offsets treated as PS5 absolute addresses.
         * This is wrong for most MC4/SHN files — use "relative" instead (see config). */
        fallback = (off_u >= 0x200000ULL) ? abs_addr : rel_addr;
        st       = CR_ADDR_RESOLVE_OK_UNVERIFIED_LEGACY;
        mode_str = "legacy_magnitude_heuristic";
        break;
      case CR_ADDR_FALLBACK_RELATIVE:
      default:
        fallback = rel_addr;
        st       = CR_ADDR_RESOLVE_OK_UNVERIFIED_RELATIVE;
        mode_str = "relative";
        break;
    }
    if (resolve_status) *resolve_status = st;
    if (!silent)
      cr_log("info", "cheats.mem",
             "addr_unverified_%s off=0x%llx base=0x%lx abs=0x%lx rel=0x%lx using=0x%lx reason=no_reliable_expected policy=%s",
             mode_str, (unsigned long long)off_u, (long)base, (long)abs_addr, (long)rel_addr, (long)fallback, mode_str);
    return fallback;
  }

  /* Reliable expected bytes available — probe both candidates to pick the right one. */
  if (!auto_detect || off_u < 0x200000ULL || pid <= 0 || byte_len == 0 || byte_len > 128) {
    return rel_addr;
  }

  /* Relative candidate first (same GPU/MMIO freeze risk); relative wins ties. */
  uint8_t probe[128];

  if (read_process_memory(pid, rel_addr, probe, byte_len) == 0 &&
      (memcmp(probe, on_b, byte_len) == 0 ||
       (expect_b && memcmp(probe, expect_b, byte_len) == 0))) {
    if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_VERIFIED;
    return rel_addr;
  }
  if (read_process_memory(pid, abs_addr, probe, byte_len) == 0 &&
      (memcmp(probe, on_b, byte_len) == 0 ||
       (expect_b && memcmp(probe, expect_b, byte_len) == 0))) {
    if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_VERIFIED;
    return abs_addr;
  }

  /* Reliable expected provided but neither candidate matched — true unresolved. */
  if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_UNRESOLVED;
  if (!silent)
    cr_log("warn", "cheats.mem",
           "addr_unresolved off=0x%llx abs=0x%lx rel=0x%lx neither matched expected bytes",
           (unsigned long long)off_u, (long)abs_addr, (long)rel_addr);
  return 0;
}

intptr_t
cheat_resolve_write_addr(pid_t pid, intptr_t base, uint64_t off_u,
                         int abs_flag, int is_non_json, int auto_detect,
                         const uint8_t *on_b, size_t byte_len,
                         const uint8_t *expect_b,
                         cr_addr_resolve_status_t *resolve_status) {
  int expected_reliable = (expect_b != NULL) ? 1 : 0;
  /* Callers that pass NULL resolve_status are scan/validate contexts — suppress logs. */
  int silent = (resolve_status == NULL) ? 1 : 0;
  return cheat_resolve_write_addr_ex(pid, base, off_u, abs_flag, is_non_json, auto_detect,
                                     on_b, byte_len, expect_b, expected_reliable,
                                     CR_ADDR_FALLBACK_BLOCK, resolve_status, silent);
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
write_process_memory_forced(pid_t pid, intptr_t addr, const uint8_t *data, size_t len) {
  if (!ADDR_IN_USER_RANGE(addr)) {
    cr_log("error", "cheats.mem",
           "write_rejected addr=0x%lx not_in_user_range — skipping to prevent kernel hang",
           (long)addr);
    return -7;
  }
  /* Split cross-page writes — old kernel_mprotect multi-page quirk, kept defensively. */
  intptr_t next_page = (intptr_t)ROUND_PG_UP((uintptr_t)addr + 1);
  if (addr + (intptr_t)len > next_page) {
    size_t first = (size_t)(next_page - addr);
    int r = write_process_memory_forced(pid, addr, data, first);
    if (r != 0) return r;
    return write_process_memory_forced(pid, next_page, data + first, len - first);
  }

  /* No kernel_mprotect: mdbg bypasses page protection, and mprotect itself was
   * the thing corrupting kernel VM structures on wrong/special pages. */
  int wrc = mdbg_io_copyin(pid, data, addr, len);
  cr_log("debug", "cheats.mem", "direct_mdbg_write addr=0x%lx len=%zu rc=%d",
         (long)addr, len, wrc);
  if (wrc == 0 && len <= 128) {
    uint8_t vbuf[128];
    if (mdbg_io_copyout(pid, addr, vbuf, len) == 0 && memcmp(vbuf, data, len) != 0)
      cr_log("warn", "cheats.mem",
             "write_verify_mismatch addr=0x%lx len=%zu — bytes did not change after write",
             (long)addr, len);
  }
  return (wrc < 0) ? -1 : 0;
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
