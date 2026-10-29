KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)
BUILD_DIR ?= $(PWD)/build

.PHONY: all clean compile_commands compile_commands.json distclean
.DEFAULT_GOAL := all

all: $(BUILD_DIR)/Kbuild
	@expected=$$(uname -r); \
	actual=$$(sed -n 's/^#define UTS_RELEASE "\(.*\)"/\1/p' $(KDIR)/include/generated/utsrelease.h); \
	if [ "$$actual" != "$$expected" ]; then \
		echo "Error: KDIR is for kernel $$actual, but the running kernel is $$expected" >&2; \
		echo "Set KDIR to the matching kernel build directory." >&2; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) modules

$(BUILD_DIR)/Kbuild: main.c
	mkdir -p $(BUILD_DIR)
	printf 'obj-m += main.o\n' > $@
	ln -sf $(PWD)/main.c $(BUILD_DIR)/main.c

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -f compile_commands.json

compile_commands: compile_commands.json

compile_commands.json: clean
	@mkdir -p $(BUILD_DIR)
	@if command -v bear >/dev/null 2>&1; then \
		bear --output compile_commands.json -- $(MAKE) all; \
		ln -sf ../compile_commands.json $(BUILD_DIR)/compile_commands.json 2>/dev/null || true; \
	elif [ -f $(KDIR)/scripts/clang-tools/gen_compile_commands.py ]; then \
		$(MAKE) all; \
		python3 $(KDIR)/scripts/clang-tools/gen_compile_commands.py -d $(PWD) -o compile_commands.json $(BUILD_DIR); \
		ln -sf ../compile_commands.json $(BUILD_DIR)/compile_commands.json 2>/dev/null || true; \
	else \
		echo "Error: 'bear' is required to generate compile_commands.json. Please install it (e.g. sudo apt install bear)." >&2; \
		exit 1; \
	fi
