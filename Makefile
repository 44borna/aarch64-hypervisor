CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
GDB     := $(CROSS)gdb

QEMU    := qemu-system-aarch64
MACHINE := virt
CPU     := cortex-a72
MEM     := 512M

SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build

TARGET_ELF := $(BUILD_DIR)/hypervisor.elf
TARGET_BIN := $(BUILD_DIR)/hypervisor.bin
LINKER     := linker.ld

# Stage 8: guest Linux payload. Override GUEST_KERNEL=/path/to/Image
# to boot a Linux kernel instead of the bare-metal guest. DTB is
# always rebuilt from docs/guest.dts.
#
# Stage 8d.1: optional initramfs. Override GUEST_INITRAMFS=/path/to/file
# to load a CPIO archive at GUEST_INITRAMFS_ADDR; the DTS is regenerated
# from guest.dts.in with the initrd-start/end properties patched in.
GUEST_KERNEL       ?=
GUEST_INITRAMFS    ?=
# Expand a leading ~/ — Make leaves it literal, and QEMU's
# "-device loader,file=~/..." path doesn't go through a shell that
# would expand it either, so we do it here.
override GUEST_KERNEL    := $(patsubst ~/%,$(HOME)/%,$(GUEST_KERNEL))
override GUEST_INITRAMFS := $(patsubst ~/%,$(HOME)/%,$(GUEST_INITRAMFS))
GUEST_DTS          := docs/guest.dts
GUEST_DTS_IN       := docs/guest.dts.in
GUEST_DTB          := $(BUILD_DIR)/guest.dtb
GUEST_KERNEL_ADDR     := 0x48200000
GUEST_DTB_ADDR        := 0x4C000000
GUEST_INITRAMFS_ADDR  := 0x50000000

C_SRCS := $(wildcard $(SRC_DIR)/*.c)
S_SRCS := $(wildcard $(SRC_DIR)/*.S)
OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
          $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS))

# NVCPU selects the guest layout. 1 (default) = Linux-only single vCPU,
# 2 = dual (vCPU0 Linux + vCPU1 Stage-7 bare-metal) with CNTHP round-robin.
NVCPU ?= 1

# HOST_IF selects the virtualization target the hypervisor is built for.
#   tcg (default) = QEMU TCG + GICv2. Uses src/gic.c + src/vgic.c.
#   hvf           = macOS HVF + GICv3. Uses src/gic_v3.c + src/vgic_v3.c.
# Mapped to -DGIC_VERSION=... for the C code.
HOST_IF ?= tcg
ifeq ($(HOST_IF),tcg)
  GIC_VERSION := 2
else ifeq ($(HOST_IF),hvf)
  GIC_VERSION := 3
else
  $(error HOST_IF must be 'tcg' or 'hvf' (got '$(HOST_IF)'))
endif

CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic \
           -mgeneral-regs-only -mcpu=$(CPU) \
           -Wall -Wextra -O2 -g \
           -DNVCPU=$(NVCPU) \
           -DGIC_VERSION=$(GIC_VERSION) \
           -I$(INC_DIR)
ASFLAGS := -ffreestanding -mcpu=$(CPU) -g -I$(INC_DIR)
LDFLAGS := -nostdlib -T $(LINKER)

QEMU_FLAGS := -machine $(MACHINE),virtualization=on,gic-version=2 \
              -cpu $(CPU) -m $(MEM) -nographic \
              -kernel $(TARGET_ELF)

ifneq ($(GUEST_KERNEL),)
QEMU_FLAGS += -device loader,file=$(GUEST_KERNEL),addr=$(GUEST_KERNEL_ADDR),force-raw=on \
              -device loader,file=$(GUEST_DTB),addr=$(GUEST_DTB_ADDR)
RUN_DEPS   := $(TARGET_ELF) $(GUEST_DTB)
ifneq ($(GUEST_INITRAMFS),)
QEMU_FLAGS += -device loader,file=$(GUEST_INITRAMFS),addr=$(GUEST_INITRAMFS_ADDR),force-raw=on
INITRAMFS_BYTES := $(shell stat -f '%z' $(GUEST_INITRAMFS) 2>/dev/null || stat -c '%s' $(GUEST_INITRAMFS) 2>/dev/null)
endif
else
RUN_DEPS   := $(TARGET_ELF)
endif

# HVF launcher (macOS only). Built with the native clang, not the
# cross compiler — it's a Mach-O binary that links against
# Hypervisor.framework and runs on the host, not inside the VM.
HOST_DIR   := host
HOST_SRCS  := $(wildcard $(HOST_DIR)/*.c)
HVF_RUNNER := $(BUILD_DIR)/hvf-runner
HOST_CC    := clang
HOST_CFLAGS := -Wall -Wextra -O2 -g -std=c11

.PHONY: all run debug clean dump build-hvf run-hvf

all: $(TARGET_ELF)

$(BUILD_DIR):
	@mkdir -p $@

# Stamp file that changes whenever HOST_IF changes, so switching
# between HOST_IF=tcg and HOST_IF=hvf forces a full rebuild without
# needing a manual `make clean`. Wipe .o files in the recipe so the
# rebuild happens even when sub-second timestamps collide.
HOST_IF_STAMP := $(BUILD_DIR)/.host-if-$(HOST_IF)
$(HOST_IF_STAMP): | $(BUILD_DIR)
	@echo "HOST_IF=$(HOST_IF), clearing stale objects"
	@rm -f $(BUILD_DIR)/.host-if-* $(BUILD_DIR)/*.o $(BUILD_DIR)/hypervisor.elf $(BUILD_DIR)/hypervisor.bin
	@touch $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HOST_IF_STAMP) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S $(HOST_IF_STAMP) | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET_ELF): $(OBJS) $(LINKER) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	$(OBJCOPY) -O binary $@ $(TARGET_BIN)

# DTS → DTB via cpp + dtc so the initrd-start/end properties can be
# toggled via -DHAVE_INITRAMFS=1 without a second source file.
ifneq ($(GUEST_INITRAMFS),)
DTB_CPPFLAGS := -DHAVE_INITRAMFS=1 \
                -DINITRAMFS_START=$(GUEST_INITRAMFS_ADDR) \
                -DINITRAMFS_END=$$(( $(GUEST_INITRAMFS_ADDR) + $(INITRAMFS_BYTES) ))
else
DTB_CPPFLAGS :=
endif

# DTB depends on the initramfs file too — its size is baked into
# linux,initrd-end, so a new initramfs without rebuilding the DTB
# means Linux unpacks the wrong range.
$(GUEST_DTB): $(GUEST_DTS) $(GUEST_INITRAMFS) | $(BUILD_DIR)
	$(CC) -E -P -nostdinc -undef -x assembler-with-cpp $(DTB_CPPFLAGS) \
	  -o $(BUILD_DIR)/guest.dts.pp $<
	dtc -I dts -O dtb -o $@ $(BUILD_DIR)/guest.dts.pp

run: $(RUN_DEPS)
	$(QEMU) $(QEMU_FLAGS)

debug: $(RUN_DEPS)
	$(QEMU) $(QEMU_FLAGS) -s -S

# ---- HVF path (macOS on Apple Silicon) ----

HVF_ENTITLEMENTS := $(HOST_DIR)/hvf.entitlements

# HVF requires the com.apple.security.hypervisor entitlement, stamped
# in via codesign. Using ad-hoc signing (-s -) is enough locally.
$(HVF_RUNNER): $(HOST_SRCS) $(HVF_ENTITLEMENTS) | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -framework Hypervisor -o $@ $(HOST_SRCS)
	codesign --entitlements $(HVF_ENTITLEMENTS) --force -s - $@

build-hvf: $(HVF_RUNNER) $(TARGET_ELF)

run-hvf: $(HVF_RUNNER) $(TARGET_ELF)
	$(HVF_RUNNER) $(TARGET_ELF)

dump: $(TARGET_ELF)
	$(CROSS)objdump -d $(TARGET_ELF) | less

clean:
	rm -rf $(BUILD_DIR)
