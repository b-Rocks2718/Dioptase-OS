/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef THREADS_H
#define THREADS_H

#include "TCB.h"
#include "queue.h"

extern struct SpinQueue ready_queue;

void threads_init(void);

void block(unsigned was, void (*func)(void *), void *arg);

void thread_entry(void);

void event_loop(void);

void thread(struct Fun* thread_fun);

void bootstrap(void);

void add_tcb(void* tcb);

void yield(void);

void stop(void);

bool disable_preemption(void);

void enable_preemption(bool was);

#endif // THREADS_H
