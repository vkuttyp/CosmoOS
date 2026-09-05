# Timer Subsystem: API

Headers: `kernel/include/kernel/timer.h` (generic) and
`kernel/include/arch/timer.h` (architecture interface, implemented by
`kernel/arch/x86_64/timer.c`). Internal kernel ABI.

Constants: `CONFIG_HZ` = 250, `NS_PER_SEC`, `TICK_NS` = 4 000 000.

---

## kernel/timer.h

### `void timer_init(void)`
- **Purpose**: calibrate (`arch_timer_calibrate`), compute the
  fixed-point nanosecond multiplier, capture the clock base so
  `clock_now_ns` starts near 0, register the tick handler on
  `arch_timer_vector()` with `interrupt_register`, and start CPU 0's tick
  (`timer_init_cpu`).
- **Lifetime**: once, after `irq_init` (needs the LAPIC), before
  `sched_init`.
- **Failure modes**: panics on calibration failure or if the tick vector
  cannot be registered.

### `void timer_init_cpu(void)`
- Initialise the calling CPU's `struct timer_queue`, point
  `this_cpu()->timers` at it, and `arch_timer_start_tick(CONFIG_HZ)`. APs
  call it during bring-up (SMP PR).

### `uint64_t clock_now_ns(void)`
- **Purpose**: monotonic nanoseconds since `timer_init`.
- **Concurrency**: lock-free; reads the counter and multiplies.
- **Interrupt context**: yes. Returns 0 before `timer_init`.
- **Precision**: `((delta * g_ns_mult) >> 32)` with a 128-bit product;
  relative error below 1e-9.

### `uint64_t clock_hz(void)`, `const char *clock_name(void)`
- Counter frequency and name (`"tsc"` on x86-64).

### `void timer_setup(struct timer *t, timer_fn fn, void *arg)`
- Initialise a caller-owned timer: link, `fn`, `arg`, state `TIMER_IDLE`.

### `void timer_start(struct timer *t, uint64_t delay_ns)`
- **Purpose**: arm `t` to fire at `clock_now_ns() + delay_ns` on the
  calling CPU's queue; records `t->cpu`.
- **Concurrency**: disables interrupts, takes the local queue lock,
  sorted insert (ascending expiry, FIFO on ties).
- **Interrupt context**: allowed (a callback may re-arm its own timer).
- **Failure modes**: panics if `t->state == TIMER_PENDING` (double start)
  or `fn` is NULL. `TIMER_RUNNING` is accepted: a callback may re-arm its
  own timer.
- **Ownership**: the caller owns `t` and must not free it while PENDING
  or RUNNING.

### `bool timer_cancel(struct timer *t)`
- **Purpose**: remove a PENDING timer from the queue of `t->cpu`.
- **Outputs**: true if it was pending (now IDLE); false if it was IDLE or
  RUNNING. A RUNNING timer's callback may still be executing on its CPU;
  the caller must not free it until `timer_cancel_sync`.
- **Interrupt context**: yes.

### `bool timer_cancel_sync(struct timer *t)` *(exported)*
- **Purpose**: cancel and wait until the callback is not running
  anywhere; on return the timer's memory may be freed.
- **Mechanism**: `struct timer_queue.running` names the callback the
  queue's CPU is executing (set before the callback with the lock held,
  cleared after it with the lock re-taken). Holding the queue lock while
  `running != t` proves the callback is not executing; seeing `running
  == t` means it runs on another CPU, and the caller spins
  (`arch_cpu_relax`, no sleep) and cancels again, so a callback that
  re-armed is caught. On the timer's own CPU `running == t` is impossible
  from thread context (callbacks run in the tick interrupt, masked while
  the lock is held) and from the callback itself it panics.
- **Outputs**: what `timer_cancel` would have returned.
- **Context**: any except the timer's own callback. Safe under a spinlock
  as long as the callback never takes that lock (TCP's callbacks take only
  the network work lock; `pcb_free_locked` relies on that).

### `typedef void (*timer_fn)(struct timer *t, void *arg)`
- Callback contract: runs in interrupt context on the arming CPU with
  interrupts disabled and the queue lock released. May call
  `sched_wake`, `waitqueue_wake_*`, `semaphore_up`, `complete`,
  `timer_start`; must not block or allocate.

### `void timer_set_tick_hook(timer_tick_hook_fn hook)`
- Register a function called from the tick on every CPU after expired
  timers ran, with the current `clock_now_ns()`. The scheduler registers
  `sched_tick`. One hook; no locking (set once at init).

### `void ndelay(uint64_t ns)`, `void udelay(uint64_t us)`
- Busy-wait on `clock_now_ns()` with `arch_cpu_relax()`. Usable anywhere
  after `timer_init`, including interrupt context and with interrupts
  disabled; by convention never for more than a few milliseconds outside
  tests.

### `uint64_t timer_ticks(void)`, `unsigned timer_pending_count(void)`
- Diagnostics for the calling CPU: tick count since its timer started;
  number of PENDING timers (under the queue lock).

---

## arch/timer.h

### `void arch_timer_calibrate(void)`
- Boot CPU, once, before the tick: measures the TSC and the LAPIC timer
  (divide 16) against PIT channel 2 over 10 ms; panics if the PIT never
  reaches terminal count or the results are implausible (TSC outside
  100 MHz–10 GHz, LAPIC outside 1 MHz–10 GHz). Allocates the tick vector
  with `arch_vector_alloc`.

### `void arch_timer_start_tick(unsigned hz)` / `void arch_timer_stop_tick(void)`
- Program the calling CPU's LAPIC timer periodic at `hz` on the tick
  vector (`lapic_hz / hz` counts); panics if the count is out of range.

### `unsigned arch_timer_vector(void)`
- The dynamic vector the tick arrives on (asserts calibration ran).

### `uint64_t arch_clock_read(void)`, `uint64_t arch_clock_hz(void)`, `const char *arch_clock_name(void)`
- `lfence; rdtsc`, the calibrated TSC frequency, `"tsc"`.

---

## Private

`kernel/arch/x86_64/pit.c` implements `arch/testhooks.h`
(`arch_test_periodic_irq_start/stop`, PIT channel 0 as an ISA IRQ 0
source) for the `irq-route` self-test only; it is not a timer API.
