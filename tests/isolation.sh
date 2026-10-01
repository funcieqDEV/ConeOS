#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Use a separate initramfs and ISO so tests never replace the normal init.
mkdir -p build/isolation-root build/isolation-initramfs
cp -a iso_root/. build/isolation-root/
cp -a build/initramfs_root/. build/isolation-initramfs/
cp build/user/isolation_test.elf build/isolation-initramfs/bin/init
tar --format=ustar -cf build/isolation-root/boot/initramfs.tar \
    -C build/isolation-initramfs .
xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    build/isolation-root -o build/isolation.iso > build/isolation-image.log 2>&1
./limine/limine bios-install build/isolation.iso >> build/isolation-image.log 2>&1

for cpu in max qemu64; do
    log="build/isolation-${cpu}.log"
    if timeout "${CONEOS_TEST_TIMEOUT:-20}s" qemu-system-x86_64 \
        -M q35 -cpu "$cpu" -m 256M -cdrom build/isolation.iso -boot d \
        -display none -serial "file:${log}" -no-reboot \
        > "build/isolation-${cpu}-qemu.log" 2>&1; then
        :
    else
        status=$?
        if [[ "$status" != 124 ]]; then
            cat "build/isolation-${cpu}-qemu.log"
            exit "$status"
        fi
    fi
    if ! rg -q '^isolation tests passed$' "$log" ||
       rg -q 'KERNEL PANIC|isolation test failed|failed to mount initramfs' "$log"; then
        cat "$log"
        exit 1
    fi
    if [[ "$cpu" == max ]]; then
        rg -q 'CPU SMEP enabled' "$log"
        rg -q 'CPU SMAP enabled' "$log"
    else
        rg -q 'CPU SMEP.*unavailable' "$log"
        rg -q 'CPU SMAP.*unavailable' "$log"
    fi
    printf 'Isolation tests passed on CPU %s\n' "$cpu"
done
