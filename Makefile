# QitoOS - top level build
#
#   make            build the kernel, ramdisk and bootable ISO
#   make iso        the same, explicitly
#   make run        boot the ISO in QEMU
#   make run-bochs  boot the ISO in Bochs
#   make test       run the host unit tests and the boot smoke test
#   make clean      remove build artefacts
#
# The build needs only a hosted GCC/binutils toolchain and Python 3; it does
# not require a cross compiler, because the kernel is compiled freestanding
# and linked with an explicit link script.

VERSION      := 0.4a
CODENAME     := Alpha
BUILD_DATE   := $(shell date -u '+%Y-%m-%d %H:%M:%S UTC')
BUILD_ID     := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)

# --- toolchain -------------------------------------------------------------
CC      := $(shell command -v x86_64-elf-gcc 2>/dev/null || echo gcc)
LD      := $(shell command -v x86_64-elf-ld 2>/dev/null || echo ld)
OBJCOPY := $(shell command -v x86_64-elf-objcopy 2>/dev/null || echo objcopy)
PYTHON  := python3

BUILD   := build
SRC     := src

# --- flags -----------------------------------------------------------------
# Freestanding, no red zone (interrupts would clobber it), kernel code model
# so the linker can resolve the -2 GiB addresses with 32-bit relocations.
KCFLAGS := -std=gnu11 -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -fno-stack-protector -fno-stack-check -fno-pie -fno-pic \
           -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
           -mno-red-zone -mcmodel=kernel -m64 -march=x86-64 \
           -mno-mmx -mno-3dnow \
           -Wall -Wextra -Wno-unused-parameter -Wno-address-of-packed-member \
           -O2 -g \
           -I$(SRC)/kernel/include -I$(SRC) \
           -DQITO_KERNEL=1 \
           -DQITO_BUILD_DATE='"$(BUILD_DATE)"' \
           -DQITO_BUILD_ID='"$(BUILD_ID)"'

KASFLAGS := -m64 -nostdlib -ffreestanding -I$(SRC)/kernel/include

KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 --build-id=none

BOOTCFLAGS := -m32 -nostdlib -ffreestanding -fno-pie -Wall
BOOTLDFLAGS := -m elf_i386 -nostdlib --build-id=none

# --- sources ---------------------------------------------------------------
BOOT_SRCS := $(SRC)/boot/stage1.S $(SRC)/boot/stage2.S
BOOT_OBJS := $(patsubst $(SRC)/%.S,$(BUILD)/%.o,$(BOOT_SRCS))

# Generated sources.
GEN_FONT := $(SRC)/kernel/gfx/font_data.c

# The shells, desktop and applications are compiled into the kernel image:
# Qito runs them as kernel tasks rather than as separate ELF executables.
KERNEL_C_SRCS := $(shell find $(SRC)/kernel $(SRC)/lib $(SRC)/user \
                          -name '*.c' 2>/dev/null | sort)
KERNEL_C_SRCS := $(sort $(KERNEL_C_SRCS) $(GEN_FONT))
KERNEL_S_SRCS := $(shell find $(SRC)/kernel $(SRC)/user \
                          -name '*.S' 2>/dev/null | sort)

KERNEL_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(KERNEL_C_SRCS)) \
               $(patsubst $(SRC)/%.S,$(BUILD)/%.o,$(KERNEL_S_SRCS))

KERNEL_LD := $(SRC)/kernel/arch/x86_64/kernel.ld

# --- outputs ---------------------------------------------------------------
BOOT_BIN   := $(BUILD)/boot.bin
KERNEL_ELF := $(BUILD)/qito-kernel.elf
KERNEL_BIN := $(BUILD)/qito-kernel.bin
RAMDISK    := $(BUILD)/qitofs.img
ISO        := $(BUILD)/qito-os.iso

.PHONY: all iso kernel bootloader ramdisk clean run run-bochs test screenshots \
        test-unit test-boot check-tools info dirs release

all: iso

info:
	@echo "QitoOS $(VERSION) ($(CODENAME))"
	@echo "  CC        = $(CC)"
	@echo "  LD        = $(LD)"
	@echo "  build id  = $(BUILD_ID)"
	@echo "  C sources = $(words $(KERNEL_C_SRCS))"
	@echo "  S sources = $(words $(KERNEL_S_SRCS))"

check-tools:
	@command -v $(CC) >/dev/null || { echo "error: $(CC) not found"; exit 1; }
	@command -v $(LD) >/dev/null || { echo "error: $(LD) not found"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "error: python3 not found"; exit 1; }
	@echo "toolchain OK"

# --- generated font --------------------------------------------------------
$(GEN_FONT): tools/genfont.py
	@echo "  GEN   $@"
	@$(PYTHON) tools/genfont.py --output $@

# --- bootloader ------------------------------------------------------------
$(BUILD)/boot/%.o: $(SRC)/boot/%.S
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(CC) $(BOOTCFLAGS) -c $< -o $@

$(BOOT_BIN): $(BOOT_OBJS) $(SRC)/boot/boot.ld
	@echo "  LD    $@"
	@$(LD) $(BOOTLDFLAGS) -T $(SRC)/boot/boot.ld $(BOOT_OBJS) -o $@
	@$(PYTHON) tools/checkboot.py $@

bootloader: $(BOOT_BIN)

# --- kernel ----------------------------------------------------------------
$(BUILD)/%.o: $(SRC)/%.c $(GEN_FONT)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRC)/%.S
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(CC) $(KASFLAGS) -c $< -o $@

# The kernel is linked twice: once to discover where each function landed,
# then again with a generated symbol table so panics can name functions
# instead of printing bare addresses.
KSYMS_SRC := $(BUILD)/ksyms.c
KSYMS_OBJ := $(BUILD)/ksyms.o

$(KERNEL_ELF): $(KERNEL_OBJS) $(KERNEL_LD) tools/gensyms.py
	@echo "  LD    $@ (pass 1)"
	@$(PYTHON) tools/gensyms.py --input /nonexistent --output $(KSYMS_SRC) --empty >/dev/null
	@$(CC) $(KCFLAGS) -c -o $(KSYMS_OBJ) $(KSYMS_SRC)
	@$(LD) $(KLDFLAGS) -T $(KERNEL_LD) $(KERNEL_OBJS) $(KSYMS_OBJ) -o $@.tmp
	@echo "  SYMS  $(KSYMS_SRC)"
	@$(PYTHON) tools/gensyms.py --input $@.tmp --output $(KSYMS_SRC)
	@$(CC) $(KCFLAGS) -c -o $(KSYMS_OBJ) $(KSYMS_SRC)
	@echo "  LD    $@ (pass 2)"
	@$(LD) $(KLDFLAGS) -T $(KERNEL_LD) $(KERNEL_OBJS) $(KSYMS_OBJ) -o $@
	@rm -f $@.tmp

$(KERNEL_BIN): $(KERNEL_ELF)
	@echo "  BIN   $@"
	@$(OBJCOPY) -O binary $< $@

kernel: $(KERNEL_BIN)

# --- ramdisk ---------------------------------------------------------------
$(RAMDISK): $(shell find rootfs -type f 2>/dev/null) tools/mkqitofs.py
	@echo "  FS    $@"
	@mkdir -p $(BUILD)
	@$(PYTHON) tools/mkqitofs.py --root rootfs --output $@ --version $(VERSION)

ramdisk: $(RAMDISK)

# --- ISO -------------------------------------------------------------------
# Extra kernel command line options, e.g. `make iso CMDLINE_EXTRA=capture=3000`
CMDLINE ?= root=qitofs console=fb $(CMDLINE_EXTRA)

# The command line is baked into the image, so a change to it has to force the
# ISO to be rebuilt. Record it in a stamp file that the ISO depends on.
CMDLINE_STAMP := $(BUILD)/.cmdline
.PHONY: force
$(CMDLINE_STAMP): force
	@mkdir -p $(BUILD)
	@echo '$(CMDLINE)' | cmp -s - $@ || echo '$(CMDLINE)' > $@

$(ISO): $(BOOT_BIN) $(KERNEL_BIN) $(RAMDISK) $(CMDLINE_STAMP) tools/mkiso.py tools/isofs.py
	@echo "  ISO   $@"
	@$(PYTHON) tools/mkiso.py --boot $(BOOT_BIN) --kernel $(KERNEL_BIN) \
		--ramdisk $(RAMDISK) --output $@ --version $(VERSION) \
		--cmdline "$(CMDLINE)"

iso: $(ISO)

# --- running ---------------------------------------------------------------
QEMU      ?= qemu-system-x86_64
QEMU_FLAGS ?= -m 512 -serial stdio -vga std -no-reboot

run: $(ISO)
	$(QEMU) -cdrom $(ISO) $(QEMU_FLAGS)

run-nographic: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 512 -nographic -no-reboot

run-bochs: $(ISO)
	$(PYTHON) tools/runbochs.py --iso $(ISO) --interactive

# --- tests -----------------------------------------------------------------
test: test-unit test-boot

test-unit:
	@$(PYTHON) tests/run_unit_tests.py

# The boot tests need an image that exercises itself without a keyboard: it
# runs the in-kernel self-test and captures a frame, both driven by the
# kernel command line.
TEST_ISO      := $(BUILD)/qito-os-test.iso
TEST_CMDLINE  := root=qitofs console=fb echo=serial \
                 autorun=qcsh;selftest;diag;sysinfo;ush;ls_-l_/etc;calc_(7+3)*4;fonts;qti_list;copy_hi;paste;qtx_exports;qdl_list;qtpkg_list \
                 capture=9000

$(TEST_ISO): $(BOOT_BIN) $(KERNEL_BIN) $(RAMDISK) tools/mkiso.py tools/isofs.py
	@echo "  ISO   $@ (instrumented for testing)"
	@$(PYTHON) tools/mkiso.py --boot $(BOOT_BIN) --kernel $(KERNEL_BIN) \
		--ramdisk $(RAMDISK) --output $@ --version $(VERSION) \
		--cmdline "$(TEST_CMDLINE)"

test-boot: $(TEST_ISO)
	@$(PYTHON) tests/run_boot_tests.py --iso $(TEST_ISO) \
		--save-frames $(BUILD)/frames --keep-log $(BUILD)/boot-test.log

# Capture fresh screenshots for the documentation.
screenshots: $(TEST_ISO)
	@$(PYTHON) tests/capture_screenshots.py --iso $(TEST_ISO) \
		--output docs/screenshots

# --- housekeeping ----------------------------------------------------------
clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD) $(GEN_FONT)

release: iso
	@mkdir -p $(BUILD)/release
	@cp $(ISO) $(BUILD)/release/qito-os-$(VERSION).iso
	@cd $(BUILD)/release && sha256sum qito-os-$(VERSION).iso > qito-os-$(VERSION).iso.sha256
	@echo "release artefacts in $(BUILD)/release"

-include $(KERNEL_OBJS:.o=.d)
