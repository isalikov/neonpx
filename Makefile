APP      := neonpx
VERSION  ?= 1.0.1
ifeq ($(strip $(VERSION)),)
VERSION  := 0.0.0-dev
endif
PREFIX   ?= $(HOME)/.local
BUILD    ?= build

UNAME_S  := $(shell uname -s)
UNAME_M  := $(shell uname -m)

# ---------------------------------------------------------------- flags

CSTD     := -std=c11
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
            -Wformat=2 -Wvla -Wcast-qual -Wpointer-arith -Wundef -Wno-format-nonliteral
CPPFLAGS += -Isrc -I$(BUILD) -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS   += -lpthread -lm

DBG_FLAGS := -O0 -g3 -fno-omit-frame-pointer
ifneq ($(SAN),0)
DBG_FLAGS += -fsanitize=address,undefined
endif
REL_FLAGS := -O2 -DNDEBUG -flto -fstack-protector-strong
REL_LDFLAGS := -flto

ifeq ($(UNAME_S),Darwin)
PLATFORM := macos
REL_FLAGS += -mmacosx-version-min=11.0
REL_LDFLAGS += -mmacosx-version-min=11.0
ifeq ($(UNIVERSAL),1)
ARCHFLAGS := -arch arm64 -arch x86_64
ARCH := universal
endif
else
PLATFORM := linux
REL_FLAGS += -D_FORTIFY_SOURCE=2
REL_LDFLAGS += -flto=auto
endif

ifeq ($(STATIC),1)
REL_LDFLAGS += -static
endif

ARCH ?= $(if $(filter arm64 aarch64,$(UNAME_M)),aarch64,$(UNAME_M))
DIST := dist/$(APP)-$(PLATFORM)-$(ARCH).tar.gz

# ---------------------------------------------------------------- sources

SRC      := $(sort $(wildcard src/*.c))
LIB_SRC  := $(filter-out src/main.c,$(SRC))
TEST_SRC := $(sort $(wildcard tests/*.c))

DBG_DIR  := $(BUILD)/debug
REL_DIR  := $(BUILD)/release
TST_DIR  := $(BUILD)/test

DBG_OBJ  := $(SRC:src/%.c=$(DBG_DIR)/%.o)
REL_OBJ  := $(SRC:src/%.c=$(REL_DIR)/%.o)
TST_OBJ  := $(LIB_SRC:src/%.c=$(TST_DIR)/%.o) $(TEST_SRC:tests/%.c=$(TST_DIR)/tests/%.o)

VERSION_H := $(BUILD)/version.h

FMT_FILES := $(SRC) $(wildcard src/*.h) $(TEST_SRC) $(wildcard tests/*.h)

.DEFAULT_GOAL := help
.PHONY: help build release run test check fmt fmt-check lint install uninstall dist linux clean FORCE

help:
	@printf "%s\n" \
		"NeonPX $(VERSION) targets:" \
		"  make build     - debug build with ASan/UBSan (SAN=0 to disable) -> $(DBG_DIR)/$(APP)" \
		"  make run       - build and run the debug binary (ARGS='--all')" \
		"  make release   - optimized, stripped binary -> $(REL_DIR)/$(APP)" \
		"                   UNIVERSAL=1 (macOS arm64+x86_64), STATIC=1 (Linux/musl)" \
		"  make test      - unit tests under sanitizers" \
		"  make check     - -Werror build + tests + clang-format/clang-tidy/cppcheck when installed" \
		"  make fmt       - clang-format the tree" \
		"  make install   - install the release binary to PREFIX=$(PREFIX)" \
		"  make uninstall - remove it" \
		"  make dist      - release tarball -> $(DIST)" \
		"  make linux     - static Linux binary via Docker (alpine/musl), for testing from macOS" \
		"  make clean     - remove build artifacts"

# ---------------------------------------------------------------- version

$(VERSION_H): FORCE
	@mkdir -p $(@D)
	@printf '#define NEONPX_VERSION "%s"\n' '$(VERSION)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# ---------------------------------------------------------------- debug

build: $(DBG_DIR)/$(APP)

$(DBG_DIR)/$(APP): $(DBG_OBJ)
	$(CC) $(DBG_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(DBG_DIR)/%.o: src/%.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(CPPFLAGS) $(DBG_FLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

run: build
	./$(DBG_DIR)/$(APP) $(ARGS)

# ---------------------------------------------------------------- release

release: $(REL_DIR)/$(APP)

$(REL_DIR)/$(APP): $(REL_OBJ)
	$(CC) $(ARCHFLAGS) $(REL_FLAGS) $(REL_LDFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
	strip $(if $(filter Darwin,$(UNAME_S)),-x,) $@

$(REL_DIR)/%.o: src/%.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(CPPFLAGS) $(ARCHFLAGS) $(REL_FLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# ---------------------------------------------------------------- tests

test: $(TST_DIR)/$(APP)-test
	./$(TST_DIR)/$(APP)-test

$(TST_DIR)/$(APP)-test: $(TST_OBJ)
	$(CC) $(DBG_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TST_DIR)/%.o: src/%.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(CPPFLAGS) $(DBG_FLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(TST_DIR)/tests/%.o: tests/%.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(CPPFLAGS) $(DBG_FLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# ---------------------------------------------------------------- quality

check:
	$(MAKE) --no-print-directory BUILD=$(BUILD)/check CFLAGS="$(CFLAGS) -Werror" build test
	@$(MAKE) --no-print-directory fmt-check lint

fmt-check:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format --dry-run --Werror $(FMT_FILES) && echo "clang-format: ok"; \
	else echo "clang-format: not installed, skipped"; fi

lint:
	@if command -v clang-tidy >/dev/null 2>&1; then \
		clang-tidy --quiet $(SRC) -- $(CSTD) $(CPPFLAGS) -I$(BUILD)/check && echo "clang-tidy: ok"; \
	else echo "clang-tidy: not installed, skipped"; fi
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --quiet --error-exitcode=1 --enable=warning,performance,portability \
			--suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches -Isrc src && echo "cppcheck: ok"; \
	else echo "cppcheck: not installed, skipped"; fi

fmt:
	clang-format -i $(FMT_FILES)

# ---------------------------------------------------------------- install / dist

install: release
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(REL_DIR)/$(APP) $(DESTDIR)$(PREFIX)/bin/$(APP)
	@echo "installed $(DESTDIR)$(PREFIX)/bin/$(APP)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)

dist: release
	@mkdir -p dist
	cp $(REL_DIR)/$(APP) dist/$(APP)
	tar -czf $(DIST) -C dist $(APP)
	rm -f dist/$(APP)
	@echo "packed $(DIST)"

linux:
	docker run --rm -v "$(CURDIR)":/src -w /src alpine:3.20 sh -c \
		'apk add --no-cache build-base >/dev/null && make release STATIC=1 BUILD=build/linux VERSION=$(VERSION)'
	@file build/linux/release/$(APP) 2>/dev/null || true

clean:
	rm -rf $(BUILD) dist

-include $(DBG_OBJ:.o=.d) $(REL_OBJ:.o=.d) $(TST_OBJ:.o=.d)
