#pragma once

/*
 * Layout must match SAVE_CTX / RESTORE_CTX in src/vectors.S.
 * 32 * 8 = 256 bytes for GPRs/elr/spsr; 272 total after 16-byte pad.
 */
typedef struct trap_frame {
    unsigned long x[31];     /* x0..x30                  offsets 0x000..0xF0 */
    unsigned long elr_el2;   /*                          offset  0xF8        */
    unsigned long spsr_el2;  /*                          offset  0x100       */
    unsigned long _pad;      /* keep 16-byte aligned     offset  0x108       */
} trap_frame_t;

enum vec_kind {
    VEC_EL2T_SYNC = 0, VEC_EL2T_IRQ, VEC_EL2T_FIQ, VEC_EL2T_SERROR,
    VEC_EL2H_SYNC = 4, VEC_EL2H_IRQ, VEC_EL2H_FIQ, VEC_EL2H_SERROR,
    VEC_L64_SYNC  = 8, VEC_L64_IRQ,  VEC_L64_FIQ,  VEC_L64_SERROR,
    VEC_L32_SYNC  = 12,VEC_L32_IRQ,  VEC_L32_FIQ,  VEC_L32_SERROR,
};

void handle_exception(trap_frame_t *tf, unsigned long kind);
