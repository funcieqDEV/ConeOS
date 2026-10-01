#include "block.h"
#include "../log.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "pci.h"
#include "pic.h"

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_BLK_LEGACY 0x1001
#define VIRTQ_SIZE 8
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_T_FLUSH 4

struct virtq_desc {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));
struct request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static uint16_t io_base;
static uint16_t queue_size;
static uint64_t sectors;
static uint64_t queue_physical, request_physical;
static volatile struct virtq_desc *descriptors;
static volatile uint16_t *available;
static volatile uint16_t *used;
static volatile struct request_header *request;
static int initialized;
static int flush_supported;

static uint16_t inw_local(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static uint32_t inl_local(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void outb_local(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static void outw_local(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}
static void outl_local(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static void clear_page(void *address, size_t size) {
    for (size_t i = 0; i < size; i++)
        ((uint8_t *)address)[i] = 0;
}

int block_init(void) {
    struct pci_device_info info;
    int found = 0;
    for (size_t i = 0; pci_get_device(i, &info); i++)
        if (info.vendor_id == VIRTIO_VENDOR &&
            info.device_id == VIRTIO_BLK_LEGACY) {
            found = 1;
            break;
        }
    if (!found) {
        LOG_INFO("VirtIO block device not present");
        return 0;
    }
    uint32_t bar = pci_config_read32(info.bus, info.slot, info.function, 0x10);
    if (!(bar & 1)) {
        LOG_ERROR("VirtIO block has no I/O BAR");
        return 0;
    }
    io_base = (uint16_t)(bar & ~3U);
    uint32_t command = pci_config_read32(info.bus, info.slot, info.function, 4);
    /* Enable I/O space and bus-master DMA before publishing the virtqueue. */
    pci_config_write16(info.bus, info.slot, info.function, 4,
                       (uint16_t)(command | 5));
    outb_local(io_base + 0x12, 0);
    outb_local(io_base + 0x12, 1);
    outb_local(io_base + 0x12, 3);
    uint32_t host_features = inl_local(io_base);
    flush_supported = (host_features & (1U << VIRTIO_BLK_F_FLUSH)) != 0;
    outl_local(io_base + 4, flush_supported ? (1U << VIRTIO_BLK_F_FLUSH) : 0);
    outw_local(io_base + 0x0E, 0);
    queue_size = inw_local(io_base + 0x0C);
    if (queue_size < VIRTQ_SIZE || queue_size > 256) {
        LOG_ERROR("VirtIO block queue unsupported");
        return 0;
    }
    queue_physical = pmm_alloc_contiguous(3);
    request_physical = pmm_alloc_page();
    if (queue_physical == PMM_INVALID_ADDRESS ||
        request_physical == PMM_INVALID_ADDRESS) {
        LOG_ERROR("VirtIO block DMA allocation failed");
        return 0;
    }
    void *queue = pmm_physical_to_virtual(queue_physical);
    clear_page(queue, PMM_PAGE_SIZE * 3);
    clear_page(pmm_physical_to_virtual(request_physical), PMM_PAGE_SIZE);
    descriptors = queue;
    available = (volatile uint16_t *)((uint8_t *)queue +
                                      sizeof(struct virtq_desc) * queue_size);
    size_t used_offset =
        sizeof(struct virtq_desc) * queue_size + 4 + 2 * queue_size;
    used_offset = (used_offset + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    used = (volatile uint16_t *)((uint8_t *)queue + used_offset);
    request = pmm_physical_to_virtual(request_physical);
    outl_local(io_base + 8, (uint32_t)(queue_physical >> 12));
    sectors = (uint64_t)inl_local(io_base + 0x14) |
              ((uint64_t)inl_local(io_base + 0x18) << 32);
    outb_local(io_base + 0x12, 7);
    initialized = 1;
    LOG_INFO("VirtIO block initialized");
    return 1;
}

static int transfer(uint64_t sector, void *buffer, int write) {
    if (!initialized || buffer == NULL || sector >= sectors)
        return 0;
    request->type = write ? 1 : 0;
    request->reserved = 0;
    request->sector = sector;
    uint8_t *data = (uint8_t *)request + 16;
    if (write)
        for (size_t i = 0; i < 512; i++)
            data[i] = ((const uint8_t *)buffer)[i];
    uint8_t *status = (uint8_t *)request + 528;
    *status = 0xFF;
    descriptors[0] =
        (struct virtq_desc){request_physical, 16, VIRTQ_DESC_F_NEXT, 1};
    descriptors[1] = (struct virtq_desc){
        request_physical + 16, 512,
        (uint16_t)(VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE)), 2};
    descriptors[2] =
        (struct virtq_desc){request_physical + 528, 1, VIRTQ_DESC_F_WRITE, 0};
    uint16_t index = available[1];
    available[2 + (index % VIRTQ_SIZE)] = 0;
    __asm__ volatile("" : : : "memory");
    available[1] = index + 1;
    outw_local(io_base + 0x10, 0);
    for (uint64_t wait = 0; wait < 10000000; wait++) {
        if (used[1] != index + 1)
            continue;
        if (*status != 0) {
            LOG_ERROR("VirtIO block request rejected");
            return 0;
        }
        if (!write)
            for (size_t i = 0; i < 512; i++)
                ((uint8_t *)buffer)[i] = data[i];
        return 1;
    }
    LOG_ERROR("VirtIO block request timed out");
    return 0;
}

int block_ready(void) { return initialized; }
uint64_t block_sector_count(void) { return sectors; }
int block_read_sector(uint64_t sector, void *buffer) {
    return transfer(sector, buffer, 0);
}
int block_write_sector(uint64_t sector, const void *buffer) {
    return transfer(sector, (void *)buffer, 1);
}

int block_flush(void) {
    if (!initialized)
        return 0;
    if (!flush_supported)
        return 0;
    request->type = VIRTIO_BLK_T_FLUSH;
    request->reserved = 0;
    request->sector = 0;
    uint8_t *status = (uint8_t *)request + 16;
    *status = 0xFF;
    descriptors[0] =
        (struct virtq_desc){request_physical, 16, VIRTQ_DESC_F_NEXT, 1};
    descriptors[1] =
        (struct virtq_desc){request_physical + 16, 1, VIRTQ_DESC_F_WRITE, 0};
    uint16_t index = available[1];
    available[2 + (index % VIRTQ_SIZE)] = 0;
    __asm__ volatile("" : : : "memory");
    available[1] = index + 1;
    outw_local(io_base + 0x10, 0);
    for (uint64_t wait = 0; wait < 10000000; wait++) {
        if (used[1] != index + 1)
            continue;
        if (*status != 0) {
            LOG_ERROR("VirtIO block flush rejected");
            return 0;
        }
        return 1;
    }
    LOG_ERROR("VirtIO block flush timed out");
    return 0;
}
