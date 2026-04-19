# QEMU TCG GICv2 `virt` — list-register path is broken

On QEMU `virt` with `-machine virt,virtualization=on,gic-version=2` running
under TCG (no KVM), writing a pending LR to `GICH_LRn` does latch — readback
confirms the state bits are set — but `GICV_IAR` / `GICV_HPPIR` always return
1023 (spurious). The virtual CPU interface never drives VIRQ to the PE even
with `GICH_HCR.En=1`, `GICH_VMCR` seeded with `EnGrp1` / `VPMR=0x1F`, and
`GICV_CTLR` enabled by the guest. Burned ~2 hours during Stage 6 before we
accepted it.

## Why

QEMU TCG's GICv2 virtualisation is incomplete for the LR → GICV path; the
hardware-linked injection model (`HW=1` in LR for auto-deactivate on guest
EOI) does not work on this emulator. KVM on real ARMv8 hardware implements
the same LRs correctly, so code written against the spec works there —
just not under TCG.

## Workaround in this tree

For IRQ injection on this platform, bypass the GIC list registers and use
`HCR_EL2.VI` (bit 7) to directly pend a virtual IRQ. Cost: the guest must
hypercall (`HVC_IRQ_DONE`) on EOI so EL2 can clear VI, since the bit is
sticky. For the Linux guest (which won't cooperate with our hypercall), we
additionally unmap the guest's view of `GICC` in stage-2 and fully
trap-and-emulate it — see `src/vgic.c`.

Also disable the physical timer source inside the EL2 IRQ handler to avoid
storming — the guest re-arms `CNTV_CTL_EL0` when it re-arms the timer.

## Related TCG quirk, same area

QEMU's GICv2 tags PPIs as Group 0 regardless of our `IGROUPR` writes, so the
physical `GICC_CTLR` needs `AckCtl` (bit 2) set for NS IAR to ack them —
otherwise IAR returns 1022 (instead of the expected INTID). That's why
`gic_init_el2` writes `0x3 | (1<<2)` to `GICC_CTLR`.
