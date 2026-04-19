#pragma once

extern volatile unsigned long guest_counter;
extern volatile unsigned long guest_current_el;
extern volatile unsigned long guest_pfr0;

void guest_entry(void);
