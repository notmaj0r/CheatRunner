#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cr_remote_sources.h"

/* Only the basename dependency is needed for these host-side parser tests. */
const char *path_basename_ptr(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int main(void) {
  static const struct { const char *filename; const char *version; } cases[] = {
    {"cheats/mc4/PPSA31246_01.200.000_ef4d3664.mc4", "01.200.000"},
    {"PPSA31246_01.200.000_f70deeb5.mc4", "01.200.000"},
    {"PPSA31246_02.013.000_d471e4d7.mc4", "02.013.000"},
    {"PPSA01342_01.004.000_example.elf.shn", "01.004.000"},
    {"PPSA01342_01.004.000.shn", "01.004.000"},
    {"CUSA00001_01.01.json", "01.01"},
    {"PPSA31246_invalid_suffix.mc4", ""},
    {"PPSA31246__suffix.mc4", ""},
    {"PPSA31246.mc4", ""},
    {NULL, ""},
  };
  char version[64];
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    extract_version_from_filename(cases[i].filename, version, sizeof(version));
    assert(strcmp(version, cases[i].version) == 0);
  }
  char small[4] = "old";
  extract_version_from_filename(cases[0].filename, small, sizeof(small));
  assert(small[0] == '\0');
  char unchanged = 'x';
  extract_version_from_filename(cases[0].filename, &unchanged, 0);
  assert(unchanged == 'x');
  extract_version_from_filename(cases[0].filename, NULL, 0);

  char exact[64], other[64];
  extract_version_from_filename(cases[0].filename, exact, sizeof(exact));
  extract_version_from_filename(cases[2].filename, other, sizeof(other));
  assert(cheat_remote_match_score("01.200.000", exact) == 300);
  assert(cheat_remote_match_score("01.200.000", other) == 150);
  puts("Remote filename versions and exact-match scoring passed");
  return 0;
}
