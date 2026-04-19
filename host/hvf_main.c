/*
 * HVF host launcher for the ARMv8-A hypervisor.
 *
 * Runs on macOS 15+ / Apple Silicon M3+. Uses Hypervisor.framework with
 * EL2 nested virtualization enabled so our hypervisor.elf boots at EL2
 * the same way it does under QEMU.
 *
 * Scope (this file, first cut):
 *   - probe for EL2 support
 *   - create the VM + one vCPU with EL2 enabled
 *   - map 512 MiB of host memory as guest RAM at IPA 0x40000000
 *   - load hypervisor.elf's PT_LOAD segments into that RAM
 *   - run loop handling PL011 MMIO (TX only, writes land on host stdout)
 *   - any other exit: dump state and halt
 *
 * Out of scope (deliberately, for now):
 *   - GIC emulation (our hypervisor is GICv2; HVF exposes GICv3. Next step.)
 *   - PL011 RX (stdin)
 *   - PSCI / SMC handling from HVF's perspective
 *   - Loading Linux Image + DTB + initramfs
 */

#include <Hypervisor/Hypervisor.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* ---------- guest physical memory layout ---------- */

#define GUEST_RAM_BASE    0x40000000UL
#define GUEST_RAM_SIZE    (512UL * 1024 * 1024)      /* 512 MiB */
#define HYP_ENTRY_IPA     0x40080000UL                /* from linker.ld */

/* Match the QEMU virt machine layout the hypervisor expects. */
#define PL011_BASE        0x09000000UL
#define PL011_SIZE        0x00001000UL

/* ---------- PL011 register offsets (PrimeCell UART, subset) ---------- */

#define PL011_UARTDR      0x000
#define PL011_UARTFR      0x018
#define PL011_UARTIBRD    0x024
#define PL011_UARTFBRD    0x028
#define PL011_UARTLCR_H   0x02C
#define PL011_UARTCR      0x030
#define PL011_UARTIFLS    0x034
#define PL011_UARTIMSC    0x038
#define PL011_UARTRIS     0x03C
#define PL011_UARTMIS     0x040
#define PL011_UARTICR     0x044

/* ---------- small error-check macro ---------- */

#define HVC(expr)                                                              \
    do {                                                                       \
        hv_return_t _r = (expr);                                               \
        if (_r != HV_SUCCESS) {                                                \
            fprintf(stderr, "HVF: %s = 0x%x at %s:%d\n",                       \
                    #expr, _r, __FILE__, __LINE__);                            \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* ---------- ELF loader (minimal, 64-bit little-endian) ---------- */

#define ELFMAG         "\x7f" "ELF"
#define ELFCLASS64     2
#define ELFDATA2LSB    1
#define EM_AARCH64     183
#define PT_LOAD        1

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr;
    uint64_t p_filesz, p_memsz, p_align;
} __attribute__((packed)) Elf64_Phdr;

static uint64_t load_elf_into(uint8_t *ram, size_t ram_size, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(1); }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); exit(1); }

    uint8_t *elf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (elf == MAP_FAILED) { perror("mmap elf"); exit(1); }
    close(fd);

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (memcmp(eh->e_ident, ELFMAG, 4) != 0 ||
        eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_AARCH64) {
        fprintf(stderr, "%s: not an aarch64 ELF64 little-endian file\n", path);
        exit(1);
    }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++, ph++) {
        if (ph->p_type != PT_LOAD) continue;
        uint64_t dst_pa = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;
        if (dst_pa < GUEST_RAM_BASE ||
            dst_pa + ph->p_memsz > GUEST_RAM_BASE + ram_size) {
            fprintf(stderr, "segment %u: PA 0x%llx + %llu bytes outside guest RAM\n",
                    i, dst_pa, ph->p_memsz);
            exit(1);
        }
        uint8_t *dst = ram + (dst_pa - GUEST_RAM_BASE);
        memcpy(dst, elf + ph->p_offset, ph->p_filesz);
        /* zero BSS tail */
        if (ph->p_memsz > ph->p_filesz)
            memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
        fprintf(stderr, "loaded segment %u: PA 0x%llx size %llu (file %llu)\n",
                i, dst_pa, ph->p_memsz, ph->p_filesz);
    }

    uint64_t entry = eh->e_entry;
    munmap(elf, st.st_size);
    return entry;
}

/* ---------- PL011 minimal emulation ---------- */

static struct {
    uint32_t cr;        /* control        */
    uint32_t lcr_h;     /* line control   */
    uint32_t ibrd;      /* baud integer   */
    uint32_t fbrd;      /* baud fraction  */
    uint32_t imsc;      /* int mask       */
    uint32_t ris;       /* raw int status */
    uint32_t icr;       /* int clear      */
} pl011 = {
    /* post-reset defaults */
    .cr = 0x0300,       /* TXE | RXE */
};

static uint32_t pl011_read(uint32_t off) {
    switch (off) {
    case PL011_UARTFR:    return 0;               /* TX empty, RX empty */
    case PL011_UARTCR:    return pl011.cr;
    case PL011_UARTLCR_H: return pl011.lcr_h;
    case PL011_UARTIBRD:  return pl011.ibrd;
    case PL011_UARTFBRD:  return pl011.fbrd;
    case PL011_UARTIMSC:  return pl011.imsc;
    case PL011_UARTRIS:   return pl011.ris;
    case PL011_UARTMIS:   return pl011.ris & pl011.imsc;
    default:              return 0;
    }
}

static void pl011_write(uint32_t off, uint32_t val) {
    switch (off) {
    case PL011_UARTDR:
        fputc(val & 0xFF, stdout);
        fflush(stdout);
        break;
    case PL011_UARTCR:    pl011.cr    = val; break;
    case PL011_UARTLCR_H: pl011.lcr_h = val; break;
    case PL011_UARTIBRD:  pl011.ibrd  = val; break;
    case PL011_UARTFBRD:  pl011.fbrd  = val; break;
    case PL011_UARTIMSC:  pl011.imsc  = val; break;
    case PL011_UARTICR:   pl011.ris &= ~val; break;
    default: break;
    }
}

/* ---------- data-abort decoding ---------- */

static bool in_pl011_range(uint64_t ipa) {
    return ipa >= PL011_BASE && ipa < PL011_BASE + PL011_SIZE;
}

static hv_return_t read_gpr(hv_vcpu_t vcpu, unsigned srt, uint64_t *out) {
    if (srt == 31) { *out = 0; return HV_SUCCESS; }   /* XZR */
    return hv_vcpu_get_reg(vcpu, HV_REG_X0 + srt, out);
}

static hv_return_t write_gpr(hv_vcpu_t vcpu, unsigned srt, uint64_t v) {
    if (srt == 31) return HV_SUCCESS;                 /* XZR, discard */
    return hv_vcpu_set_reg(vcpu, HV_REG_X0 + srt, v);
}

/* Decoded load/store info. Enough to emulate the subset of AArch64
 * load/store-immediate forms our hypervisor uses: scaled, unscaled,
 * pre-indexed, and post-indexed. Pre/post-indexed require a writeback
 * to Rn after the access. */
typedef struct {
    unsigned size_log2;     /* 0=B, 1=H, 2=W, 3=X */
    unsigned rt;            /* transfer register 0..31 (31 = XZR/WZR) */
    bool     wnr;           /* 1 = store */
    bool     writeback;     /* true for pre/post-indexed */
    unsigned rn;            /* base register, for writeback */
    int32_t  wb_offset;     /* sign-extended imm9 added to Rn */
} ldst_decoded_t;

/* Sign-extend a 9-bit value to int32_t. */
static int32_t sext9(uint32_t v) {
    v &= 0x1FF;
    return (int32_t)(v << 23) >> 23;
}

/* Decode a small subset of AArch64 load/store instructions. Returns
 * true on success; fills *d. HVF reports stage-1 aborts on unmapped
 * IPAs with ISV=0 (no ESR instruction syndrome), so we fetch the
 * insn from guest RAM and parse it ourselves. */
static bool decode_ldst(uint32_t insn, ldst_decoded_t *d) {
    memset(d, 0, sizeof(*d));

    /* Integer load/store immediate share: bits 29:26 = 1110
     * (bits 29:27 = 111 for load/store, bit 26 = 0 for integer). */
    if (((insn >> 26) & 0xF) != 0xE) return false;

    unsigned op24 = (insn >> 24) & 0x3;                  /* 01 = unsigned
                                                            00 = unscaled /
                                                                 pre / post */
    unsigned op   = (insn >> 22) & 0x3;
    if (op > 1) return false;                            /* signed/load-specials */

    d->size_log2 = (insn >> 30) & 0x3;
    d->rt        = insn & 0x1F;
    d->rn        = (insn >> 5) & 0x1F;
    d->wnr       = (op == 0);

    if (op24 == 0x1) {
        /* LDR/STR (unsigned offset, scaled imm12) */
        d->writeback = false;
        return true;
    }

    if (op24 == 0x0) {
        if ((insn >> 21) & 1) return false;              /* register-offset */
        unsigned idx = (insn >> 10) & 0x3;
        int32_t  imm9 = sext9((insn >> 12) & 0x1FF);
        switch (idx) {
        case 0:                                          /* unscaled (STUR/LDUR) */
            d->writeback = false;
            return true;
        case 1:                                          /* post-indexed */
        case 3:                                          /* pre-indexed */
            d->writeback = true;
            d->wb_offset = imm9;
            return true;
        default:
            return false;                                /* idx==2 reserved */
        }
    }
    return false;
}

static uint32_t mask_for_size(unsigned size_log2) {
    switch (size_log2) {
    case 0: return 0x000000FF;
    case 1: return 0x0000FFFF;
    case 2: return 0xFFFFFFFF;
    default: return 0xFFFFFFFF;  /* 3 = 64-bit; treat as word for MMIO */
    }
}

/* Handle an EC=0x24/0x25 data abort that fell on PL011. Returns true
 * if we handled it (caller advances PC past the faulting insn). */
static bool handle_data_abort(hv_vcpu_t vcpu, uint64_t esr, uint64_t ipa,
                              const uint8_t *ram) {
    uint64_t iss = esr & 0x1FFFFFFUL;
    bool     isv = (iss >> 24) & 1;

    ldst_decoded_t d = {0};

    if (isv) {
        /* ESR carries a valid syndrome: trust it. No writeback info
         * in ISS; scaled/unscaled offsets without writeback is the
         * only form ESR.ISV==1 applies to. */
        d.size_log2 = (unsigned)((iss >> 22) & 3);
        d.rt        = (unsigned)((iss >> 16) & 0x1F);
        d.wnr       = (iss >> 6) & 1;
        d.writeback = false;
    } else {
        /* ISV=0 on HVF for our case: fetch and decode the faulting
         * instruction ourselves. */
        uint64_t pc = 0;
        HVC(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc));
        if (pc < GUEST_RAM_BASE || pc + 4 > GUEST_RAM_BASE + GUEST_RAM_SIZE) {
            fprintf(stderr, "data abort: PC 0x%llx outside guest RAM\n", pc);
            return false;
        }
        uint32_t insn;
        memcpy(&insn, ram + (pc - GUEST_RAM_BASE), 4);
        if (!decode_ldst(insn, &d)) {
            fprintf(stderr, "data abort: can't decode 0x%08x at PC 0x%llx, IPA 0x%llx\n",
                    insn, pc, ipa);
            return false;
        }
    }

    if (!in_pl011_range(ipa)) return false;

    /* Perform the MMIO access. */
    uint32_t off = (uint32_t)(ipa - PL011_BASE);
    if (d.wnr) {
        uint64_t v = 0;
        HVC(read_gpr(vcpu, d.rt, &v));
        pl011_write(off, (uint32_t)(v & mask_for_size(d.size_log2)));
    } else {
        uint32_t v = pl011_read(off);
        HVC(write_gpr(vcpu, d.rt, v & mask_for_size(d.size_log2)));
    }

    /* Pre/post-indexed: update Rn. Architecturally, pre-indexed writes
     * Rn before the access and post-indexed after, but the end state
     * is the same (Rn_new = Rn_old + imm) because the access itself
     * doesn't touch Rn. */
    if (d.writeback && d.rn != 31) {
        uint64_t rn_val = 0;
        HVC(hv_vcpu_get_reg(vcpu, HV_REG_X0 + d.rn, &rn_val));
        rn_val += (int64_t)d.wb_offset;
        HVC(hv_vcpu_set_reg(vcpu, HV_REG_X0 + d.rn, rn_val));
    }

    return true;
}

/* ---------- diagnostic dump ---------- */

/* GICv2 MMIO window on our QEMU virt layout. Hitting this under HVF
 * is the expected "next unfinished step" for this session, so the
 * dump note identifies it specifically. */
#define GIC_WINDOW_BASE   0x08000000UL
#define GIC_WINDOW_LIMIT  0x08020000UL

static void dump_state(hv_vcpu_t vcpu, uint64_t esr, uint64_t ipa) {
    uint64_t pc = 0, lr = 0, cpsr = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC,   &pc);
    hv_vcpu_get_reg(vcpu, HV_REG_LR,   &lr);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
    unsigned ec = (unsigned)((esr >> 26) & 0x3f);

    if (ipa >= GIC_WINDOW_BASE && ipa < GIC_WINDOW_LIMIT) {
        fprintf(stderr,
                "\n[host] stopped at GIC MMIO (IPA 0x%llx). GIC emulation is\n"
                "       the next step, not implemented in this launcher yet.\n"
                "       PC=0x%llx ESR=0x%llx EC=0x%02x\n",
                ipa, pc, esr, ec);
        return;
    }

    fprintf(stderr, "\n=== unhandled VM exit ===\n");
    fprintf(stderr, "  ESR  = 0x%016llx  (EC=0x%02x)\n", esr, ec);
    fprintf(stderr, "  IPA  = 0x%016llx\n", ipa);
    fprintf(stderr, "  PC   = 0x%016llx\n", pc);
    fprintf(stderr, "  LR   = 0x%016llx\n", lr);
    fprintf(stderr, "  CPSR = 0x%016llx\n", cpsr);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <hypervisor.elf>\n", argv[0]);
        return 2;
    }

    /* Feature probe — bail clearly if this Mac doesn't do nested virt. */
    bool el2_supported = false;
    HVC(hv_vm_config_get_el2_supported(&el2_supported));
    if (!el2_supported) {
        fprintf(stderr,
                "This Mac does not support EL2 in an HVF guest.\n"
                "Requires Apple Silicon M3+ on macOS 15 or later.\n");
        return 1;
    }

    /* Allocate host memory that will be the guest's physical RAM.
     * HVF requires page-aligned host VA; mmap() already gives us that. */
    void *ram = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap ram"); return 1; }

    /* Build VM with EL2 enabled, then create the vCPU. */
    hv_vm_config_t vm_cfg = hv_vm_config_create();
    HVC(hv_vm_config_set_el2_enabled(vm_cfg, true));
    HVC(hv_vm_create(vm_cfg));

    HVC(hv_vm_map(ram, GUEST_RAM_BASE, GUEST_RAM_SIZE,
                  HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    uint64_t entry_pc = load_elf_into(ram, GUEST_RAM_SIZE, argv[1]);
    if (entry_pc == 0) entry_pc = HYP_ENTRY_IPA;
    fprintf(stderr, "entry PC = 0x%llx\n", entry_pc);

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit_info = NULL;
    hv_vcpu_config_t vcpu_cfg = hv_vcpu_config_create();
    HVC(hv_vcpu_create(&vcpu, &exit_info, vcpu_cfg));

    /* Initial CPU state:
     *   PC   -> _start at 0x40080000
     *   CPSR -> EL2h (mode 0x9), DAIF masked (0x3C0)  ==> 0x3C9
     * boot.S sets SP itself from __stack_top, so we don't seed SP here. */
    HVC(hv_vcpu_set_reg(vcpu, HV_REG_PC,   entry_pc));
    HVC(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3C9));

    /* QEMU passes DTB pointer in x0 when booting Linux directly. Our
     * hypervisor's _start doesn't read x0; it's safe to leave zero. */

    fprintf(stderr, "Starting vCPU at EL2...\n\n");

    for (;;) {
        HVC(hv_vcpu_run(vcpu));
        switch (exit_info->reason) {
        case HV_EXIT_REASON_EXCEPTION: {
            uint64_t esr = exit_info->exception.syndrome;
            uint64_t ipa = exit_info->exception.physical_address;
            unsigned ec  = (unsigned)((esr >> 26) & 0x3f);

            /* Data abort classes: 0x24 (lower EL) and 0x25 (same EL).
             * When our hypervisor (the "EL2 guest" as HVF sees it) faults
             * on an unmapped IPA, HVF reports it here. */
            if (ec == 0x24 || ec == 0x25) {
                if (handle_data_abort(vcpu, esr, ipa, (const uint8_t *)ram)) {
                    /* ILEN in ESR[25:24] says whether the trapping insn
                     * was 16 or 32 bits. For AArch64 it's always 32. */
                    uint64_t pc = 0;
                    HVC(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc));
                    HVC(hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4));
                    continue;
                }
            }

            dump_state(vcpu, esr, ipa);
            return 1;
        }

        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            /* The host has work to do related to the virtual timer.
             * With no guest timer yet, treat as benign and resume. */
            continue;

        case HV_EXIT_REASON_CANCELED:
            fprintf(stderr, "\n[host] vCPU canceled\n");
            return 0;

        default:
            fprintf(stderr, "\n[host] unknown exit reason %u\n",
                    exit_info->reason);
            return 1;
        }
    }
}
