#include "usercopy.h"
#include "pmm.h"
#include "vmm.h"

#define USER_ADDRESS_LIMIT 0x0000800000000000ULL

static int user_range_valid(uint64_t address, size_t size) {
    return address < USER_ADDRESS_LIMIT && size <= USER_ADDRESS_LIMIT - address;
}

static int user_range_accessible(uint64_t address, size_t size, int writable) {
    if (!user_range_valid(address, size))
        return 0;
    if (size == 0)
        return 1;

    struct vmm_space current;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current.pml4_physical));
    current.pml4_physical &= 0x000FFFFFFFFFF000ULL;
    uint64_t end = address + size;
    for (uint64_t page = address & ~(PMM_PAGE_SIZE - 1); page < end;
         page += PMM_PAGE_SIZE) {
        uint64_t flags;
        if (!vmm_space_query(&current, page, NULL, &flags) ||
            !(flags & VMM_USER) ||
            (writable && !(flags & (VMM_WRITABLE | VMM_COPY_ON_WRITE))))
            return 0;
    }
    return 1;
}

static int copy_user(void *kernel_buffer, uint64_t user_address, size_t size,
                     int write_to_user) {
    if (size == 0)
        return 1;
    if (kernel_buffer == NULL || !user_range_valid(user_address, size))
        return 0;

    uint8_t *kernel = kernel_buffer;
    size_t copied = 0;
    while (copied < size) {
        uint64_t address = user_address + copied;
        uint64_t physical;
        uint64_t flags;
        struct vmm_space current;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current.pml4_physical));
        current.pml4_physical &= 0x000FFFFFFFFFF000ULL;
        if (!vmm_space_query(&current, address, &physical, &flags))
            return 0;
        if (write_to_user && (flags & VMM_COPY_ON_WRITE)) {
            if (vmm_space_resolve_cow(&current, address) <= 0 ||
                !vmm_space_query(&current, address, &physical, &flags))
                return 0;
        }
        if (!(flags & VMM_USER) || (write_to_user && !(flags & VMM_WRITABLE)))
            return 0;

        size_t chunk = PMM_PAGE_SIZE - (address & (PMM_PAGE_SIZE - 1));
        if (chunk > size - copied)
            chunk = size - copied;
        uint8_t *mapped = pmm_physical_to_virtual(physical);
        for (size_t i = 0; i < chunk; i++) {
            if (write_to_user)
                mapped[i] = kernel[copied + i];
            else
                kernel[copied + i] = mapped[i];
        }
        copied += chunk;
    }
    return 1;
}

int copy_from_user(void *destination, uint64_t source, size_t size) {
    return copy_user(destination, source, size, 0);
}

int copy_to_user(uint64_t destination, const void *source, size_t size) {
    return copy_user((void *)source, destination, size, 1);
}

int user_range_readable(uint64_t address, size_t size) {
    return user_range_accessible(address, size, 0);
}

int user_range_writable(uint64_t address, size_t size) {
    return user_range_accessible(address, size, 1);
}
