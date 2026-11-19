CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -D_XOPEN_SOURCE=700 -Isrc
PREFIX   ?= /usr/local
HOME_DIR ?= $(HOME)/.akern

ROOT      = build
ASAN_ROOT = build/asan
SANITIZE  = -fsanitize=address,undefined -fno-omit-frame-pointer
OBJ       = $(ROOT)/obj
BIN       = $(ROOT)/bin

KERNEL     = $(patsubst %,$(OBJ)/src/%.o,main loop driver calls files inbox proc ctl sink session msg json base)
SHARED     = $(OBJ)/src/json.o $(OBJ)/src/base.o
FILE_TOOLS = $(BIN)/akern-edit
PROVIDERS  = $(patsubst providers/%.c,$(ROOT)/providers/%,$(wildcard providers/*.c))
SCRIPTS    = $(patsubst tools/%,$(BIN)/%,$(filter-out %.c,$(wildcard tools/akern tools/akern-*)))
DATA       = $(patsubst %,$(ROOT)/%,render.jq $(wildcard skills/*/*))
PROGRAMS   = $(BIN)/akern-loop $(FILE_TOOLS)
VENV       = build/venv
PYTEST     = $(VENV)/bin/pytest

LINK = @mkdir -p $(@D) && $(CC) $(CFLAGS) $(LDFLAGS) -o $@

write_system_md = list=$$(for f in $(1); do [ -f "$$f" ] || continue; \
	    name=$$(sed -n 's/^name: *//p' "$$f" | head -1); \
	    description=$$(sed -n 's/^description: *//p' "$$f" | head -1); \
	    echo "- $${name:-$$(basename "$$(dirname "$$f")")}: $$description ($$f)"; done); \
	if [ -n "$$list" ]; then printf "Skills: read a skill's file before relying on it.\n%s\n" "$$list" > "$(2)"; \
	else rm -f "$(2)"; fi

all: $(PROGRAMS) $(PROVIDERS) $(SCRIPTS) $(DATA) $(ROOT)/system.md

$(BIN)/akern-loop: $(KERNEL)
	$(LINK) $^ -lm

$(FILE_TOOLS): $(BIN)/%: $(OBJ)/tools/%.o
	$(LINK) $^

$(ROOT)/providers/mock: $(OBJ)/providers/mock.o $(SHARED)
	$(LINK) $^

$(ROOT)/providers/anthropic: $(OBJ)/providers/anthropic.o $(SHARED)
	$(LINK) $^ -lcurl

$(ROOT)/providers/claude-code: $(OBJ)/providers/claude-code.o $(SHARED)
	$(LINK) $^

$(OBJ)/%.o: %.c src/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(SCRIPTS): $(BIN)/%: tools/%
	@mkdir -p $(@D) && cp -p $< $@

vpath render.jq tools

$(DATA): $(ROOT)/%: %
	@mkdir -p $(@D) && cp -p $< $@

$(ROOT)/system.md: $(wildcard skills) $(filter $(ROOT)/skills/%,$(DATA))
	@$(call write_system_md,$(abspath $(filter %/SKILL.md,$^)),$@)

asan:
	@rm -f $(ASAN_ROOT)/sanitizer.*
	ASAN_OPTIONS=detect_leaks=0:log_path=$(abspath $(ASAN_ROOT))/sanitizer \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:log_path=$(abspath $(ASAN_ROOT))/sanitizer \
	$(MAKE) ROOT=$(ASAN_ROOT) CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Wpedantic $(SANITIZE)" LDFLAGS="$(SANITIZE)" smoke
	@! ls $(ASAN_ROOT)/sanitizer.* 2>/dev/null

$(PYTEST):
	python3 -m venv $(VENV) && $(VENV)/bin/pip install -q --disable-pip-version-check pytest

smoke: all $(PYTEST)
	AKERN_BUILD=$(abspath $(ROOT)) PYTHONDONTWRITEBYTECODE=1 $(PYTEST) -q -p no:cacheprovider tests

lines:
	@wc -l $(patsubst $(OBJ)/%.o,%.c,$(KERNEL)) src/*.h | tail -1

install: all
	mkdir -p $(PREFIX)/bin
	cp -p $(PROGRAMS) $(SCRIPTS) $(PREFIX)/bin/
	mkdir -p $(HOME_DIR)/providers
	cp -p $(PROVIDERS) $(HOME_DIR)/providers/
	@for f in $(patsubst $(ROOT)/%,%,$(DATA)); do \
	    mkdir -p $(HOME_DIR)/$$(dirname $$f); \
	    [ -e $(HOME_DIR)/$$f ] || cp -p $(ROOT)/$$f $(HOME_DIR)/$$f; done
	@[ -e $(HOME_DIR)/system.md ] || { $(call write_system_md,"$(HOME_DIR)"/skills/*/SKILL.md,$(HOME_DIR)/system.md); }

clean:
	rm -rf $(ROOT) 2>/dev/null || rm -rf $(ROOT)

.PHONY: all asan smoke lines install clean
