# ============================================================
# Azix OS - Makefile
# Build with: make
# Clean with: make clean
# ============================================================

CC  = i686-linux-gnu-gcc
AS  = nasm
LD  = i686-linux-gnu-gcc

# Compiler flags — freestanding 32-bit kernel, no stdlib
CFLAGS  = -m32 -std=c99 -ffreestanding -O2 \
          -Wall -Wextra \
          -fno-stack-protector \
          -fno-pic \
          -Ikernel

# Linker flags — no standard startup / libraries; use our linker script
LDFLAGS = -m32 -nostdlib -ffreestanding -T linker.ld

# Object files (order matters: boot first so multiboot header is at offset 0)
OBJ = obj/boot.o    \
      obj/idt_asm.o \
      obj/terminal.o\
      obj/idt.o     \
      obj/keyboard.o\
      obj/fb.o      \
      obj/mouse.o   \
      obj/heap.o    \
      obj/pci.o     \
      obj/pcnet.o   \
      obj/net.o     \
      obj/tcp.o     \
      obj/browser.o \
      obj/kernel.o

# ------------------------------------------------------------------ #

all: azix.iso

obj:
	mkdir -p obj

# Assembly files
obj/boot.o: boot/boot.asm | obj
	$(AS) -f elf32 $< -o $@

obj/idt_asm.o: kernel/idt_asm.asm | obj
	$(AS) -f elf32 $< -o $@

# C kernel sources (all live under kernel/)
obj/%.o: kernel/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

# Link the kernel ELF
azix.kernel: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ -lgcc

# Create the bootable ISO with GRUB
azix.iso: azix.kernel
	mkdir -p iso/boot/grub
	cp azix.kernel iso/boot/
	cp grub/grub.cfg iso/boot/grub/
	grub-mkrescue -o azix.iso iso/

clean:
	rm -rf obj/ azix.kernel azix.iso iso/

.PHONY: all clean
