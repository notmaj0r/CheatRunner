#ifndef CR_MEMORY_H
#define CR_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ROUND_PG_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)0x3fff)
#define ROUND_PG_UP(x) (((uintptr_t)(x) + 0x3fff) & ~(uintptr_t)0x3fff)

int parse_offset_hex_checked(const char *s, uint64_t *out);
int parse_hex_bytes_checked(const char *s, uint8_t *out, size_t out_cap, size_t *out_len);

int read_process_memory(pid_t pid, intptr_t addr, uint8_t *out, size_t len);

typedef enum {
    CR_ADDR_RESOLVE_OK_VERIFIED        =  0, /* expected was reliable and one candidate matched */
    CR_ADDR_RESOLVE_OK_UNVERIFIED_LEGACY    =  1, /* no reliable expected; legacy heuristic used */
    CR_ADDR_RESOLVE_OK_UNVERIFIED_ABSOLUTE  =  2, /* no reliable expected; absolute forced */
    CR_ADDR_RESOLVE_OK_UNVERIFIED_RELATIVE  =  3, /* no reliable expected; relative forced */
    CR_ADDR_RESOLVE_AMBIGUOUS          =  4, /* both candidates matched; relative preferred */
    CR_ADDR_RESOLVE_OK_X86_PROBE       =  5, /* no reliable expected; x86 instruction heuristic picked winner */
    CR_ADDR_RESOLVE_OK_OFFBYTES_PROBE  =  6, /* no reliable expected; ValueOff byte-exact match picked winner */
    CR_ADDR_RESOLVE_UNRESOLVED         = -1, /* expected was reliable but neither candidate matched */
    CR_ADDR_RESOLVE_BLOCKED_NO_BASELINE= -2  /* no reliable expected and policy=block */
} cr_addr_resolve_status_t;

typedef enum {
    CR_ADDR_FALLBACK_BLOCK    = 0, /* no baseline → block the write */
    CR_ADDR_FALLBACK_LEGACY,       /* off >= 0x200000 → abs, else → rel */
    CR_ADDR_FALLBACK_ABSOLUTE,     /* no baseline → always use abs */
    CR_ADDR_FALLBACK_RELATIVE      /* no baseline → always use rel */
} cr_addr_fallback_policy_t;

/* Legacy wrapper: expected_reliable inferred from (expect_b != NULL); fallback=BLOCK; silent=(resolve_status==NULL). */
intptr_t cheat_resolve_write_addr(pid_t pid, intptr_t base, uint64_t off_u,
                                  int abs_flag, int is_non_json, int auto_detect,
                                  const uint8_t *on_b, size_t byte_len,
                                  const uint8_t *expect_b,
                                  cr_addr_resolve_status_t *resolve_status);

/* Full version: expected_reliable=0 triggers fallback_policy instead of unresolved.
 * silent=1 suppresses warn logs (for scan/list/validate contexts). */
intptr_t cheat_resolve_write_addr_ex(pid_t pid, intptr_t base, uint64_t off_u,
                                     int abs_flag, int is_non_json, int auto_detect,
                                     const uint8_t *on_b, size_t byte_len,
                                     const uint8_t *expect_b, int expected_reliable,
                                     cr_addr_fallback_policy_t fallback_policy,
                                     cr_addr_resolve_status_t *resolve_status,
                                     int silent);

void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size);
void get_cheat_addr_flags(int kind, int entry_abs_flag, int auto_detect,
                          int *abs_flag_out, int *is_non_json_out, int *auto_detect_out);

/* Returns 0 on success, sets *out_base to the module's map base.
 * If module_name is NULL or empty, *out_base = eboot_base.
 * Returns -1 if the named module is not loaded (caller should surface "module_not_loaded" error). */
int resolve_module_base(pid_t pid, const char *module_name, intptr_t eboot_base, intptr_t *out_base);

/* Returns 1 if the process has libScePs2EmuMenuDialog.sprx loaded (PS2 emulation mode). */
int process_is_ps2_emu(pid_t pid);

/* Code-cave write: allocates an anonymous page at the target address via pt_mmap MAP_FIXED,
 * preserves surrounding code, writes data, then mprotects RX.
 * Use when write_process_memory_forced fails and codecave config is enabled. */
int write_via_codecave(pid_t pid, intptr_t addr, const uint8_t *data, size_t len);

/* Forced write: skips kernel_get_vmem_protection, uses RWX→write→RX directly.
 * Safe for all games including PS4 BC and PS5 games with special vmem entry types that
 * cause kernel_get_vmem_protection to panic. Always restores page to PROT_READ|PROT_EXEC. */
int write_process_memory_forced(pid_t pid, intptr_t addr, const uint8_t *data, size_t len);


#endif
