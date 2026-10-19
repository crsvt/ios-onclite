/* Host test stand-in for xnu's arm_saved_state accessors (64-bit only). */
#pragma once
#include <stdint.h>
typedef int boolean_t;
#define TRUE 1
#define FALSE 0
typedef int64_t register_t;
typedef uint64_t user_addr_t;
typedef struct {
	uint64_t x[31];	/* x0-x30 (x29 fp, x30 lr) */
	uint64_t sp, pc;
} arm_saved_state_t;
static inline register_t get_saved_state_reg(const arm_saved_state_t *s, unsigned r) { return (register_t)s->x[r]; }
static inline void set_saved_state_reg(arm_saved_state_t *s, unsigned r, register_t v) { s->x[r] = (uint64_t)v; }
static inline register_t get_saved_state_sp(const arm_saved_state_t *s) { return (register_t)s->sp; }
static inline register_t get_saved_state_pc(const arm_saved_state_t *s) { return (register_t)s->pc; }
static inline void add_saved_state_pc(arm_saved_state_t *s, int d) { s->pc += (uint64_t)d; }
