# ARMv8-A Type-1 Hypervisor

A Type-1 bare-metal hypervisor for ARMv8-A. Written from scratch in C and AArch64 assembly. Runs at EL2 on QEMU's `virt` machine and boots an unmodified Alpine Linux 6.18 kernel all the way to an interactive userspace shell.

```
Hello from EL2
[gic] GICv2 up; PPIs 26 (htimer) + 27 (vtimer), SPI 33 (pl011)
[stage2] VTCR_EL2  = 0x23560
[stage2] VM 0 VMID 1 VTTBR= 0x100004008d000
[sched] initialised 1 vCPU(s), slice=50 ms; vCPU0 -> Linux@0x48200000 DTB@0x4c000000
[sched] ERET into vCPU 0
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd083]
[    0.000000] Linux version 6.18.22-0-virt ...
        ...
[    0.132393] printk: console [ttyAMA0] enabled

--- Hypervisor guest shell ---
Alpine 6.18.22-0-virt under the Type-1 hypervisor.

guest:/# uname -a
Linux (none) 6.18.22-0-virt #1-Alpine SMP PREEMPT_DYNAMIC 2026-04-11 15:53:25 aarch64 Linux
guest:/# cat /proc/interrupts
           CPU0
 11:       N  GIC-0  27 Level     arch_timer
 13:       N  GIC-0  33 Level     uart-pl011
```

## What it does

The hypervisor runs at EL2 with its own vector table, stack, and stage-1 identity mapping. An EL1 guest VM (Alpine Linux 6.18) boots inside it, and every GIC access from the guest is trapped and emulated on top of `HCR_EL2.VI` virtual interrupt injection.

Multiple guests are isolated with stage-2 translation. Each VM gets its own VMID, its own `VTTBR_EL2`, and its own page table tree. A per-VM private page at IPA `0x0A000000` proves the isolation: every guest reads a different byte from that same address.

Two vCPUs can share one physical CPU round-robin. Linux runs on vCPU 0, a small bare-metal guest runs on vCPU 1, and every scheduler tick saves and restores the full EL1 context.

Real device interrupts get forwarded. The PL011 UART's `SPI 33` traps at EL2, lands in the guest's virtual GIC, and comes out as keystrokes at the Linux shell.

PSCI v1.0 over HVC handles Linux's power management and SMP calls.

## Capabilities

| Feature | Status |
| ----------------------------------------- | --------------------------------- |
| EL2 boot and exception vectors | working |
| Stage-2 MMU with per-VM VMIDs | working |
| Virtual GICv2 (CPU interface + DIST) | emulated / passthrough split |
| Virtual IRQ injection via `HCR_EL2.VI` | working |
| Virtual timer forwarding (PPI 27) | working |
| PSCI v1.0 over HVC | working |
| Round-robin scheduler (CNTHP-driven) | working, with a TCG quirk (below) |
| Multi-guest (Linux + bare-metal) | working, via `make NVCPU=2` |
| Linux boot to userspace shell | working, interactive input |
| VM-exit ring buffer for post-mortems | working |
| `virtio-blk` / persistent rootfs | not wired |
| `virtio-net` / networking | not wired |
| SMP guest (multi-vCPU Linux) | not wired, guest is UP |
| HVF backend / native Apple Silicon | not wired, QEMU TCG only |

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                           EL0                            │  user space (busybox ash)
├──────────────────────────────────────────────────────────┤
│ Linux kernel (Alpine 6.18)  │   bare-metal guest         │  EL1. Each guest
│   stage-1 page tables       │    stage-1 off             │  has its own VMID,
│   emulated GICv2 view       │    emulated GICv2 view     │  EL1 sysreg state,
└──────────────────────────────┴────────────────────────────┘  vtimer.
                 │                           │
       stage-2 translation            stage-2 translation          VTTBR_EL2 per VM
                 └───────────┬───────────────┘                      4 KB page granule
                             ↓                                      VTCR_EL2.T0SZ=32
┌──────────────────────────────────────────────────────────┐
│                     This hypervisor                      │  EL2
│   • traps stage-2 faults in the GIC window → vgic_mmio   │
│   • traps HVC calls (PSCI + our own hcalls) → exceptions │
│   • handles physical IRQs (PPI 26, 27; SPI 33) → gic     │
│   • round-robin schedule via CNTHP (PPI 26)              │
│   • HCR_EL2: VM=1 IMO=1 TWI=1 RW=1                       │
│   • VI bit used to inject virtual IRQs                   │
└──────────────────────────────────────────────────────────┘
                             │
┌──────────────────────────────────────────────────────────┐
│  QEMU virt (aarch64, GICv2, -cpu cortex-a72)             │
│  GICD @ 0x08000000  PL011 @ 0x09000000  RAM @ 0x40000000 │
└──────────────────────────────────────────────────────────┘
```

## Build and run

### Host prerequisites

macOS (Apple Silicon or Intel):

```bash
brew install aarch64-unknown-linux-gnu qemu dtc
```

Linux:

```bash
apt install gcc-aarch64-linux-gnu qemu-system-arm device-tree-compiler cpio
```

### One-time setup of the guest payload

```bash
mkdir -p ~/lab

# Linux kernel. Alpine's vmlinuz-virt is a raw Image despite the name.
curl -L -o ~/lab/Image \
  https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/aarch64/netboot/vmlinuz-virt

# Matching initramfs (busybox and a pile of kernel modules).
curl -L -o ~/lab/initramfs-virt \
  https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/aarch64/netboot/initramfs-virt

# Build a combined initramfs: Alpine's base plus our /init overlay.
# Alpine's own /init blocks forever in `nlplug-findfs` looking for install
# media we don't emulate, so ours runs a small REPL instead.
cd ~/Projects/aarch64-hypervisor
tools/build-initramfs.sh ~/lab/initramfs-virt ~/lab/initramfs-combined
```

### Boot Linux

```bash
make GUEST_KERNEL=~/lab/Image GUEST_INITRAMFS=~/lab/initramfs-combined run
```

After about 2 seconds you get:

```
--- Hypervisor guest shell ---
guest:/#
```

Try `ls /`, `cat /proc/interrupts`, `cat /proc/cpuinfo`, `uname -a`, `dmesg | head`, `free`. Press Ctrl-A then X to exit QEMU.

### Boot Linux plus the bare-metal guest

```bash
make NVCPU=2 GUEST_KERNEL=~/lab/Image GUEST_INITRAMFS=~/lab/initramfs-combined run
```

vCPU 0 runs Linux, vCPU 1 runs `src/guest.c`. CNTHP ticks swap them every 50 ms. Note: QEMU TCG stalls CNTHP after a few fires, see [Known limitations](#known-limitations).

### Debug with gdb

```bash
make GUEST_KERNEL=~/lab/Image debug   # qemu -S -s, waits on tcp:1234
# in another terminal:
aarch64-linux-gnu-gdb build/hypervisor.elf
(gdb) target remote :1234
(gdb) b handle_exception
(gdb) c
```

## Project layout

```
src/
  boot.S              entry from _start, sets up EL2 stack, VBAR_EL2, zeroes BSS, calls kmain
  kmain.c             calls uart_init, gic_init_el2, sched_init, sched_start
  vectors.S           16-entry EL2 vector table, saves and restores trap_frame
  exceptions.c        handle_exception dispatch, PSCI, HVC, WFI trap, stage-2 abort
  stage2.c            per-VM L1/L2/L3 tables, VTCR_EL2, TLB flushing
  sched.c             round-robin scheduler, full EL1 context save and restore
  vgic.c              software GICC emulator, pending bitmap, deferred physical EOI
  gic.c               physical GIC setup, IRQ handler dispatch (PPI 26/27, SPI 33)
  guest.c             bare-metal guest VM (IRQ handler, timer re-arm, HVC_IRQ_DONE)
  guest_vectors.S     EL1 vector table for the bare-metal guest
  uart.c              PL011 polled write helpers for hypervisor logging

include/              matching headers

docs/
  guest.dts           device tree for the Linux guest (preprocessed via cpp)

tools/
  build-initramfs.sh  concatenates Alpine's initramfs-virt with our /init overlay

memory/
  qemu_tcg_gicv2_virt.md   notes on TCG's GICv2 LR path being broken

Makefile / linker.ld  build rules and image layout
LICENSE               MIT
```

Source size: about 1,800 lines of C and 130 lines of AArch64 assembly.

## Key design choices

### Virtual IRQ injection via `HCR_EL2.VI`, not GICH list registers

The most invasive architectural choice in the project. QEMU TCG's GICv2 list-register path doesn't drive `GICV_IAR` correctly (see [`memory/qemu_tcg_gicv2_virt.md`](memory/qemu_tcg_gicv2_virt.md)), so using list registers to inject IRQs from EL2 to EL1 is broken on the platform I develop on. Instead, the guest's view of the GIC CPU interface is unmapped in stage-2. Every access to it traps to EL2, and we maintain our own software `pending_mask` and `active` state per vCPU. `HCR_EL2.VI` asserts a virtual IRQ line to the guest, and the guest reads and writes its emulated `GICC_IAR` and `GICC_EOIR` through the fault handler.

The tradeoff is that every GIC CPU-interface register touch is a VM exit. On real hardware with working list registers this would go away. Under TCG the CPU interface is accessed on every IRQ anyway, so the cost is negligible.

### GICD passthrough, GICC emulated

The distributor is shared state that the guest can reach directly, with the stage-2 mapping pointing at real hardware. It's per-system, not per-CPU, so isolation via VMID doesn't matter at that layer. The CPU interface is per-CPU state that a guest expects to own, so it's emulated. The guest's view matches GIC semantics even when the EL2 CNTHP handler is interleaved on the same physical hardware.

This split reused about 90% of real GIC semantics (priorities, enable bits, target-CPU masks) without implementing a full soft GIC.

### Full EL1 stage-1 context swap on vCPU switch

When Linux was added as vCPU 0 alongside the bare-metal guest on vCPU 1, the first physical CNTHP tick swapped Linux out correctly. Then the bare-metal guest started running with Linux's `SCTLR_EL1`, `TTBR0`, `TTBR1`, `TCR`, `MAIR`, and `VBAR` still live in hardware. Guest B's first instruction translated through Linux's kernel page tables and jumped into a Linux VA.

The fix was obvious in retrospect. Every EL1 sysreg that gets MSR'd has to be part of `vcpu_t` and saved or restored. See `save_vcpu` and `load_vcpu` in [`src/sched.c`](src/sched.c).

### VM-exit ring buffer

`dump_and_halt` (the handler for unexpected exceptions) prints the last 32 VM exits: vector kind, ESR, ELR, FAR, vCPU id. When something crashes, the prelude prints too, not just the final fault. That caught both alignment bugs in `vgic_t` in a single run.

## Known limitations

These are documented in the source too.

- PL011 RX is polled from `handle_wfi`, not a real IRQ. TCG's GICv2 never reliably asserts `SPI 33` to the CPU through `HCR_EL2.IMO` routing. The workaround polls `GICC_HPPIR` every time Linux idles, which is every few ms. It works. Fixing it properly means either switching to HVF or digging into TCG's `gic_update_virt_irq`.

- The shell uses a custom REPL, not busybox `sh -i`. The real shell's interactive line mode hangs waiting on a controlling-tty handshake I haven't untangled. The REPL in `tools/build-initramfs.sh` (a `while IFS= read -r line; do eval "$line"; done`) sidesteps it.

- `NVCPU=2` scheduler stalls after a few slices under TCG. The `CNTHP_CTL_EL2.ISTATUS` bit stops asserting even when `CNTPCT > CVAL`. This is a QEMU TCG timer-model bug, not a hypervisor bug. KVM and HVF don't have it. Guest B boots and ticks a few times, then time-slicing freezes.

- Single active INTID per vCPU in the vGIC. `pending_mask` is a 64-bit bitmap (supports INTIDs 0 to 63), but `active` is one slot. Fine for a UP Linux guest, breaks on nested preemption.

- No virtio devices. No `virtio-blk` means no persistent root. Everything is tmpfs in initramfs RAM, wiped on reboot. No `virtio-net` means no network, so no `apk add` and no downloads.

- Guest is uniprocessor. `PSCI_CPU_ON` returns `NOT_SUPPORTED`.

## Technical coverage

The hypervisor exercises the ARMv8-A virtualization extensions (`HCR_EL2`, `VTCR_EL2`, stage-2 translation, VMIDs, `VTTBR_EL2`) and the full GICv2 architecture (distributor, CPU interface, hypervisor interface, group 0/1, priority arbitration, SPIs / PPIs / SGIs, level and edge triggered). An unmodified Linux guest sits on top of the GICC emulation and runs through its normal init path.

## References

- ARM Architecture Reference Manual for A-profile (ARM DDI 0487)
- ARM Generic Interrupt Controller v2 Architecture Specification (ARM IHI 0048)
- Linux kernel source (`arch/arm64/include/asm/kvm_*.h`, `virt/kvm/arm/*`)
- [QEMU virt machine docs](https://www.qemu.org/docs/master/system/arm/virt.html)

## License

[MIT](LICENSE). Use, modify, redistribute freely. No warranty.
