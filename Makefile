CC = gcc
LD = ld

CFLAGS = -Wall -Wextra -O2 -pipe -m64 -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel -Iflanterm/src
LDFLAGS = -m elf_x86_64 -nostdlib -static -T linker.ld -z max-page-size=0x1000

OBJS = build/kernel/kernel.o \
       build/mm/kmalloc.o \
       build/lib/memory.o \
       build/log.o \
       build/drivers/serial.o \
       build/drivers/framebuffer.o \
	   build/drivers/pic.o \
	   build/drivers/ps2.o \
       build/cpu/idt.o \
	   build/cpu/irq.o \
       build/gfx/draw.o \
	   build/gfx/console.o \
       build/flanterm/flanterm.o \
       build/flanterm/flanterm_backends/fb.o

.PHONY: all clean run iso fmt

all: iso

build/cpu/irq.o: src/cpu/irq.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/flanterm/%.o: flanterm/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

iso_root/boot/kernel.elf: $(OBJS)
	@mkdir -p iso_root/boot
	$(LD) $(LDFLAGS) $(OBJS) -o $@

limine/limine:
	make -C limine

iso: iso_root/boot/kernel.elf limine/limine limine.conf
	@mkdir -p iso_root/boot/limine
	@mkdir -p iso_root/EFI/BOOT
	cp limine.conf iso_root/boot/limine/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o ConeOS.iso
	./limine/limine bios-install ConeOS.iso

run: iso
	qemu-system-x86_64 -M q35 -m 2G -cdrom ConeOS.iso -boot d -serial stdio

fmt:
	find src -name '*.c' -o -name '*.h' | xargs clang-format -i

clean:
	rm -rf build iso_root/boot/kernel.elf ConeOS.iso
	make -C limine clean
