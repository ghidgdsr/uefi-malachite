ARCH              = arm64
TARGET            = aarch64-linux-gnu
CC                = clang
CFLAGS            = --target=$(TARGET)
OBJCOPY           = llvm-objcopy

# Optional: copy the bootloader's DTB to a reserved page before the FD copy.
DTB_COPY_ADDR     ?= 0
DTB_COPY_SIZE     ?= 0

# Optional: copy an "XNUP" payload appended after the FD to this address (0: none).
PAYLOAD_BASE      ?= 0

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) ${CFLAGS} -c $< -o $@ -DREQUIRES_KERNEL_HEADER=$(REQUIRES_KERNEL_HEADER) -DFD_BASE=$(FD_BASE) -DFD_SIZE=$(FD_SIZE) -DDTB_COPY_ADDR=$(DTB_COPY_ADDR) -DDTB_COPY_SIZE=$(DTB_COPY_SIZE) -DPAYLOAD_BASE=$(PAYLOAD_BASE)

BootShim.S:
