#include "framebuffer.h"

#define NULL ((void *)0)

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 6};

struct limine_framebuffer *framebuffer_get(void) {
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        return NULL;
    }

    return framebuffer_request.response->framebuffers[0];
}
