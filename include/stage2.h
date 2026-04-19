#pragma once

/* One-time VTCR_EL2 programming + per-VM stage-2 table allocation.
 *
 * stage2_global_init()   — program VTCR_EL2 and cntvoff_el2 once at boot.
 * stage2_alloc_vm(i, vm) — build stage-2 tables for VM `i` with VMID `vm`
 *                         and return the VTTBR_EL2 value to install when
 *                         that vCPU runs. Each VM gets identity mappings
 *                         plus one *private* 4 KiB scratch page at
 *                         IPA 0x50000000 backed by distinct host memory,
 *                         so the guest can prove it sees its own RAM.
 */
void          stage2_global_init(void);
unsigned long stage2_alloc_vm(unsigned id, unsigned vmid);

#define VM_PRIVATE_IPA   0x0A000000UL
