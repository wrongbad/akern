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
SYSTEM    = $(ROOT)/system

KERNEL     = $(patsubst %,$(OBJ)/src/%.o,main loop driver calls files inbox proc ctl sink session msg json base)
SHARED     = $(OBJ)/src/json.o $(OBJ)/src/base.o
FILE_TOOLS = $(BIN)/akern-edit
PROVIDERS  = $(patsubst providers/%.c,$(SYSTEM)/providers/%,$(wildcard providers/*.c))
PROTOCOL   = $(OBJ)/providers/lib/provider.o $(SHARED)
MESSAGES   = $(OBJ)/providers/lib/messages.o $(OBJ)/providers/lib/media.o $(PROTOCOL)
HTTP       = $(OBJ)/providers/lib/http.o $(PROTOCOL)
SCRIPTS    = $(patsubst tools/%,$(BIN)/%,$(filter-out %.c,$(wildcard tools/akern-*)))
SKILLS     = $(patsubst %,$(SYSTEM)/%,$(wildcard skills/*/*))
DATA       = $(SKILLS) $(SYSTEM)/render.jq $(SYSTEM)/README.md $(SYSTEM)/system.md
PROGRAMS   = $(BIN)/akern $(FILE_TOOLS)
VENV       = build/venv
PYTEST     = $(VENV)/bin/pytest

LINK = @mkdir -p $(@D) && $(CC) $(CFLAGS) $(LDFLAGS) -o $@
COPY = @mkdir -p $(@D) && cp -p $< $@

write_system_md = list=$$(for f in $(1); do [ -f "$$f" ] || continue; \
	    name=$$(sed -n 's/^name: *//p' "$$f" | head -1); \
	    description=$$(sed -n 's/^description: *//p' "$$f" | head -1); \
	    echo "- $${name:-$$(basename "$$(dirname "$$f")")}: $$description ($$f)"; done); \
	if [ -n "$$list" ]; then printf "Skills: read a skill's file before relying on it.\n%s\n" "$$list" > "$(2)"; \
	else rm -f "$(2)"; fi

all: $(PROGRAMS) $(PROVIDERS) $(SCRIPTS) $(DATA)

$(BIN)/akern: $(KERNEL)
	$(LINK) $^ -lm

$(FILE_TOOLS): $(BIN)/%: $(OBJ)/tools/%.o
	$(LINK) $^

$(SYSTEM)/providers/mock: $(OBJ)/providers/mock.o $(PROTOCOL)
	$(LINK) $^

$(SYSTEM)/providers/anthropic: $(OBJ)/providers/anthropic.o $(MESSAGES) $(HTTP)
	$(LINK) $^ -lcurl

$(SYSTEM)/providers/claude-code: $(OBJ)/providers/claude-code.o $(MESSAGES)
	$(LINK) $^

$(SYSTEM)/providers/openai: $(OBJ)/providers/openai.o $(OBJ)/providers/lib/media.o $(HTTP)
	$(LINK) $^ -lcurl

$(patsubst %.c,$(OBJ)/%.o,$(wildcard providers/*.c providers/lib/*.c)): $(wildcard providers/lib/*.h)

$(OBJ)/%.o: %.c src/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(SCRIPTS): $(BIN)/%: tools/%
	$(COPY)

$(SKILLS): $(SYSTEM)/%: %
	$(COPY)

$(SYSTEM)/render.jq: tools/render.jq
	$(COPY)

$(SYSTEM)/README.md: home/README.md
	$(COPY)

$(SYSTEM)/system.md: $(wildcard skills) $(SKILLS)
	@$(call write_system_md,$(abspath $(filter %/SKILL.md,$^)),$@)

asan:
	@rm -f $(ASAN_ROOT)/sanitizer.*
	ASAN_OPTIONS=detect_leaks=0:log_path=$(abspath $(ASAN_ROOT))/sanitizer \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:log_path=$(abspath $(ASAN_ROOT))/sanitizer \
	$(MAKE) ROOT=$(ASAN_ROOT) CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Wpedantic $(SANITIZE)" LDFLAGS="$(SANITIZE)" test
	@! ls $(ASAN_ROOT)/sanitizer.* 2>/dev/null

$(PYTEST):
	python3 -m venv $(VENV) && $(VENV)/bin/pip install -q --disable-pip-version-check pytest

test: all $(PYTEST)
	AKERN_BUILD=$(abspath $(ROOT)) PYTHONDONTWRITEBYTECODE=1 $(PYTEST) -q -p no:cacheprovider tests

lines:
	@wc -l $(patsubst $(OBJ)/%.o,%.c,$(KERNEL)) src/*.h | tail -1

install: all
	mkdir -p $(PREFIX)/bin $(HOME_DIR)
	cp -p $(PROGRAMS) $(SCRIPTS) $(PREFIX)/bin/
	rm -rf $(HOME_DIR)/system
	cp -pR $(SYSTEM) $(HOME_DIR)/system
	@$(call write_system_md,"$(HOME_DIR)"/system/skills/*/SKILL.md,$(HOME_DIR)/system/system.md)
	@cd $(HOME_DIR) && for f in providers/* render.jq system.md; do [ -e "system/$$f" ] || continue; \
	    if [ ! -L "$$f" ] && cmp -s "$$f" "system/$$f"; then rm "$$f" && echo "removed $(HOME_DIR)/$$f: same as the built-in"; \
	    elif [ -e "$$f" ]; then echo "note: $(HOME_DIR)/$$f overrides the built-in"; fi; done; true

clean:
	rm -rf $(ROOT) 2>/dev/null || rm -rf $(ROOT)

.PHONY: all asan test lines install clean
