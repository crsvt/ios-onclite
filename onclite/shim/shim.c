/*
 * onclite (Redmi 7, SDM632) boot shim: act as iBoot for the ONCLITE xnu.
 *
 * LK enters on cpu0 (Kryo 250 Silver, no 16K granule) at EL1, MMU and
 * caches off, x0 = DTB. tools/mkpayload.py already resolved the kernel's
 * chained fixups and built the ADT (tools/mkadt.py --onclite), so this only
 * has to: find the payload in the LK ramdisk, copy the segments to
 * phys_base + (vmaddr - virt_base), place the ADT and boot_args behind the
 * kernel, and start xnu on the first Gold core (MPIDR 0x100) through PSCI
 * CPU_ON with x0 = boot_args (physical). cpu0 then switches itself off.
 * This hand-off is what probe v5 verified (NOTES.md).
 *
 * There is no screen (LK turns the panel off for `fastboot boot`): every
 * line goes into the ramoops console zone, and xnu continues the record.
 * Read it from the next Linux boot: /sys/fs/pstore/console-ramoops-0.
 */
#include <stdint.h>
#include "../../shim/fdt.h"

#define IMEM_RESTART_REASON	0x0860065cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x004ab000UL

#define RAMOOPS_CONSOLE		0x91400000UL
#define RAMOOPS_CONSOLE_SIZE	0x200000UL
#define PRZ_SIG			0x43474244U	/* "DBGC", fs/pstore/ram_core.c */

#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define XNU_BOOT_MPIDR		0x100		/* first Gold core: 16K granule */
#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define dsb() __asm__ volatile("dsb sy" ::: "memory")

static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

/* ---- ramoops console: struct persistent_ram_buffer { sig, start, size, data[] } ---- */

static uint32_t plog_len;

static void plog_init(void)
{
	wr32(RAMOOPS_CONSOLE, PRZ_SIG);
	wr32(RAMOOPS_CONSOLE + 4, 0);
	wr32(RAMOOPS_CONSOLE + 8, 0);
	plog_len = 0;
}

static void plog_str(const char *s)
{
	while (*s && plog_len < RAMOOPS_CONSOLE_SIZE - 12)
		wr8(RAMOOPS_CONSOLE + 12 + plog_len++, *s++);
	/* no wrap: write position (start) == size */
	wr32(RAMOOPS_CONSOLE + 4, plog_len);
	wr32(RAMOOPS_CONSOLE + 8, plog_len);
	dsb();
}

static void plog_hex(const char *label, uint64_t v)
{
	char buf[19] = "0x";
	for (int i = 0; i < 16; i++) {
		unsigned d = (v >> (4 * (15 - i))) & 0xf;
		buf[2 + i] = d < 10 ? '0' + d : 'a' + d - 10;
	}
	buf[18] = 0;
	plog_str("SHIM ");
	plog_str(label);
	plog_str(" ");
	plog_str(buf);
	plog_str("\n");
}

static void plog_line(const char *s)
{
	plog_str("SHIM ");
	plog_str(s);
	plog_str("\n");
}

/* ---- PSCI ---- */

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void delay_ms(unsigned ms)
{
	__asm__ volatile("isb" ::: "memory");
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

static void delay_us(unsigned us)
{
	__asm__ volatile("isb" ::: "memory");
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000000 * us + 1;
	while (rsr("cntpct_el0") < end)
		;
}

static void __attribute__((noreturn)) fail(const char *why)
{
	plog_str("SHIM FAIL ");
	plog_str(why);
	plog_str("\nSHIM reset to fastboot\n");
	delay_ms(500);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	dsb();
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/*
 * ---- Gold cluster clock ----
 * LK runs on Silver and leaves the Gold (perf, C1) mux on XO: xnu measured
 * ~19.2 MHz on the Gold cores (H1). Before CPU_ON, switch it to GPLL0 /1 =
 * 800 MHz, as Linux clock-cpu-sdm632.c does before the Gold cores come up
 * (and lk2nd): no voltage change. Faster needs the C1 PLL plus a VDD_APC
 * (PM8953 S5) check; the state for that is logged here.
 * .claude/research/gold-clock-report.md.
 */
#define GOLD_MUX_CMD	0x0b011050UL
#define GOLD_MUX_CFG	0x0b011054UL
#define GOLD_CBCR	0x0b011058UL
#define GOLD_PLL	0x0b016000UL
#define GPLL0_MODE	0x01821000UL
#define GPLL_ENA_VOTE	0x01845000UL
#define APC_SAW_VLEVEL	0x0b1d2c18UL
#define APC_SAW_VERSION	0x0b1d2fd0UL
#define SPEED_BIN_FUSE	0x000a4124UL

static void gold_clock_log(const char *when)
{
	plog_line(when);
	plog_hex("GOLD mux cmd", rd32(GOLD_MUX_CMD));
	plog_hex("GOLD mux cfg", rd32(GOLD_MUX_CFG));
	plog_hex("GOLD cbcr", rd32(GOLD_CBCR));
}

static void gold_clock(void)
{
	gold_clock_log("GOLD clock before");
	plog_hex("GOLD pll mode", rd32(GOLD_PLL));
	plog_hex("GOLD pll L", rd32(GOLD_PLL + 0x08));
	plog_hex("GPLL0 mode", rd32(GPLL0_MODE));
	plog_hex("APC SAW version", rd32(APC_SAW_VERSION));
	plog_hex("APC SAW vlevel", rd32(APC_SAW_VLEVEL));
	plog_hex("SPEED bin fuse", rd32(SPEED_BIN_FUSE));
	if (!(rd32(GPLL0_MODE) & (1u << 30))) {
		wr32(GPLL_ENA_VOTE, rd32(GPLL_ENA_VOTE) | 1);
		dsb();
		for (int i = 0; i < 200 && !(rd32(GPLL0_MODE) & (1u << 30)); i++)
			delay_us(1);
		if (!(rd32(GPLL0_MODE) & (1u << 30))) {
			plog_line("GOLD GPLL0 not active: clock left as is");
			return;
		}
	}
	wr32(GOLD_MUX_CFG, 0x401);	/* source 4 = GPLL0, divider /1 */
	dsb();
	wr32(GOLD_MUX_CMD, rd32(GOLD_MUX_CMD) | 1);	/* UPDATE */
	dsb();
	int i;
	for (i = 0; i < 500 && (rd32(GOLD_MUX_CMD) & 1); i++)
		delay_us(1);
	if (rd32(GOLD_MUX_CMD) & 1)
		plog_line("GOLD mux UPDATE did not clear");
	wr32(GOLD_CBCR, rd32(GOLD_CBCR) | 1);
	dsb();
	gold_clock_log("GOLD clock after (GPLL0 800 MHz)");
}

/* ---- exceptions: anything here is a shim bug ---- */

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	plog_hex("EXCEPTION slot", slot);
	plog_line(stage);
	plog_hex("ESR", rsr("esr_el1"));
	plog_hex("ELR", rsr("elr_el1"));
	plog_hex("FAR", rsr("far_el1"));
	plog_hex("X0", regs[0]);
	fail("exception");
}

/* ---- payload (tools/mkpayload.py) and boot_args (pexpert/pexpert/arm64/boot.h) ---- */

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

/* Whole-word boot-arg match in the payload's command line (no libc here). */
static int cmdline_has(const char *line, const char *arg)
{
	for (int i = 0; i < BOOT_LINE_LENGTH && line[i]; i++) {
		if (i && line[i - 1] != ' ')
			continue;
		int j = 0;
		while (arg[j] && i + j < BOOT_LINE_LENGTH && line[i + j] == arg[j])
			j++;
		if (!arg[j] && (i + j >= BOOT_LINE_LENGTH || line[i + j] == ' ' || !line[i + j]))
			return 1;
	}
	return 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

void shim_main(uint64_t dtb)
{
	uint64_t rd_start, rd_end;

	stage = "start";
	plog_init();
	plog_line("onclite shim v1");
	plog_hex("DTB", dtb);
	plog_hex("MPIDR", rsr("mpidr_el1"));

	stage = "initrd";
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		fail("no initrd in /chosen");
	plog_hex("INITRD lo", rd_start);
	plog_hex("INITRD hi", rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		fail("no SWEETXNU payload");
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		fail("bad payload header");

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	plog_hex("PHYS base", p->phys_base);
	plog_hex("PHYS size", p->mem_size);
	plog_hex("VIRT base", p->virt_base);
	plog_hex("ENTRY va", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		fail("ramdisk overlaps kernel memory");

	stage = "segments";
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	plog_hex("SEGS n", p->nsegs);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	stage = "boot_args";
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/*
	 * No boot video: the panel is off and the splash is RGB888, while xnu's
	 * console and AppleCLCD expect 32bpp. Zero keeps both away from it.
	 */
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	plog_hex("BOOTARG pa", ba_pa);
	plog_hex("TOP", top);

	/*
	 * Start xnu on the Gold core: PSCI CPU_ON enters it at EL1 with the MMU
	 * and caches off and x0 = context id = boot_args, as __start expects.
	 * Everything above was written with the MMU and caches off.
	 */
	stage = "gold clock";
	int gold_clk = 1;
	if (cmdline_has(p->cmdline, "onclite_goldclk=0"))
		gold_clk = 0;
	if (gold_clk)
		gold_clock();
	else
		plog_line("GOLD clock left on XO (onclite_goldclk=0)");

	stage = "handoff";
	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	plog_hex("JUMP pa", entry_pa);
	/* Last line: once CPU_ON succeeds xnu may append to this record at any time. */
	plog_line("CPU_ON 0x100 -> xnu, then cpu0 CPU_OFF");
	dsb();
	__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");
	int64_t r = smc(PSCI_CPU_ON, XNU_BOOT_MPIDR, entry_pa, ba_pa);
	if (r != 0) {
		plog_hex("CPU_ON ret", (uint64_t)r);
		fail("CPU_ON 0x100");
	}
	/* 0 = ON, 1 = OFF, 2 = ON_PENDING */
	uint64_t st = 2;
	for (int i = 0; i < 100 && st != 0; i++) {
		delay_ms(1);
		st = smc(PSCI_AFFINITY_INFO, XNU_BOOT_MPIDR, 0, 0);
	}
	if (st != 0) {
		plog_hex("AFFINITY 0x100", st);
		fail("Gold core did not come up");
	}
	smc(PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}
