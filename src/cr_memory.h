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
int write_process_memory(pid_t pid, intptr_t addr, const uint8_t *data, size_t len);

intptr_t cheat_resolve_write_addr(pid_t pid, intptr_t base, uint64_t off_u,
                                  int abs_flag, int is_non_json, int auto_detect,
                                  const uint8_t *on_b, size_t byte_len,
                                  const uint8_t *expect_b);

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
 * Use only when write_process_memory returns -3 (verify mismatch) and codecave config is enabled. */
int write_via_codecave(pid_t pid, intptr_t addr, const uint8_t *data, size_t len);

#endif
