#pragma once

/* Hypercall numbers — passed in x0, args in x1..x7. */
#define HVC_PING     0   /* no args, no effect; just logs */
#define HVC_PUTS     1   /* x1 = const char * (guest IPA, flat for now) */
#define HVC_IRQ_DONE 2   /* guest finished its virt IRQ — clear HCR_EL2.VI */
