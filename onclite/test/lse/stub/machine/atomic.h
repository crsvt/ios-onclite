#pragma once
#define os_atomic_inc(p, m) (__c11_atomic_fetch_add((p), 1, __ATOMIC_RELAXED) + 1)
