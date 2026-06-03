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
write_process_memory_ex(pid_t pid, intptr_t addr, const uint8_t *data, size_t len,
                         int *orig_prot_out) {
  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) - (uintptr_t)page);

  int orig_prot = kernel_get_vmem_protection(pid, page, span);
  if (orig_prot < 0) {
    cr_log("warn", "cheats.mem", "get_vmem_protection failed page=0x%lx span=0x%zx rc=%d — aborting write",
           (long)page, span, orig_prot);
    if (orig_prot_out) *orig_prot_out = -1;
    return -5;
  }
  if (orig_prot_out) *orig_prot_out = orig_prot;

  int mrc = kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
  cr_log("debug", "cheats.mem", "mprotect_rwx page=0x%lx span=0x%zx orig_prot=%d rc=%d",
         (long)page, span, orig_prot, mrc);
  if (mrc != 0) return -4;

  int wrc = pt_copyin(pid, data, addr, len);
  cr_log("debug", "cheats.mem", "copyin addr=0x%lx len=%zu rc=%d", (long)addr, len, wrc);
  if (wrc < 0) {
    kernel_mprotect(pid, page, span, orig_prot);
    return -1;
  }

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

  /* Restore original page protection — never leave RWX after a successful write. */
  kernel_mprotect(pid, page, span, orig_prot);
  cr_log("debug", "cheats.mem", "int_verify ok addr=0x%lx len=%zu prot_restored=%d", (long)addr, len, orig_prot);
  return 0;
}

int
write_process_memory(pid_t pid, intptr_t addr, const uint8_t *data, size_t len) {
  return write_process_memory_ex(pid, addr, data, len, NULL);
}

/* Returns 1 if the first byte looks like a plausible x86-64 instruction start.
 * Used to distinguish absolute vs relative SHN/MC4 offsets when no expected bytes exist.
 * A null first byte almost always means data/uninitialized memory, not code. */
static int
x86_looks_like_code(const uint8_t *b, size_t len) {
  if (!b || len == 0) return 0;
  uint8_t fb = b[0];
  if (fb >= 0x40 && fb <= 0x4F) return 1; /* REX prefix — extremely common in 64-bit code */
  switch (fb) {
    case 0x0F: /* 2-byte escape (SSE, CMOV, MOVZX, …) */
    case 0x29: case 0x2B:              /* SUB */
    case 0x31: case 0x33:              /* XOR */
    case 0x50: case 0x51: case 0x52: case 0x53: /* PUSH reg */
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: /* POP reg */
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x66:                         /* operand-size prefix */
    case 0x72: case 0x73:              /* JB / JAE */
    case 0x74: case 0x75:              /* JE / JNE */
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: /* JL / JGE / JLE / JG */
    case 0x81: case 0x83: case 0x85:   /* ADD/SUB/AND/OR/CMP + TEST */
    case 0x89: case 0x8B: case 0x8D:   /* MOV / LEA */
    case 0x90:                         /* NOP */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: /* MOV reg, imm64 */
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
    case 0xC3: case 0xC7:              /* RET / MOV r/m, imm32 */
    case 0xE8:                         /* CALL rel32 */
    case 0xE9: case 0xEB:              /* JMP rel32 / rel8 */
    case 0xF2: case 0xF3:              /* REP/REPNE prefix */
    case 0xFF:                         /* CALL/JMP indirect, PUSH r/m, INC/DEC */
      return 1;
    default:
      return 0;
  }
}


/* Canonical x86-64 user-space limit. Probe reads above this address risk
 * hitting kernel space or PS5 MMIO regions that cause ptrace page-fault hangs. */
#define ADDR_IN_USER_RANGE(a) ((intptr_t)(a) >= 0x1000L && (intptr_t)(a) <= (intptr_t)0x7FFFFFFFFFFFL)

intptr_t
cheat_resolve_write_addr_ex(pid_t pid, intptr_t base, uint64_t off_u,
                             int abs_flag, int is_non_json, int auto_detect,
                             const uint8_t *on_b, size_t byte_len,
                             const uint8_t *expect_b, int expected_reliable,
                             cr_addr_fallback_policy_t fallback_policy,
                             cr_addr_resolve_status_t *resolve_status,
                             int silent) {
  if (resolve_status) *resolve_status = CR_ADDR_RESOLVE_OK_VERIFIED;
  intptr_t abs_addr = (intptr_t)off_u;
  intptr_t rel_addr = base + (intptr_t)off_u;

  /* JSON or explicit absolute flag: address is determined without heuristics. */
  if (!is_non_json || abs_flag) {
    return abs_flag ? abs_addr : rel_addr;
  }

  /* Non-JSON without reliable expected bytes: try several probes before falling back. */
  if (!expected_reliable) {
    /* Tier 1 — off_bytes byte-exact probe (highest confidence, runs for any length).
     *
     * When the caller supplies expect_b (off_bytes from MC4/SHN ValueOff) we read
     * the full byte_len from both candidates and pick the one that matches exactly.
     * This is far more reliable than the x86 heuristic: for well-formed cheats
     * ValueOff == original game code bytes, which are unique to the correct address.
     * Covers both hooks AND code caves (byte_len >= 16) which x86_probe skips. */
    if (expect_b != NULL && auto_detect && pid > 0 && off_u >= 0x1000ULL &&
        byte_len > 0 && byte_len <= 128) {
      uint8_t probe_off[128];
      /* Read the relative candidate first: the absolute candidate is the raw
       * cheat offset and can map to PS5 GPU/MMIO, where a ptrace read page-faults
       * and freezes the game.  Checking base+offset first means the raw offset is
       * only touched when relative didn't match. */
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

    /* Tier 2 — X86 instruction heuristic probe (hooks only, len < 16).
     *
     * Reads 4 bytes from both candidates and checks which one starts with a
     * plausible x86-64 instruction prefix.  Weaker than off_bytes probe (first-byte
     * heuristic has ~30-40% false-positive rate) but still useful when ValueOff is
     * generic (NOPs/zeros) and both abs and rel are mapped memory. */
    if (auto_detect && pid > 0 && off_u >= 0x1000ULL && byte_len > 0 && byte_len < 16) {
      uint8_t pb_rel[4] = {0}, pb_abs[4] = {0};
      /* Relative-first (same rule as Tier 1 and the reliable-probe branch): never
       * read the raw absolute offset until the relative candidate is ruled out.
       * The raw offset can map to PS5 GPU/MMIO, where a ptrace read page-faults and
       * never returns, freezing the game.  If the relative candidate already looks
       * like code we return it without ever touching the absolute candidate. */
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

  /* Probe the RELATIVE candidate first and return on match.  The absolute
   * candidate is the raw cheat offset, which on PS5 can map to GPU/MMIO;
   * reading it under ptrace triggers a kernel page-fault that never returns,
   * freezing the game.  base+offset is the correct target for the vast
   * majority of MC4/SHN cheats, so checking it first means the raw offset is
   * never touched in the common case.  Preserves the previous tie-break
   * (relative wins when both match). */
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
  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t   span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) - (uintptr_t)page);
  int mrc = kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
  cr_log("debug", "cheats.mem", "mprotect_rwx page=0x%lx span=0x%zx rc=%d",
         (long)page, span, mrc);
  if (mrc != 0) {
    cr_log("info", "cheats.mem",
           "mprotect_failed page=0x%lx rc=%d — codecave fallback addr=0x%lx len=%zu",
           (long)page, mrc, (long)addr, len);
    return write_via_codecave(pid, addr, data, len);
  }
  int wrc = pt_copyin(pid, data, addr, len);
  cr_log("debug", "cheats.mem", "copyin addr=0x%lx len=%zu rc=%d", (long)addr, len, wrc);
  /* Leave the page READ|WRITE|EXEC. Forcing it back to R-X stripped WRITE from the
   * page — fine for a hook in a pure code page, but MC4/SHN code caves frequently
   * land in a page the GAME also writes to (the cave region sits well past .text,
   * in mixed data). Removing write from such a page makes the game's next store
   * there page-fault → freeze shortly after the cheat is applied. Keeping the page
   * RWX (it was already RWX for the copyin) preserves both execution of the cave
   * and the game's own writes. */
  kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
  return (wrc < 0) ? -1 : 0;
}

int
write_via_codecave(pid_t pid, intptr_t addr, const uint8_t *data, size_t len) {
  if (!data || len == 0 || len > 256) return -1;
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

  /* Restore original bytes then apply the patch.  Keep `orig` until after the
   * verify so a failed write can be rolled back — freeing it here would leave a
   * half-applied patch in the anonymous mapping with no way to restore. */
  pt_copyin(pid, orig, page, span);
  pt_copyin(pid, data, addr, len);

  /* Leave the cave RWX. This page now backs a code cave the game jumps into and
   * may also write through; stripping write could fault the game. */
  kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);

  /* Verify */
  uint8_t vbuf[256];
  if (pt_copyout(pid, addr, vbuf, len) < 0 || memcmp(vbuf, data, len) != 0) {
    cr_log("warn", "cheats.cave",
           "cave verify failed addr=0x%lx len=%zu — restoring original page", (long)addr, len);
    /* Roll the page back to its original contents: this MAP_FIXED page replaced
     * the game's original mapping, so leaving a partially-written cave here can
     * crash the game when the hook jumps into it. */
    pt_copyin(pid, orig, page, span);
    kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    free(orig);
    return -1;
  }
  free(orig);
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
