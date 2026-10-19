/*
 * onclite (Redmi 7, SDM632) feasibility probe. One `fastboot boot` answers
 * the questions that decide the XNU port:
 *   - CPU feature registers of both clusters (16K granule, atomics, FP16)
 *   - PSCI version and features, CPU_ON for the 7 secondaries
 *   - GICv2 (QGIC2) registers and a timer IRQ through GICC
 *   - where LK's continuous-splash framebuffer is (read from the MDP5 pipes)
 * Everything runs with the MMU off (Device memory), so all accesses are
 * aligned (-mstrict-align). Text is white/grey only so RGB vs BGR does not
 * matter.
 *
 * v1 left the panel dark after `fastboot boot`, so every line also goes to
 * the ramoops console zone (DT ramoops@91400000, console-size 2M, no dump
 * zones). The probe then reboots into recovery, where the previous boot's
 * text is /sys/fs/pstore/console-ramoops-0. The MDP read comes last.
 */
#include <stdint.h>
#include "../../shim/font.h"

#define IMEM_RESTART_REASON	0x0860065cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define RESTART_RECOVERY	0x77665502U
#define PSHOLD			0x004ab000UL

#define MDP_BASE		0x01a00000UL
#define SSPP_SRC_SIZE		0x00
#define SSPP_OUT_SIZE		0x0c
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT		0x30
#define SPLASH_LO		0x90000000UL
#define SPLASH_HI		0x91400000UL	/* splash_region@90001000 + 0x13ff000 */

#define GICD_BASE		0x0b000000UL
#define GICC_BASE		0x0b002000UL
#define GICD_CTLR		0x000
#define GICD_TYPER		0x004
#define GICD_IIDR		0x008
#define GICD_ISENABLER0		0x100
#define GICD_ICENABLER0		0x180
#define GICD_ISPENDR0		0x200
#define GICD_IPRIORITYR		0x400
#define GICC_CTLR		0x000
#define GICC_PMR		0x004
#define GICC_IAR		0x00c
#define GICC_EOIR		0x010
#define GICC_IIDR		0x0fc
#define PTIMER_INTID		19	/* DT timer: <1 3 ...> = PPI 3, non-secure phys */
#define VTIMER_INTID		20	/* <1 4 ...> = PPI 4, virtual */

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL

#define RAMOOPS_CONSOLE		0x91400000UL
#define RAMOOPS_CONSOLE_SIZE	0x200000UL
#define PRZ_SIG			0x43474244U	/* "DBGC", fs/pstore/ram_core.c */

#define NR_CPUS			8
#define RESET_AFTER_S		3

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb() __asm__ volatile("dsb sy" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

/* ---- framebuffer console ---- */

#define SCALE		2
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		16
#define LABEL_COLS	7
#define FG		0xff
#define DIM		0x90
#define BG		0x18

static uintptr_t fb;
static uint32_t fb_w, fb_h, fb_stride, fb_bpp;
static int row, fb_ok;

/* struct persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; } */
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

static void px(uint32_t x, uint32_t y, uint8_t g)
{
	uintptr_t p = fb + y * fb_stride + x * fb_bpp;
	if (fb_bpp == 4) {
		wr32(p, 0xff000000U | g * 0x010101U);
	} else {
		for (uint32_t i = 0; i < fb_bpp; i++)
			wr8(p + i, g);
	}
}

static void con_clear(void)
{
	for (uint32_t y = 0; y < fb_h; y++)
		for (uint32_t x = 0; x < fb_w; x++)
			px(x, y, BG);
	row = 0;
}

static void put_char(int col, char ch, uint8_t g)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *gl = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > fb_w || y0 + CH_H > fb_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint8_t c = (gl[gy] & (0x80 >> gx)) ? g : BG;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					px(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
}

static int put_str(int col, const char *s, uint8_t g)
{
	plog_str(s);
	if (!fb_ok)
		return col;
	while (*s)
		put_char(col++, *s++, g);
	return col;
}

static void newline(void)
{
	plog_str("\n");
	row++;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	for (int i = 0; i < digits; i++) {
		unsigned d = (v >> (4 * (digits - 1 - i))) & 0xf;
		buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
	}
	buf[digits] = 0;
}

static void line(const char *label, const char *s)
{
	put_str(0, label, DIM);
	plog_str(": ");
	put_str(LABEL_COLS, s, FG);
	newline();
}

static void hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	line(label, buf);
}

/* label, then "a=xxxxxxxx b=xxxxxxxx" pairs of 32-bit values */
static void hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, DIM);
	plog_str(": ");
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, DIM);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, FG);
	if (b) {
		plog_str(" ");
		col = put_str(col + 1, b, DIM);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, FG);
	}
	newline();
}

/*
 * LK leaves one MDP5 source pipe scanning out the splash. Take its address,
 * stride and bytes per pixel; fall back to the DT splash region as RGB888.
 */
static int fb_pipe = -1;
static uint32_t fb_fmt;

static void fb_find(void)
{
	static const uint32_t pipes[] = { 0x05000, 0x15000, 0x17000, 0x25000 };	/* VIG0 RGB0 RGB1 DMA0 */

	fb = 0x90001000UL;
	fb_w = 720;
	fb_h = 1520;
	fb_bpp = 3;
	fb_stride = fb_w * fb_bpp;
	for (int i = 0; i < 4; i++) {
		uintptr_t p = MDP_BASE + pipes[i];
		uint32_t a = rd32(p + SSPP_SRC0_ADDR);
		if (a < SPLASH_LO || a >= SPLASH_HI)
			continue;
		uint32_t sz = rd32(p + SSPP_SRC_SIZE), st = rd32(p + SSPP_SRC_YSTRIDE0);
		uint32_t fmt = rd32(p + SSPP_SRC_FORMAT), bpp = ((fmt >> 9) & 3) + 1;
		uint32_t w = sz & 0xffff, h = sz >> 16;
		if (bpp < 3 || w < 320 || h < 320 || (st & 0xffff) < w * bpp)
			continue;
		fb = a;
		fb_w = w;
		fb_h = h;
		fb_bpp = bpp;
		fb_stride = st & 0xffff;
		fb_pipe = i;
		fb_fmt = fmt;
		return;
	}
}

/* ---- SMC / time ---- */

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

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset_to(uint32_t reason, unsigned after_s)
{
	line("RESET", reason == RESTART_RECOVERY ? "to recovery" : "to fastboot");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, reason);
	dsb();
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile uint32_t irq_src = 1023;

static void irq_handler(void)
{
	uint32_t iar = rd32(GICC_BASE + GICC_IAR), id = iar & 0x3ff;

	if (id >= 1020)
		return;	/* spurious */
	irq_last = id;
	if (id == irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wr32(GICC_BASE + GICC_EOIR, iar);
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	newline();
	switch (slot & 3) {
	case 0: line("PANIC", "SYNC"); break;
	case 1: line("PANIC", "IRQ"); break;
	case 2: line("PANIC", "FIQ"); break;
	default: line("PANIC", "SERROR"); break;
	}
	hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	line("STAGE", stage);
	hex("ESR", rsr("esr_el1"));
	hex("ELR", rsr("elr_el1"));
	hex("FAR", rsr("far_el1"));
	hex("X0", regs[0]);
	reset_to(RESTART_RECOVERY, RESET_AFTER_S);
}

/* ---- CPU features ---- */

struct cpu_regs {
	uint64_t mpidr, midr, mmfr0, isar0, pfr0, done;
};
static volatile struct cpu_regs cpu[NR_CPUS];

static void read_regs(volatile struct cpu_regs *r)
{
	r->mpidr = rsr("mpidr_el1");
	r->midr = rsr("midr_el1");
	r->mmfr0 = rsr("id_aa64mmfr0_el1");
	r->isar0 = rsr("id_aa64isar0_el1");
	r->pfr0 = rsr("id_aa64pfr0_el1");
	dsb();
	r->done = 1;
}

void secondary_main(uint64_t idx)
{
	read_regs(&cpu[idx]);
	dsb();
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void features(void)
{
	stage = "features";
	read_regs(&cpu[0]);
	uint64_t m0 = cpu[0].mmfr0, i0 = cpu[0].isar0, p0 = cpu[0].pfr0;
	hex("MIDR", cpu[0].midr);
	hex("MPIDR", cpu[0].mpidr);
	hex2("EL", "cur=", rsr("CurrentEL") >> 2, "sctlr=", rsr("sctlr_el1"));
	hex("MMFR0", m0);
	hex("MMFR1", rsr("id_aa64mmfr1_el1"));
	hex("MMFR2", rsr("id_aa64mmfr2_el1"));
	hex("ISAR0", i0);
	hex("ISAR1", rsr("id_aa64isar1_el1"));
	hex("PFR0", p0);
	hex2("TIMER", "frq=", rsr("cntfrq_el0"), 0, 0);
	/* TGran16 [23:20]: 0 = no; TGran4 [31:28] and TGran64 [27:24]: 0 = yes */
	line("16K", ((m0 >> 20) & 0xf) ? "16K GRANULE: YES" : "16K GRANULE: NO");
	line("4K", ((m0 >> 28) & 0xf) == 0 ? "4K granule: yes" : "4K granule: no");
	line("LSE", ((i0 >> 20) & 0xf) >= 2 ? "atomics: yes" : "atomics: no");
	line("FP16", ((p0 >> 16) & 0xf) == 1 ? "fp16: yes" : "fp16: no");
}

static void psci(void)
{
	stage = "psci";
	uint32_t v = smc(PSCI_VERSION, 0, 0, 0);
	hex2("PSCI", "ver=", v, "mig=", smc(PSCI_MIGRATE_INFO_TYPE, 0, 0, 0));
	if (v >= 0x10000) {
		hex2("FEAT", "susp=", smc(PSCI_FEATURES, PSCI_CPU_SUSPEND, 0, 0),
		     "sysusp=", smc(PSCI_FEATURES, PSCI_SYSTEM_SUSPEND, 0, 0));
		hex2("FEAT", "rst2=", smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0), 0, 0);
	}
}

static void smp(void)
{
	stage = "smp";
	static const uint32_t mpidr[NR_CPUS] = { 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 };
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, mpidr[i], (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (!cpu[i].done) {
			hex2(label, "ret=", (uint32_t)ret[i], "none", 0);
			continue;
		}
		/* mmfr0 bits 31:16 hold TGran4/64/16 and BigEndEL0; isar0 bits 23:20 atomics */
		hex2(label, "mpidr=", cpu[i].mpidr, "m0=", (uint32_t)cpu[i].mmfr0);
		if (i == 4)
			hex2("  A73?", "midr=", cpu[i].midr, "is0=", (uint32_t)cpu[i].isar0);
	}
}

static void gic_timer(int intid)
{
	irq_src = intid;
	irq_count = irq_other = 0;
	wr32(GICD_BASE + GICD_ICENABLER0, 0xffffffff);
	wr8(GICD_BASE + GICD_IPRIORITYR + intid, 0xa0);
	wr32(GICD_BASE + GICD_ISENABLER0, 1U << intid);
	if (intid == VTIMER_INTID) {
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);
		wsr("cntv_ctl_el0", 1);
	} else {
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	delay_ms(300);
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	wr32(GICD_BASE + GICD_ICENABLER0, 1U << intid);
	isb();
}

static void gic(void)
{
	stage = "gic";
	hex2("GICD", "ctlr=", rd32(GICD_BASE + GICD_CTLR), "typer=", rd32(GICD_BASE + GICD_TYPER));
	hex2("GICD", "iidr=", rd32(GICD_BASE + GICD_IIDR), "pend0=", rd32(GICD_BASE + GICD_ISPENDR0));
	hex2("GICC", "ctlr=", rd32(GICC_BASE + GICC_CTLR), "iidr=", rd32(GICC_BASE + GICC_IIDR));
	/* Non-secure view: bit0 enables Group 1 in both. */
	wr32(GICD_BASE + GICD_CTLR, rd32(GICD_BASE + GICD_CTLR) | 1);
	wr32(GICC_BASE + GICC_PMR, 0xff);
	wr32(GICC_BASE + GICC_CTLR, rd32(GICC_BASE + GICC_CTLR) | 1);
	hex2("GIC on", "d=", rd32(GICD_BASE + GICD_CTLR), "c=", rd32(GICC_BASE + GICC_CTLR));
	stage = "vtimer irq";
	gic_timer(VTIMER_INTID);
	hex2("VTIMER", "n=", irq_count, "other=", irq_other);
	stage = "ptimer irq";
	gic_timer(PTIMER_INTID);
	hex2("PTIMER", "n=", irq_count, "other=", irq_other);
	hex2("", "last=", irq_last, "(30 = ok)", 0);
}

void probe_main(uint64_t dtb)
{
	stage = "start";
	plog_init();
	line("ONCLITE", "probe v2");
	hex("DTB", dtb);
	features();
	psci();
	smp();
	gic();
	stage = "mdp read";
	line("STAGE", stage);
	fb_find();
	hex2("FB", "addr=", fb, "pipe=", fb_pipe);
	hex2("FB", "w=", fb_w, "h=", fb_h);
	hex2("FB", "strd=", fb_stride, "fmt=", fb_fmt);
	stage = "fb draw";
	line("STAGE", stage);
	fb_ok = 1;
	con_clear();
	line("ONCLITE", "probe v2 done");
	line("DONE", "all stages");
	reset_to(RESTART_RECOVERY, RESET_AFTER_S);
}
