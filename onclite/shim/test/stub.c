/* Appends to the shim's ramoops record what xnu would see, then resets. */
#include <stdint.h>

#define RAMOOPS_CONSOLE		0x91400000UL
#define RAMOOPS_CONSOLE_SIZE	0x200000UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	uint64_t video[6];
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[608];
	uint64_t bootFlags, memSizeActual;
};

static void put(const char *s)
{
	volatile uint32_t *h = (volatile uint32_t *)RAMOOPS_CONSOLE;
	uint32_t len = h[2];
	while (*s && len < RAMOOPS_CONSOLE_SIZE - 12)
		*(volatile uint8_t *)(RAMOOPS_CONSOLE + 12 + len++) = *s++;
	h[1] = len;
	h[2] = len;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void hex(const char *label, uint64_t v)
{
	char buf[19] = "0x";
	for (int i = 0; i < 16; i++) {
		unsigned d = (v >> (4 * (15 - i))) & 0xf;
		buf[2 + i] = d < 10 ? '0' + d : 'a' + d - 10;
	}
	buf[18] = 0;
	put("STUB ");
	put(label);
	put(" ");
	put(buf);
	put("\n");
}

void stub_main(uint64_t x0)
{
	const volatile struct boot_args *ba = (const volatile struct boot_args *)x0;
	put("STUB gold core entered\n");
	hex("x0", x0);
	hex("MPIDR", rsr("mpidr_el1"));
	hex("EL", rsr("CurrentEL") >> 2);
	hex("SCTLR", rsr("sctlr_el1"));
	hex("Revision", ba->Revision);
	hex("physBase", ba->physBase);
	hex("memSize", ba->memSize);
	hex("top", ba->topOfKernelData);
	/* deviceTreeP is a VA; PA = VA - virtBase + physBase */
	uint64_t adt_pa = ba->deviceTreeP - ba->virtBase + ba->physBase;
	hex("ADT pa", adt_pa);
	hex("ADT len", ba->deviceTreeLength);
	hex("ADT word0", *(volatile uint32_t *)adt_pa);
	/* cmdline is NUL padded */
	put("STUB cmdline ");
	char c[64];
	int i;
	for (i = 0; i < 63 && ba->CommandLine[i]; i++)
		c[i] = ba->CommandLine[i];
	c[i] = 0;
	put(c);
	put("\nSTUB done, reset\n");
	register uint64_t r0 __asm__("x0") = PSCI_SYSTEM_RESET;
	__asm__ volatile("smc #0" : "+r"(r0) : : "memory");
}
