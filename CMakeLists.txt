cmake_minimum_required(VERSION 3.10)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(CheatRunnerPayload C)
set(CHEATRUNNER_VERSION "0.1")

# Embed CheatRunner.png as a C byte array (cheatrunner_png.h in build dir)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/CheatRunner.png" _CR_PNG_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _CR_PNG_BYTES "${_CR_PNG_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cheatrunner_png.h"
  "static const unsigned char g_cheatrunner_png[] = {${_CR_PNG_BYTES}};\n"
)

add_executable(CheatRunner
  src/main.c
  src/cr_log.c
  src/cr_paths.c
  src/cr_config.c
  src/cr_notifications.c
  src/cr_activity.c
  src/cr_json.c
  src/cr_titles.c
  src/cr_appdb.c
  src/cr_game_monitor.c
  src/cr_launch.c
  src/cr_memory.c
  src/cr_cheat_formats.c
  src/cr_cheats.c
  src/cr_remote_sources.c
  src/cr_source_jobs.c
  src/cr_api.c
  src/cr_api_dashboard.c
  src/cr_api_games.c
  src/cr_api_cheats.c
  src/cr_api_sources.c
  src/cr_api_logs.c
  src/cr_api_dev.c
  src/cr_repo_mirror.c
  src/cr_browser.c
  src/cr_http.c
  src/cr_shutdown.c
  src/jb.c
  src/priv_bootstrap.c
  src/http_client.c
  src/pt.c
  src/third_party/cJSON.c
  src/third_party/mc4/aes.c
  src/third_party/mc4/base64.c
  src/third_party/mc4/mc4decrypter.c
)

set_target_properties(CheatRunner PROPERTIES
  OUTPUT_NAME CheatRunner.elf
)

target_include_directories(CheatRunner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(CheatRunner PRIVATE -Wall -Wextra -O2)
target_compile_definitions(CheatRunner PRIVATE CHEATRUNNER_VERSION="${CHEATRUNNER_VERSION}")
target_link_options(CheatRunner PRIVATE "-Wl,--allow-multiple-definition")
target_compile_definitions(CheatRunner PRIVATE CHEATRUNNER_HAVE_BROWSER_OPEN=1)

set(CHEATRUNNER_LIBS
  kernel_sys
  SceSystemService
  SceUserService
  SceHttp
  SceSsl
  SceNet
  pthread
)

# ps5-payload-sdk exposes sceLncUtil* through libSceSystemService.
# Do not link -lSceLncUtil: many SDK bundles do not ship a separate library,
# but libSceSystemService contains the needed stubs.
target_compile_definitions(CheatRunner PRIVATE
  CHEATRUNNER_HAVE_SCE_LNCUTIL=1
  CHEATRUNNER_HAVE_SCE_USER_LIST=1
)
message(STATUS "Using sceLncUtil* symbols from SceSystemService")
message(STATUS "sceUserServiceGetLoginUserIdList: enabled")

# SQLite-backed app.db scanner (primary game list, falls back to folder scan).
#
# The ps5-payload-sdk does not ship libsqlite3. To enable:
#   1. Download the amalgamation from https://sqlite.org/download.html
#      (sqlite-amalgamation-*.zip  →  sqlite3.c + sqlite3.h)
#   2. Place both files in src/third_party/
#   3. Re-run cmake with -DCHEATRUNNER_SQLITE_BUNDLED=ON
#      (or flip the option(... OFF) below to ON before configuring)
#
option(CHEATRUNNER_SQLITE_BUNDLED "Bundle sqlite3 amalgamation (src/third_party/sqlite3.c)" OFF)
if(CHEATRUNNER_SQLITE_BUNDLED)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/sqlite3.c")
    message(FATAL_ERROR "CHEATRUNNER_SQLITE_BUNDLED=ON but src/third_party/sqlite3.c not found. "
                        "Download the amalgamation from https://sqlite.org/download.html")
  endif()
  target_sources(CheatRunner PRIVATE src/third_party/sqlite3.c)
  target_compile_definitions(CheatRunner PRIVATE
    CHEATRUNNER_HAVE_SQLITE_APPDB=1
    SQLITE_THREADSAFE=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
  )
  message(STATUS "app.db scanner: SQLite bundled from src/third_party/sqlite3.c")
else()
  message(STATUS "app.db scanner: disabled (folder fallback only) — see CMakeLists.txt to enable")
endif()

target_link_libraries(CheatRunner PRIVATE ${CHEATRUNNER_LIBS})

if(CMAKE_STRIP)
  add_custom_command(TARGET CheatRunner POST_BUILD
    COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:CheatRunner>
  )
endif()
