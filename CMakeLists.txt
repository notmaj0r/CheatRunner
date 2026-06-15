cmake_minimum_required(VERSION 3.10)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(CheatRunnerPayload C)
set(CHEATRUNNER_VERSION "0.13")

# Embed CheatRunner.png as a C byte array (cheatrunner_png.h in build dir).
# If the asset is missing, generate a tiny fallback icon so builds still work.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CheatRunner.png")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/CheatRunner.png" _CR_PNG_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _CR_PNG_BYTES "${_CR_PNG_HEX}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cheatrunner_png.h"
    "static const unsigned char g_cheatrunner_png[] = {${_CR_PNG_BYTES}};\n"
  )
else()
  message(WARNING "CheatRunner.png missing; generating fallback cheatrunner_png.h")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cheatrunner_png.h"
    "static const unsigned char g_cheatrunner_png[] = {"
    "0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,"
    "0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,"
    "0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,"
    "0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,"
    "0x00,0x01,0x00,0x00,0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,"
    "0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};\n"
  )
endif()

# Embed dist/CheatRunner.pkg as a C byte array (cheatrunner_tile_pkg.h).
# Build the PKG first with: cd pkg-source && ./build-pkg.sh
# If absent the auto-install feature is compiled out but everything else works.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dist/CheatRunner.pkg")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dist/CheatRunner.pkg" _PKG_HEX HEX)
  string(LENGTH "${_PKG_HEX}" _PKG_HEX_LEN)
  math(EXPR _PKG_LEN "${_PKG_HEX_LEN} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _PKG_BYTES "${_PKG_HEX}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cheatrunner_tile_pkg.h"
    "static const unsigned char g_cheatrunner_tile_pkg[] = {${_PKG_BYTES}};\n"
    "static const size_t g_cheatrunner_tile_pkg_len = ${_PKG_LEN}u;\n"
  )
  set(_HAVE_TILE_PKG ON)
  message(STATUS "CheatRunner tile PKG: embedding ${_PKG_LEN} bytes from dist/CheatRunner.pkg")
else()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cheatrunner_tile_pkg.h"
    "/* dist/CheatRunner.pkg not present at configure time — auto-install disabled */\n"
  )
  set(_HAVE_TILE_PKG OFF)
  message(STATUS "CheatRunner tile PKG: dist/CheatRunner.pkg not found — auto-install disabled (run pkg-source/build-pkg.sh first)")
endif()

file(GLOB CHEATRUNNER_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(CheatRunner
  ${CHEATRUNNER_SRC}
  src/third_party/cJSON.c
  src/third_party/stb_impl.c
  src/third_party/mc4/aes.c
  src/third_party/mc4/base64.c
  src/third_party/mc4/mc4decrypter.c
)

set_target_properties(CheatRunner PROPERTIES
  OUTPUT_NAME CheatRunner.elf
)

target_include_directories(CheatRunner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(CheatRunner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(CheatRunner PRIVATE -Wall -Wextra -O2)
target_compile_definitions(CheatRunner PRIVATE CHEATRUNNER_VERSION="${CHEATRUNNER_VERSION}")
target_link_options(CheatRunner PRIVATE "-Wl,--allow-multiple-definition")
target_compile_definitions(CheatRunner PRIVATE CHEATRUNNER_HAVE_BROWSER_OPEN=1)

set(CHEATRUNNER_LIBS
  kernel_sys
  SceSystemService
  SceUserService
  SceAppInstUtil
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
if(_HAVE_TILE_PKG)
  target_compile_definitions(CheatRunner PRIVATE CHEATRUNNER_HAVE_TILE_PKG=1)
endif()
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
