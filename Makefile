CC = gcc
LD = ld
LIMINE_DIR = limine
LIMINE_REPO = https://github.com/limine-bootloader/limine.git
LIMINE_BRANCH = v9.x-binary

CFLAGS = -Wall -Wextra -O2 -pipe -m64 -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel -Iflanterm/src -MMD -MP
USER_CFLAGS = -Wall -Wextra -O2 -m64 -ffreestanding -fno-stack-protector \
	-fno-pie -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone
LDFLAGS = -m elf_x86_64 -nostdlib -static -T linker.ld -z max-page-size=0x1000

OBJS = build/kernel/kernel.o \
       build/kernel/process.o \
       build/kernel/elf.o \
       build/kernel/shell.o \
       build/kernel/task.o \
       build/fs/ramfs.o \
       build/fs/devfs.o \
       build/fs/vfs.o \
       build/fs/initramfs.o \
       build/fs/fat32.o \
       build/mm/kmalloc.o \
       build/mm/pmm.o \
       build/mm/stack.o \
       build/mm/vmm.o \
       build/mm/usercopy.o \
       build/lib/memory.o \
       build/log.o \
       build/drivers/serial.o \
       build/drivers/framebuffer.o \
	   build/drivers/pic.o \
       build/drivers/pit.o \
       build/drivers/ps2.o \
       build/drivers/rtc.o \
       build/drivers/pci.o \
       build/drivers/block.o \
       build/cpu/gdt.o \
       build/cpu/protection.o \
       build/cpu/apic.o \
       build/firmware/acpi.o \
       build/firmware/madt.o \
       build/cpu/idt.o \
	   build/cpu/exceptions.o \
	   build/cpu/irq.o \
       build/cpu/syscall.o \
       build/gfx/draw.o \
	   build/gfx/console.o \
       build/flanterm/flanterm.o \
       build/flanterm/flanterm_backends/fb.o

USER_ELFS = build/user/init.elf build/user/shell.elf build/user/ls.elf \
            build/user/cat.elf build/user/echo.elf build/user/uptime.elf \
            build/user/stat.elf
USER_ELFS += build/user/mkdir.elf build/user/rm.elf
USER_ELFS += build/user/rmdir.elf
USER_ELFS += build/user/cp.elf
USER_ELFS += build/user/lspci.elf
USER_ELFS += build/user/sync.elf
USER_ELFS += build/user/mount.elf
USER_ELFS += build/user/umount.elf
USER_ELFS += build/user/isolation_test.elf

.PHONY: all clean run iso fmt check-build-tools check-run-tools test-isolation test-acpi test-apic
.SECONDARY: build/user/init.elf build/user/shell.elf build/user/ls.elf \
            build/user/cat.elf build/user/echo.elf build/user/uptime.elf

all: iso

check-build-tools:
	@command -v $(CC) >/dev/null || { echo "error: missing $(CC)"; exit 1; }
	@command -v $(LD) >/dev/null || { echo "error: missing $(LD)"; exit 1; }
	@command -v xorriso >/dev/null || { echo "error: missing xorriso (Nobara/Fedora: sudo dnf install xorriso)"; exit 1; }
	@command -v git >/dev/null || { echo "error: missing git"; exit 1; }

check-run-tools:
	@command -v qemu-system-x86_64 >/dev/null || { echo "error: missing QEMU (Nobara/Fedora: sudo dnf install qemu-system-x86-core)"; exit 1; }

$(LIMINE_DIR)/limine.h:
	@echo "Fetching Limine $(LIMINE_BRANCH)..."
	@if test -f $(LIMINE_DIR)/limine.h; then \
		echo "Limine sources already present"; \
	else \
		git clone --depth=1 --branch=$(LIMINE_BRANCH) $(LIMINE_REPO) $(LIMINE_DIR); \
	fi

$(OBJS): | $(LIMINE_DIR)/limine.h

build/user/shell.o: userspace/hello.c userspace/lib/syscall.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.o: userspace/%.c userspace/lib/syscall.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/lib/syscall.o: userspace/lib/syscall.c userspace/lib/syscall.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/lib/heap.o: userspace/lib/heap.c userspace/lib/heap.h userspace/lib/syscall.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.elf: build/user/%.o build/user/lib/syscall.o build/user/lib/heap.o userspace/linker.ld
	$(LD) -m elf_x86_64 -nostdlib -static -T userspace/linker.ld \
		$< build/user/lib/syscall.o build/user/lib/heap.o -o $@

build/user/%_elf.o: build/user/%.elf
	$(LD) -m elf_x86_64 -r -b binary $< -o $@

build/initramfs.tar: $(USER_ELFS)
	@mkdir -p build/initramfs_root/bin
	cp build/user/init.elf build/initramfs_root/bin/init
	cp build/user/shell.elf build/initramfs_root/bin/shell
	cp build/user/ls.elf build/initramfs_root/bin/ls
	cp build/user/cat.elf build/initramfs_root/bin/cat
	cp build/user/echo.elf build/initramfs_root/bin/echo
	cp build/user/uptime.elf build/initramfs_root/bin/uptime
	cp build/user/stat.elf build/initramfs_root/bin/stat
	cp build/user/mkdir.elf build/initramfs_root/bin/mkdir
	cp build/user/rm.elf build/initramfs_root/bin/rm
	cp build/user/rmdir.elf build/initramfs_root/bin/rmdir
	cp build/user/cp.elf build/initramfs_root/bin/cp
	cp build/user/lspci.elf build/initramfs_root/bin/lspci
	cp build/user/sync.elf build/initramfs_root/bin/sync
	cp build/user/mount.elf build/initramfs_root/bin/mount
	cp build/user/umount.elf build/initramfs_root/bin/umount
	cp build/user/isolation_test.elf build/initramfs_root/bin/isolation-test
	tar --format=ustar -cf $@ -C build/initramfs_root .

build/cpu/irq.o build/cpu/exceptions.o: build/cpu/%.o: src/cpu/%.c
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

iso_root/boot/kernel.elf: $(OBJS) linker.ld
	@mkdir -p iso_root/boot
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.h
	$(MAKE) -C $(LIMINE_DIR)

iso: check-build-tools iso_root/boot/kernel.elf build/initramfs.tar $(LIMINE_DIR)/limine limine.conf
	@mkdir -p iso_root/boot/limine
	@mkdir -p iso_root/EFI/BOOT
	cp limine.conf iso_root/boot/limine/
	cp build/initramfs.tar iso_root/boot/initramfs.tar
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o ConeOS.iso
	./limine/limine bios-install ConeOS.iso

disk.img:
	truncate -s 64M $@
	mkfs.fat -F 32 -n CONEOS $@

run: check-run-tools iso disk.img
	qemu-system-x86_64 -M q35 -m 256M -cdrom ConeOS.iso -boot d -serial stdio\
		-drive file=disk.img,if=none,id=coneos_disk,format=raw \
		-device virtio-blk-pci,drive=coneos_disk,disable-modern=on

fmt:
	find src -name '*.c' -o -name '*.h' | xargs clang-format -i

test-isolation: iso check-run-tools
	bash tests/isolation.sh

build/acpi-test: tests/acpi_test.c src/firmware/madt.c src/firmware/madt.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -O2 tests/acpi_test.c src/firmware/madt.c -o $@

test-acpi: build/acpi-test
	./build/acpi-test

test-apic: test-acpi iso check-run-tools
	python3 tests/apic_boot.py

clean:
	rm -rf build iso_root/boot/kernel.elf ConeOS.iso
	make -C limine clean

-include $(OBJS:.o=.d)
