#ifndef CR_MEMORY_H
#define CR_MEMORY_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

/* Heap buffers for cheat byte strings: sized from the hex text, freed when the variable leaves scope. */
#define CR_AUTOFREE __attribute__((cleanup(cr_autofree)))
static inline void cr_autofree(void *p) { free(*(void **)p); }
static inline size_t hex_bytes_need(const char *s) { return s ? (strlen(s) + 1) / 2 + 1 : 1; }

#define ROUND_PG_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)0x3fff)
#define ROUND_PG_UP(x) (((uintptr_t)(x) + 0x3fff) & ~(uintptr_t)0x3fff)

/* Canonical x86-64 user-space limit — addresses outside this risk hitting
 * kernel space or PS5 MMIO regions. */
#define ADDR_IN_USER_RANGE(a) ((intptr_t)(a) >= 0x1000L && (intptr_t)(a) <= (intptr_t)0x7FFFFFFFFFFFL)

int parse_offset_hex_checked(const char *s, uint64_t *out);
int parse_hex_bytes_checked(const char *s, uint8_t *out, size_t out_cap, size_t *out_len);

int read_process_memory(pid_t pid, intptr_t addr, uint8_t *out, size_t len);

/* Relative (base + off) unless abs_flag is set. expect_b (may be NULL) lets an
 * absolute-authored offset be spotted when only the absolute address matches. */
intptr_t cheat_resolve_addr(pid_t pid, intptr_t base, uint64_t off_u, int abs_flag,
                            const uint8_t *on_b, const uint8_t *expect_b, size_t byte_len);

void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size);

/* Returns 0 and sets *out_base to the module's map base (the eboot base if module_name is empty).
 * Returns -1 if the module is not loaded (callers report "module_not_loaded"). */
struct cJSON;

/* Module handle index from an entry's "section" (a number, or number-like text); 0 when absent or invalid. */
int cheat_entry_section(const struct cJSON *entry);

/* Start of region `index` in the process VM map, only if `off` falls inside it; 0 otherwise. */
intptr_t vmmap_region_base(pid_t pid, int index, uint64_t off);

/* Base for one memory entry: the "section" as a dynlib handle, else as a VM map region index, else mod_base. */
intptr_t cheat_entry_base(pid_t pid, const struct cJSON *entry, intptr_t mod_base, uint64_t off);

int resolve_module_base(pid_t pid, const char *module_name, intptr_t eboot_base, intptr_t *out_base);

/* Returns 1 if the process has libScePs2EmuMenuDialog.sprx loaded (PS2 emulation mode). */
int process_is_ps2_emu(pid_t pid);

/* Forced write via mdbg — bypasses page protection, never touches it, no ptrace. */
int write_process_memory_forced(pid_t pid, intptr_t addr, const uint8_t *data, size_t len);


#endif
