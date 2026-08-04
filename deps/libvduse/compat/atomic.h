/*
 * Minimal replacement for QEMU's "include/atomic.h", providing only the
 * four barrier macros that libvduse.c actually uses.
 *
 * These mirror the semantics of QEMU's include/qemu/atomic.h. The compiler
 * barrier is paired with the __atomic fence in each case because QEMU's
 * definitions do the same: the fence orders the machine, the barrier stops
 * the compiler from hoisting the vring accesses across it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VDUSE_COMPAT_ATOMIC_H
#define VDUSE_COMPAT_ATOMIC_H

/* Compiler-only barrier. */
#define barrier()   ({ __asm__ __volatile__("" ::: "memory"); })

#define smp_mb()    ({ barrier(); __atomic_thread_fence(__ATOMIC_SEQ_CST); })
#define smp_wmb()   ({ barrier(); __atomic_thread_fence(__ATOMIC_RELEASE); })
#define smp_rmb()   ({ barrier(); __atomic_thread_fence(__ATOMIC_ACQUIRE); })

#endif /* VDUSE_COMPAT_ATOMIC_H */
