CC         ?= cc
CFLAGS     ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS   += -D_XOPEN_SOURCE=700 -Ishared
PREFIX     ?= /usr/local
AKERN_HOME ?= $(HOME)/.akern

BUILD_ROOT = build
ASAN_ROOT  = build/asan
SANITIZE   = -fsanitize=address,undefined -fno-omit-frame-pointer
OBJ        = $(BUILD_ROOT)/obj
BIN        = $(BUILD_ROOT)/bin
SYSTEM     = $(BUILD_ROOT)/system

SHARED     = $(OBJ)/shared/json.o $(OBJ)/shared/bytes.o
KERNEL     = $(patsubst %,$(OBJ)/akern/%.o,main loop input turns calls tools inbox proc session output message) $(SHARED)
FILE_TOOLS = $(BIN)/akern-edit
PROVIDERS  = $(patsubst %,$(SYSTEM)/providers/%,mock anthropic claude-code openai)
PROTOCOL   = $(OBJ)/providers/provider.o $(SHARED)
ANTHROPIC  = $(OBJ)/providers/anthropic_messages.o $(OBJ)/providers/media.o $(PROTOCOL)
HTTP       = $(OBJ)/providers/http.o $(PROTOCOL)
SCRIPTS    = $(patsubst tools/%,$(BIN)/%,$(filter-out %.c,$(wildcard tools/akern-*)))
SKILLS     = $(patsubst %,$(SYSTEM)/%,$(wildcard skills/*/*))
DATA       = $(SKILLS) $(SYSTEM)/render.jq $(SYSTEM)/system-prompt $(SYSTEM)/base-prompt.md $(SYSTEM)/tools.json $(SYSTEM)/README.md
PROGRAMS   = $(BIN)/akern $(FILE_TOOLS)
VENV       = build/venv
PYTEST     = $(VENV)/bin/pytest

LINK = @mkdir -p $(@D) && $(CC) $(CFLAGS) $(LDFLAGS) -o $@
COPY = @mkdir -p $(@D) && cp -p $< $@
NOT_BUILD = @[ "$$(cd $(AKERN_HOME) 2>/dev/null && pwd -P)" != "$$(cd $(BUILD_ROOT) && pwd -P)" ] || \
	{ echo "AKERN_HOME is the build directory: $(AKERN_HOME)" >&2; exit 1; }

all: $(PROGRAMS) $(PROVIDERS) $(SCRIPTS) $(DATA)

$(BIN)/akern: $(KERNEL)
	$(LINK) $^ -lm

$(FILE_TOOLS): $(BIN)/%: $(OBJ)/tools/%.o
	$(LINK) $^

$(SYSTEM)/providers/mock: $(OBJ)/providers/mock.o $(PROTOCOL)
	$(LINK) $^

$(SYSTEM)/providers/anthropic: $(OBJ)/providers/anthropic.o $(ANTHROPIC) $(HTTP)
	$(LINK) $^ -lcurl

$(SYSTEM)/providers/claude-code: $(OBJ)/providers/claude-code.o $(ANTHROPIC)
	$(LINK) $^

$(SYSTEM)/providers/openai: $(OBJ)/providers/openai.o $(OBJ)/providers/media.o $(HTTP)
	$(LINK) $^ -lcurl

$(patsubst %.c,$(OBJ)/%.o,$(wildcard akern/*.c)): $(wildcard akern/*.h)

$(patsubst %.c,$(OBJ)/%.o,$(wildcard providers/*.c)): $(wildcard providers/*.h)

$(OBJ)/%.o: %.c shared/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(SCRIPTS): $(BIN)/%: tools/%
	$(COPY)

$(SKILLS): $(SYSTEM)/%: %
	$(COPY)

$(SYSTEM)/render.jq: tools/render.jq
	$(COPY)

$(SYSTEM)/system-prompt: tools/system-prompt
	$(COPY)

$(SYSTEM)/base-prompt.md: tools/base-prompt.md
	$(COPY)

# The shell tool runs bash; where there is none, sh, and its description says so.
$(SYSTEM)/tools.json: tools/tools.json
	@mkdir -p $(@D)
	@if command -v bash >/dev/null 2>&1; then cp -p $< $@; \
	else sed -e 's/"program": "bash"/"program": "sh"/' -e 's/Run a command with bash /Run a command with sh /' $< > $@; \
	echo "no bash on PATH: $@ runs sh"; fi

$(SYSTEM)/README.md: home/README.md
	$(COPY)

asan:
	@rm -f $(ASAN_ROOT)/sanitizer.*
	ASAN_OPTIONS=detect_leaks=0:log_path=$(abspath $(ASAN_ROOT))/sanitizer \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:log_path=$(abspath $(ASAN_ROOT))/sanitizer \
	$(MAKE) BUILD_ROOT=$(ASAN_ROOT) CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Wpedantic $(SANITIZE)" LDFLAGS="$(SANITIZE)" test
	@! ls $(ASAN_ROOT)/sanitizer.* 2>/dev/null

$(PYTEST):
	python3 -m venv $(VENV) && $(VENV)/bin/pip install -q --disable-pip-version-check pytest

test: all $(PYTEST)
	AKERN_BUILD=$(abspath $(BUILD_ROOT)) PYTHONDONTWRITEBYTECODE=1 $(PYTEST) -q -p no:cacheprovider tests

install: all
	$(NOT_BUILD)
	mkdir -p $(PREFIX)/bin $(AKERN_HOME)
	cp -p $(PROGRAMS) $(SCRIPTS) $(PREFIX)/bin/
	rm -rf $(AKERN_HOME)/system
	cp -pR $(SYSTEM) $(AKERN_HOME)/system
	@cd $(AKERN_HOME) && for f in providers/* render.jq system-prompt base-prompt.md tools.json; do [ -e "system/$$f" ] || continue; \
	    if [ ! -L "$$f" ] && cmp -s "$$f" "system/$$f"; then rm "$$f" && echo "removed $(AKERN_HOME)/$$f: same as the built-in"; \
	    elif [ -e "$$f" ]; then echo "note: $(AKERN_HOME)/$$f overrides the built-in"; fi; done; true

uninstall:
	$(NOT_BUILD)
	rm -f $(addprefix $(PREFIX)/bin/,$(notdir $(PROGRAMS) $(SCRIPTS)))
	rm -rf $(AKERN_HOME)/system
	@if [ -d $(AKERN_HOME) ]; then rmdir $(AKERN_HOME) 2>/dev/null || echo "kept $(AKERN_HOME): files of yours remain"; fi

clean:
	rm -rf $(BUILD_ROOT) 2>/dev/null || rm -rf $(BUILD_ROOT)

.PHONY: all asan test install uninstall clean
