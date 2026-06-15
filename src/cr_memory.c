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

/* Canonical x86-64 user-space limit. Probe reads above this address risk
 * hitting kernel space or PS5 MMIO regions that cause ptrace page-fault hangs. */
#define ADDR_IN_USER_RANGE(a) ((intptr_t)(a) >= 0x1000L && (intptr_t)(a) <= (intptr_t)0x7FFFFFFFFFFFL)

int
read_process_memory(pid_t pid, intptr_t addr, uint8_t *out, size_t len) {
  if (!out || len == 0) return -1;
  /* Fast path: data/heap pages are readable, so this succeeds for most reads
   * and the page protection is never touched. */
  if (pt_copyout(pid, addr, out, len) == 0) return 0;

  /* Slow path. PS5 maps game .text execute-only (PROT_EXEC, no PROT_READ), so a
   * raw pt_copyout on code faults — this is why baseline reads of hook sites came
   * back unreadable and cheats showed mismatch/partial. Temporarily add PROT_READ,
   * re-read, then restore the page's ORIGINAL protection so W^X is preserved.
   *
   * Guard the address first: kernel_get_vmem_protection can panic on a bad pointer
   * (the same reason write_process_memory_forced avoids it), so never call it for
   * an address outside user space. */
  if (!ADDR_IN_USER_RANGE(addr)) return -1;

  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t   span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) - (uintptr_t)page);

  int op = kernel_get_vmem_protection(pid, page, span);
  if (op < 0) return -1;            /* genuinely unmapped */
  if (op & PROT_READ) return -1;    /* already readable — the read truly failed */

  if (kernel_mprotect(pid, page, span, op | PROT_READ) != 0) return -1;
  int rc = pt_copyout(pid, addr, out, len);
  kernel_mprotect(pid, page, span, op);   /* restore original protection (keeps W^X) */
  return rc < 0 ? -1 : 0;
}


/* Returns 1 if the first two bytes look like a plausible x86-64 instruction start.
 * Used to distinguish absolute vs relative SHN/MC4 offsets when no expected bytes exist.
 * Using two bytes for REX-prefixed instructions cuts the false-positive rate significantly:
 * 0x48 ("H") is common in ASCII strings and pointer data, but REX+valid_opcode is not. */
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
  if (!ADDR_IN_USER_RANGE(addr)) {
    cr_log("error", "cheats.mem",
           "write_rejected addr=0x%lx not_in_user_range — skipping to prevent kernel hang",
           (long)addr);
    return -7;
  }
  /* Split cross-page writes at each page boundary.  kernel_mprotect with a
   * multi-page span returns 0 on PS5 but silently only unlocks the first page;
   * subsequent pages stay R-X and pt_copyin silently truncates the write.
   * One mprotect+write per page is the only reliable approach. */
  intptr_t next_page = (intptr_t)ROUND_PG_UP((uintptr_t)addr + 1);
  if (addr + (intptr_t)len > next_page) {
    size_t first = (size_t)(next_page - addr);
    int r = write_process_memory_forced(pid, addr, data, first);
    if (r != 0) return r;
    return write_process_memory_forced(pid, next_page, data + first, len - first);
  }

  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t   span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) - (uintptr_t)page);
  /* W^X: make the page WRITABLE (drop EXEC) for the write, never W+X. PS5's
   * hypervisor rejects a writable+executable code mapping; setting RWX — even
   * momentarily — can trigger a delayed kernel panic when the page is next
   * executed (this was the repeated PPSA30803 cheat-apply panic). We restore
   * R-X after the write so the patched code stays executable but not writable. */
  int mrc = kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE);
  cr_log("debug", "cheats.mem", "mprotect_rw page=0x%lx span=0x%zx rc=%d",
         (long)page, span, mrc);
  if (mrc != 0) {
    /* kernel_mprotect refused to make this page writable (special vmem entry,
     * hardware-enforced protection, or PS5 integrity guard).
     *
     * Strategy:
     *   1. Try pt_copyin without mprotect — ptrace on a jailbroken PS5 can
     *      often write to protected pages directly.
     *   2. If that also fails AND the write is a large cave (>=16 bytes):
     *      try write_via_codecave (remaps the page with MAP_FIXED).
     *   3. For small hook writes (<16 bytes): NEVER remap — MAP_FIXED on a
     *      kernel-protected code page causes a kernel panic when the page is
     *      executed. Fail cleanly instead. */
    cr_log("warn", "cheats.mem",
           "mprotect_failed page=0x%lx rc=%d — trying direct ptrace write addr=0x%lx len=%zu",
           (long)page, mrc, (long)addr, len);
    int drc = pt_copyin(pid, data, addr, len);
    if (drc == 0) {
      cr_log("info", "cheats.mem",
             "direct_ptrace_write ok addr=0x%lx len=%zu (mprotect was refused)", (long)addr, len);
      return 0;
    }
    cr_log("warn", "cheats.mem",
           "direct_ptrace_write failed addr=0x%lx len=%zu rc=%d", (long)addr, len, drc);
    if (len >= 16) {
      cr_log("info", "cheats.mem",
             "codecave_remap fallback addr=0x%lx len=%zu (large cave write)", (long)addr, len);
      return write_via_codecave(pid, addr, data, len);
    }
    cr_log("error", "cheats.mem",
           "hook_write_blocked addr=0x%lx len=%zu — page is kernel-protected and cannot be remapped safely; skipping to prevent kernel panic",
           (long)addr, len);
    return -6;
  }
  int wrc = pt_copyin(pid, data, addr, len);
  cr_log("debug", "cheats.mem", "copyin addr=0x%lx len=%zu rc=%d", (long)addr, len, wrc);
  /* Restore R-X (drop WRITE, re-add EXEC). Combined with the RW mprotect above,
   * the page is never Writable+eXecutable at the same instant — satisfying PS5's
   * hypervisor W^X rule. Future writes go through pt_copyin (ptrace PT_IO), which
   * bypasses page protection on a jailbroken PS5, so dropping WRITE here is safe. */
  int rrc = kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);
  if (rrc != 0) {
    /* Restore failed. On PS4 BC pages the emulation layer manages page protections
     * independently — mprotect(RX) is rejected but the hardware PTE still allows
     * execution, so the written bytes execute correctly. Returning success here
     * matches the pre-existing behaviour that patches on Bloodborne and similar
     * PS4 BC titles depended on. */
    cr_log("warn", "cheats.mem",
           "restore_rx_failed page=0x%lx span=0x%zx rc=%d", (long)page, span, rrc);
  }
  if (wrc == 0 && len <= 128) {
    uint8_t vbuf[128];
    if (pt_copyout(pid, addr, vbuf, len) == 0 && memcmp(vbuf, data, len) != 0)
      cr_log("warn", "cheats.mem",
             "write_verify_mismatch addr=0x%lx len=%zu — bytes did not change after write",
             (long)addr, len);
  }
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

  /* Set R-X before verify. Never leave a page RWX — PS5's memory integrity
   * scanner panics on executable+writable pages. pt_copyin (ptrace PT_IO)
   * bypasses page protections on jailbroken PS5, so the rollback write below
   * works without needing to re-elevate to RWX. */
  kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);

  /* Verify */
  uint8_t vbuf[256];
  if (pt_copyout(pid, addr, vbuf, len) < 0 || memcmp(vbuf, data, len) != 0) {
    cr_log("warn", "cheats.cave",
           "cave verify failed addr=0x%lx len=%zu — restoring original page", (long)addr, len);
    /* Roll back via ptrace (bypasses R-X protection on jailbroken PS5). */
    pt_copyin(pid, orig, page, span);
    kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);
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
