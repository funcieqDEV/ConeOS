#pragma once

int smp_init(void);
int smp_prepare_aps(void);
void smp_timer_tick_current(void);
void smp_start_timers(void);
