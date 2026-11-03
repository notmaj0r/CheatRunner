# Standalone Makefile for CheatRunner.elf — compiles directly with the PS5
# payload SDK toolchain.
#
# Usage:
#   make                 # build build-make/CheatRunner.elf
#   make clean
#   make test            # host-side tests
#   make SQLITE=1        # force-enable bundled sqlite3 (auto-detected otherwise)
#   make SQLITE=0        # force-disable it
#   PS5_PAYLOAD_SDK=/path/to/sdk make   # only used when /opt/ps5-payload-sdk is missing

VERSION := 0.18.1

BUILD_DIR := build-make
GENDIR    := $(BUILD_DIR)/gen
OBJDIR    := $(BUILD_DIR)/obj
ELF       := $(BUILD_DIR)/CheatRunner.elf

# ---------------------------------------------------------------- SDK resolve
PS5_PAYLOAD_SDK ?=
SDK_ROOT := $(patsubst %/toolchain/prospero.cmake,%,$(firstword \
  $(wildcard /opt/ps5-payload-sdk/toolchain/prospero.cmake) \
  $(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.cmake) \
  $(wildcard PS5-Projects/sdk-master/toolchain/prospero.cmake) \
  $(wildcard ps5-payload-sdk/toolchain/prospero.cmake)))

ifeq ($(SDK_ROOT),)
$(error PS5 payload SDK not found. Install it at /opt/ps5-payload-sdk, or set PS5_PAYLOAD_SDK)
endif

CC     := $(SDK_ROOT)/bin/prospero-clang
CXX    := $(SDK_ROOT)/bin/prospero-clang++
STRIP  := $(SDK_ROOT)/bin/prospero-strip

# ---------------------------------------------------------------- sources
SRC_C := $(wildcard src/*.c) \
  src/third_party/cJSON.c \
  src/third_party/stb_impl.c \
  src/third_party/mc4/aes.c \
  src/third_party/mc4/base64.c \
  src/third_party/mc4/mc4decrypter.c \
  src/third_party/miniz.c
SRC_CPP := $(wildcard src/*.cpp)

SQLITE ?= $(if $(and $(wildcard src/third_party/sqlite3.c),$(wildcard src/third_party/sqlite3.h)),1,0)
ifeq ($(SQLITE),1)
SRC_C += src/third_party/sqlite3.c
endif

OBJ_C   := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC_C))
OBJ_CPP := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SRC_CPP))
OBJS    := $(OBJ_C) $(OBJ_CPP)

# ---------------------------------------------------------------- generated headers
PNG_SRC := $(wildcard CheatRunner.png)
PKG_SRC := $(wildcard dist/CheatRunner.pkg)
HAVE_TILE_PKG := $(if $(PKG_SRC),1,0)

PAYLOAD_ELF := $(BUILD_DIR)/payload/cr_getdata_payload.elf
PAYLOAD_SRC := src/payload/cr_getdata_payload.c src/payload/cr_detour.c src/payload/cr_plog.c src/payload/cr_reloc.c src/payload/hde64.c

GEN_HEADERS := $(GENDIR)/cr_getdata_payload_blob.h $(GENDIR)/cheatrunner_png.h $(GENDIR)/cheatrunner_tile_pkg.h \
  $(GENDIR)/dashboard_html_gz.h $(GENDIR)/dashboard_css_gz.h $(GENDIR)/dashboard_js_gz.h

# ---------------------------------------------------------------- flags
DEFINES := -DCHEATRUNNER_VERSION=\"$(VERSION)\" \
  -DCHEATRUNNER_HAVE_BROWSER_OPEN=1 \
  -DCHEATRUNNER_HAVE_SCE_LNCUTIL=1 \
  -DCHEATRUNNER_HAVE_SCE_USER_LIST=1
ifeq ($(HAVE_TILE_PKG),1)
DEFINES += -DCHEATRUNNER_HAVE_TILE_PKG=1
endif
ifeq ($(SQLITE),1)
DEFINES += -DCHEATRUNNER_HAVE_SQLITE_APPDB=1 -DSQLITE_THREADSAFE=0 \
  -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION
endif

INCLUDES := -I$(GENDIR) -Isrc
CFLAGS   := -fPIE -Wall -Wextra -O2 -ffunction-sections -fdata-sections $(INCLUDES) $(DEFINES)
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -Wl,--allow-multiple-definition

# Cold bundled libraries are built for size; our own code stays at -O2.
SIZE_OBJS := $(OBJDIR)/third_party/sqlite3.o $(OBJDIR)/third_party/miniz.o $(OBJDIR)/third_party/stb_impl.o
$(SIZE_OBJS): CFLAGS := $(subst -O2,-Os,$(CFLAGS))
LIBS     := -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil \
  -lSceHttp -lSceSsl -lSceNet -lpthread

# Host tests: pure logic (versions, hex, conflicts, patch XML) built with the host compiler, no console needed.
TEST_SRC := tests/test_main.c tests/host_stubs.c src/cr_version.c src/cr_sha1.c src/cr_memory.c \
  src/cr_conflict.c src/cr_patch_parser.c src/third_party/cJSON.c

.PHONY: all clean probe test
.DEFAULT_GOAL := all

test: | $(BUILD_DIR)
	cc -g -O1 -w -fsanitize=address,undefined -Isrc -Itests/stubs -o $(BUILD_DIR)/cr_tests $(TEST_SRC) -lm -lpthread
	$(BUILD_DIR)/cr_tests

all: $(ELF)

$(ELF): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(LDFLAGS) -Wl,--gc-sections -o $@ $(OBJS) $(LIBS)
	@if [ -x "$(STRIP)" ]; then $(STRIP) --strip-all $@; fi

$(OBJDIR)/cr_api.o: src/dashboard_html.inc src/dashboard_css.inc src/dashboard_js.inc

$(OBJDIR)/%.o: src/%.c $(GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: src/%.cpp $(GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Hotkey payload: built as its own PIE ELF, then embedded into CheatRunner.elf
$(PAYLOAD_ELF): $(PAYLOAD_SRC) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -fPIE -Wall -Wextra -O2 -Isrc -Isrc/payload $(LDFLAGS) -o $@ $(PAYLOAD_SRC) -lkernel_sys
	@if [ -x "$(STRIP)" ]; then $(STRIP) --strip-all $@; fi

$(GENDIR)/cr_getdata_payload_blob.h: $(PAYLOAD_ELF) tools/gen_blob_header.py | $(GENDIR)
	python3 tools/gen_blob_header.py $< $@ g_cr_getdata_payload_elf

# Short hash of the sources and assets, shown in the log so each session says which ELF ran.
.PHONY: FORCE
$(GENDIR)/build_id.h: FORCE | $(GENDIR)
	@id=$$(find src Makefile CheatRunner.png dist -type f ! -name 'dashboard_*.inc' 2>/dev/null | sort | xargs sha1sum | sha1sum | cut -c1-7); \
	line="#define CHEATRUNNER_BUILD_ID \"$$id\""; \
	[ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" > $@
$(OBJDIR)/main.o $(OBJDIR)/cr_log.o: $(GENDIR)/build_id.h

$(GENDIR)/cheatrunner_png.h: $(PNG_SRC) | $(GENDIR)
	@if [ -f CheatRunner.png ]; then \
	  python3 -c "d=open('CheatRunner.png','rb').read(); open('$@','w').write('static const unsigned char g_cheatrunner_png[] = {'+','.join(str(b) for b in d)+'};\n')"; \
	else \
	  printf '%s\n' 'static const unsigned char g_cheatrunner_png[] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};' > $@; \
	fi

$(GENDIR)/cheatrunner_tile_pkg.h: $(PKG_SRC) | $(GENDIR)
	@if [ -f dist/CheatRunner.pkg ]; then \
	  python3 -c "d=open('dist/CheatRunner.pkg','rb').read(); open('$@','w').write('static const unsigned char g_cheatrunner_tile_pkg[] = {'+','.join(str(b) for b in d)+'};\nstatic const unsigned long g_cheatrunner_tile_pkg_len = %du;\n'%len(d))"; \
	else \
	  printf '/* dist/CheatRunner.pkg not present -- auto-install disabled */\n' > $@; \
	fi

# Dashboard sources live in src/web/; the .inc files are generated from them.
src/dashboard_html.inc: src/web/index.html tools/gen_dashboard_inc.py
	python3 tools/gen_dashboard_inc.py $< $@
src/dashboard_css.inc: src/web/app.css tools/gen_dashboard_inc.py
	python3 tools/gen_dashboard_inc.py $< $@
$(GENDIR)/dashboard_i18n.js: tools/gen_i18n_js.py $(wildcard src/web/locales/*.json) $(wildcard src/web/flags/*.svg) | $(GENDIR)
	python3 tools/gen_i18n_js.py $@
$(GENDIR)/dashboard_bundle.js: src/web/qrcode.js $(GENDIR)/dashboard_i18n.js src/web/i18n.js src/web/app.js | $(GENDIR)
	cat $^ > $@
src/dashboard_js.inc: $(GENDIR)/dashboard_bundle.js tools/gen_dashboard_inc.py
	python3 tools/gen_dashboard_inc.py $< $@

$(GENDIR)/dashboard_%_gz.h: src/dashboard_%.inc tools/gen_gzip_header.py | $(GENDIR)
	python3 tools/gen_gzip_header.py $< $@ g_dashboard_$*_gz

$(GENDIR) $(BUILD_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

# Test-only probe for SceShellUI (not part of `all`).
probe: $(BUILD_DIR)/ShellUIProbe.elf
$(BUILD_DIR)/ShellUIProbe.elf: tools/shellui_probe.c src/jb.c | $(BUILD_DIR)
	$(CC) -fPIE -Wall -Wextra -O2 -Isrc $(LDFLAGS) -o $@ $^ -lkernel_sys
