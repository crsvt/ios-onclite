/*
 * Host test for xnu osfmk/arm64/onclite_lse.c on an LSE-capable Mac (Apple
 * silicon): every LSE/LDAPR form runs once natively (JIT) and once through
 * the emulator from identical random state; memory and registers must match.
 *   cc -O1 -I stub lse_host_test.c -o lse_host_test && ./lse_host_test [N]
 * Register use: Rs (pair) x4/x5, Rt (pair) x6/x7, Rn x8; 31 = XZR forms.
 */
#include <sys/errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/arm/thread_status.h>

int onclite_copy_cas(user_addr_t a, unsigned int s, uint64_t exp, uint64_t des, uint64_t *old);
int onclite_copy_casp(user_addr_t a, unsigned int s, const uint64_t exp[2], const uint64_t des[2], uint64_t old[2]);
int copyin_atomic32(const user_addr_t a, uint32_t *k);
int copyin_atomic64(const user_addr_t a, uint64_t *k);
int proc_selfpid(void) { return 1; }

#include ONCLITE_LSE_C

#define CAS_N(T, a, exp, des, old) do { T e = (T)(exp); \
	__atomic_compare_exchange_n((T *)(a), &e, (T)(des), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	*(old) = e; } while (0)

int
onclite_copy_cas(user_addr_t a, unsigned int s, uint64_t exp, uint64_t des, uint64_t *old)
{
	if (a & ((1u << s) - 1))
		return EINVAL;
	switch (s) {
	case 0: CAS_N(uint8_t, a, exp, des, old); break;
	case 1: CAS_N(uint16_t, a, exp, des, old); break;
	case 2: CAS_N(uint32_t, a, exp, des, old); break;
	default: CAS_N(uint64_t, a, exp, des, old); break;
	}
	return 0;
}

int
onclite_copy_casp(user_addr_t a, unsigned int s, const uint64_t exp[2], const uint64_t des[2], uint64_t old[2])
{
	if (a & ((2u << s) - 1))
		return EINVAL;
	if (s == 2) {
		uint64_t o;
		CAS_N(uint64_t, a, exp[0] | exp[1] << 32, des[0] | des[1] << 32, &o);
		old[0] = (uint32_t)o;
		old[1] = o >> 32;
	} else {
		unsigned __int128 e = exp[0] | (unsigned __int128)exp[1] << 64;
		unsigned __int128 d = des[0] | (unsigned __int128)des[1] << 64;
		__atomic_compare_exchange_n((unsigned __int128 *)a, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		old[0] = (uint64_t)e;
		old[1] = (uint64_t)(e >> 64);
	}
	return 0;
}

int copyin_atomic32(const user_addr_t a, uint32_t *k) { if (a & 3) return EINVAL; *k = __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); return 0; }
int copyin_atomic64(const user_addr_t a, uint64_t *k) { if (a & 7) return EINVAL; *k = __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST); return 0; }

/* x0 = regs[5]: x4 x5 x6 x7 x8 */
__asm__(
	".p2align 2\n"
	".globl _tmpl_start, _tmpl_patch, _tmpl_end\n"
	"_tmpl_start:\n"
	"	mov x9, x0\n"
	"	ldp x4, x5, [x9]\n"
	"	ldp x6, x7, [x9, #16]\n"
	"	ldr x8, [x9, #32]\n"
	"_tmpl_patch:\n"
	"	nop\n"
	"	stp x4, x5, [x9]\n"
	"	stp x6, x7, [x9, #16]\n"
	"	ret\n"
	"_tmpl_end:\n");
extern char tmpl_start[], tmpl_patch[], tmpl_end[];

static uint32_t *jit;
static size_t patch_at;

static void
run_native(uint32_t instr, uint64_t regs[5])
{
	pthread_jit_write_protect_np(0);
	jit[patch_at] = instr;
	pthread_jit_write_protect_np(1);
	sys_icache_invalidate(jit, (size_t)(tmpl_end - tmpl_start));
	((void (*)(uint64_t *))jit)(regs);
}

static uint64_t
rnd(void)
{
	return (uint64_t)arc4random() << 32 | arc4random();
}

int
main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 200000, fails = 0, tested = 0;
	size_t len = (size_t)(tmpl_end - tmpl_start);
	jit = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (jit == MAP_FAILED) {
		perror("mmap MAP_JIT");
		return 2;
	}
	pthread_jit_write_protect_np(0);
	memcpy(jit, tmpl_start, len);
	pthread_jit_write_protect_np(1);
	patch_at = (size_t)(tmpl_patch - tmpl_start) / 4;

	static uint8_t bufA[64] __attribute__((aligned(64))), bufB[64] __attribute__((aligned(64)));
	const char *names[] = { "CAS", "CASP", "LDOP", "SWP", "LDAPR" };
	long per_class[5] = { 0 };

	for (long it = 0; it < n; it++) {
		unsigned cls = arc4random_uniform(5), size = arc4random_uniform(4);
		unsigned rs = 4, rt = 6, rn = 8, opc = 0;
		uint32_t instr;
		unsigned access = 1u << size;

		if (cls == 1) {
			size = 2 + arc4random_uniform(2);	/* CASP: W or X pairs */
			access = 2u << size;
		}
		if ((cls == 2 || cls == 3) && arc4random_uniform(5) == 0)
			rt = 31;			/* ST<op> / discard */
		if (cls == 2 && arc4random_uniform(8) == 0)
			rs = 31;			/* XZR operand */
		switch (cls) {
		case 0:
			instr = 0x08a07c00 | size << 30 | arc4random_uniform(2) << 22 | arc4random_uniform(2) << 15 |
			    rs << 16 | rn << 5 | rt;
			break;
		case 1:
			instr = 0x08207c00 | (size == 3) << 30 | arc4random_uniform(2) << 22 | arc4random_uniform(2) << 15 |
			    rs << 16 | rn << 5 | rt;
			break;
		case 2:
			opc = arc4random_uniform(8);
			instr = 0x38200000 | size << 30 | arc4random_uniform(2) << 23 | arc4random_uniform(2) << 22 |
			    rs << 16 | opc << 12 | rn << 5 | rt;
			break;
		case 3:
			instr = 0x38208000 | size << 30 | arc4random_uniform(2) << 23 | arc4random_uniform(2) << 22 |
			    rs << 16 | rn << 5 | rt;
			break;
		default:
			rs = 31;
			instr = 0x38bfc000 | size << 30 | rn << 5 | rt;
			break;
		}

		for (int i = 0; i < 64; i += 8)
			*(uint64_t *)(bufA + i) = rnd();
		unsigned off = arc4random_uniform(64 / access) * access;
		uint64_t r[4] = { rnd(), rnd(), rnd(), rnd() };
		/* CAS/CASP: often make the compare succeed */
		if (cls == 0 && arc4random_uniform(2)) {
			uint64_t m;
			memcpy(&m, bufA + off, 8 > 64 - off ? 64 - off : 8);
			r[0] = m;
		}
		if (cls == 1 && arc4random_uniform(2)) {
			unsigned w = access / 2;
			r[0] = 0;
			r[1] = 0;
			memcpy(&r[0], bufA + off, w);
			memcpy(&r[1], bufA + off + w, w);
		}
		/* small operands sometimes, so signed/unsigned min/max cross over */
		if (cls == 2 && arc4random_uniform(3) == 0)
			r[0] = (uint64_t)(int64_t)(int8_t)arc4random();
		memcpy(bufB, bufA, 64);

		uint64_t regs[5] = { r[0], r[1], r[2], r[3], (uint64_t)(bufA + off) };
		run_native(instr, regs);

		arm_saved_state_t st;
		memset(&st, 0, sizeof(st));
		st.x[4] = r[0]; st.x[5] = r[1]; st.x[6] = r[2]; st.x[7] = r[3];
		st.x[8] = (uint64_t)(bufB + off);
		st.pc = 0x1000;
		boolean_t ok = onclite_emulate_lse(&st, instr);

		int bad = !ok || st.pc != 0x1004 || memcmp(bufA, bufB, 64) ||
		    st.x[4] != regs[0] || st.x[5] != regs[1] || st.x[6] != regs[2] || st.x[7] != regs[3];
		tested++;
		per_class[cls]++;
		if (bad && fails++ < 10) {
			printf("FAIL %s instr=%08x size=%u opc=%u ok=%d\n", names[cls], instr, size, opc, ok);
			printf("  native x4-x7 %016llx %016llx %016llx %016llx\n", regs[0], regs[1], regs[2], regs[3]);
			printf("  emul   x4-x7 %016llx %016llx %016llx %016llx\n", st.x[4], st.x[5], st.x[6], st.x[7]);
			printf("  memory %s\n", memcmp(bufA, bufB, 64) ? "DIFFERS" : "same");
		}
	}

	/* Not LSE: must be refused and leave the state alone. */
	const uint32_t not_lse[] = { 0xd503201f /* nop */, 0x885f7d00 /* ldxr w0,[x8] */,
				     0xc85ffd00 /* ldaxr x0,[x8] */, 0xb9400100 /* ldr w0,[x8] */ };
	for (unsigned i = 0; i < sizeof(not_lse) / sizeof(not_lse[0]); i++) {
		arm_saved_state_t st = { .pc = 0x1000 };
		st.x[8] = (uint64_t)bufB;
		if (onclite_emulate_lse(&st, not_lse[i]) || st.pc != 0x1000) {
			printf("FAIL accepted non-LSE %08x\n", not_lse[i]);
			fails++;
		}
	}
	printf("%ld cases (CAS %ld, CASP %ld, LDOP %ld, SWP %ld, LDAPR %ld), %ld failures\n",
	    tested, per_class[0], per_class[1], per_class[2], per_class[3], per_class[4], fails);
	return fails != 0;
}
