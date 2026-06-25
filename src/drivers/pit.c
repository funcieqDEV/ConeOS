#include "pit.h"
#include "../cpu/irq.h"
#include "../kernel/task.h"
#include "../log.h"
#include "pic.h"

#define PIT_INPUT_HZ 1193182
#define PIT_CHANNEL_0 0x40
#define PIT_COMMAND 0x43
#define PIT_MODE_RATE_GENERATOR 0x34

static uint64_t ticks;
static uint8_t scheduler_ticks;

static void pit_handler(struct interrupt_frame *frame) {
    (void)frame;
    __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
    scheduler_ticks++;
    if (scheduler_ticks >= 5) {
        scheduler_ticks = 0;
        task_preempt();
    }
}

void pit_init(void) {
    uint16_t divisor = PIT_INPUT_HZ / PIT_FREQUENCY_HZ;

    outb(PIT_COMMAND, PIT_MODE_RATE_GENERATOR);
    outb(PIT_CHANNEL_0, divisor & 0xFF);
    outb(PIT_CHANNEL_0, divisor >> 8);

    irq_install_handler(0, pit_handler);
    irq_clear_mask(0);
    LOG_INFO("PIT initialized at 100 Hz");
}

uint64_t pit_ticks(void) { return __atomic_load_n(&ticks, __ATOMIC_RELAXED); }

uint64_t pit_uptime_ms(void) { return pit_ticks() * 1000 / PIT_FREQUENCY_HZ; }
