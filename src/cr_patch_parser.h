#ifndef CR_PATCH_PARSER_H
#define CR_PATCH_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PATCH_MAX_ENTRIES        64
#define PATCH_MAX_LINES         640  /* raised from 64; largest patch in repo has 569 lines */
#define PATCH_LINE_MAX_BYTES    256
#define PATCH_MASK_MAX_BYTES    128  /* max pattern length for mask scan */
#define PATCH_NAME_DIR_MAX     4096  /* index slots */
#define PATCH_UNSUPPORTED_MAX     8  /* unique unsupported type names per entry */
#define PATCH_ENTRY_ID_LEN       10  /* 8 hex chars + null */

typedef enum {
    CR_PATCH_SOURCE_CHEATRUNNER_PS5 = 0,  /* xml_prospero */
    CR_PATCH_SOURCE_CHEATRUNNER_XML = 1,  /* xml          */
    CR_PATCH_SOURCE_ELF_ARSENAL_XML = 2   /* /data/elf-arsenal/ tree */
} cr_patch_source_kind_t;

typedef enum {
    PATCH_LINE_UNKNOWN     = 0,
    PATCH_LINE_BYTES       = 1,  /* raw hex bytes  */
    PATCH_LINE_BYTES16     = 2,
    PATCH_LINE_BYTES32     = 3,
    PATCH_LINE_BYTES64     = 4,
    PATCH_LINE_FLOAT32     = 5,
    PATCH_LINE_UTF8        = 6,
    PATCH_LINE_UTF16       = 7,
    PATCH_LINE_MASK        = 8,  /* byte-pattern scan */
    PATCH_LINE_MASK_JUMP32 = 9,  /* unsupported */
} patch_line_type_t;

typedef struct {
    patch_line_type_t type;
    /* Addressable types (bytes, float, …) */
    intptr_t  offset;                      /* relative to image base */
    uint8_t   value[PATCH_LINE_MAX_BYTES];
    size_t    value_len;
    /* Mask-scan type */
    uint8_t   pattern[PATCH_MASK_MAX_BYTES];
    uint8_t   wildcard[PATCH_MASK_MAX_BYTES]; /* 1 = '??' byte */
    size_t    pattern_len;
    int       match_offset;  /* byte offset from pattern match start to write site */
} patch_line_t;

typedef struct {
    char         name[128];
    char         author[128];
    char         note[512];
    char         app_ver[32];
    int          is_mask_ver;   /* app_ver == "mask" */
    patch_line_t lines[PATCH_MAX_LINES];
    int          line_count;    /* supported lines only */
    int          has_unsupported;
    int          unsupported_count;
    char         unsupported_types[PATCH_UNSUPPORTED_MAX][32];
    int          unsupported_type_count;
    int          is_absolute_addr; /* PS4: offsets are absolute VAs, do NOT add image base */
    /* Source metadata — stamped by parser */
    char                   source_path[384];
    cr_patch_source_kind_t source_kind;
    int                    metadata_index;
    char                   entry_id[PATCH_ENTRY_ID_LEN];
} patch_entry_t;

typedef struct {
    char         game_title[128];
    patch_entry_t entries[PATCH_MAX_ENTRIES];
    int          count;
} patch_doc_t;

typedef struct {
    int   ok;
    int   rolled_back;
    int   rollback_errors;
    int   verify_fail_count;
    pid_t pid;   /* pid of the process that was patched; 0 if apply failed before attach */
    char  error[64];
    char  message[256];
} patch_apply_result_t;

/* Returns 1 when a mask line has trailing original pattern bytes not covered by the write.
 * Condition: type==MASK && match_offset>=0 && match_offset+value_len < pattern_len */
int cr_patch_line_has_trailing_pattern_bytes(const patch_line_t *ln);

/* Computes a stable 8-hex-char entryId from source metadata; call after those fields are populated. */
void cr_patch_compute_entry_id(patch_entry_t *entry);

/* Finds all XML files for a title ID; order is xml_prospero, xml, external, alphabetical within each. */
int patch_find_xmls_for_title(const char *title_id,
                              char out_paths[][384], cr_patch_source_kind_t out_kinds[],
                              int max);

/* Compatibility shim: returns first matching XML path only. */
int patch_find_xml_for_title(const char *title_id, char *out_path, size_t out_sz);

/* Parses xml_path for entries referencing title_id; returns count found, -1 on hard error. */
int patch_parse_xml_file(const char *xml_path, const char *title_id,
                         cr_patch_source_kind_t source_kind, patch_doc_t *doc);

/* Applies all lines in one entry with backup/rollback (all-or-nothing); result may be NULL. */
int patch_apply_entry_ex(const char *title_id, const patch_entry_t *entry,
                         patch_apply_result_t *result);

/* Applied-state tracking (per session, keyed on pid + titleId + entryId) */
void patch_mark_applied(const char *title_id, const char *entry_id, pid_t pid);
void patch_mark_unapplied(const char *title_id, const char *entry_id, pid_t pid);
int  patch_is_applied(const char *title_id, const char *entry_id, pid_t pid);
void patch_clear_for_pid(pid_t pid);    /* called by game monitor on game exit */
void patch_clear_all_applied(void);     /* called on rescan */

/* Backup store of original bytes captured at apply time, used to restore on game stop. */
int  patch_restore_entry(const char *title_id, const char *entry_id, pid_t pid,
                         patch_apply_result_t *result);
void patch_restore_all_for_pid(pid_t pid, const char *title_id);
void patch_clear_backups_for_pid(pid_t pid);

/* Force a rescan of the patch directories on next lookup; also clears applied state. */
void patch_index_invalidate(void);

/* Global enable/disable: rename all live patch dirs to <dir>.off (off) or back (on).
 * patch_global_enabled() returns 1 if at least one live patch directory exists. */
int  patch_global_enabled(void);
void patch_global_set(int on);

/* List/toggle/delete patch XML files; dir_idx: 0=xml_prospero, 1=xml, 2=external. Toggle renames .xml <-> .xml.off. */
char *patch_files_list_json(void);
int   patch_file_toggle(const char *name, int on, int dir_idx);
int   patch_file_delete(const char *name, int dir_idx);

#endif /* CR_PATCH_PARSER_H */
