# Step 7: event groups and ETS timers

Ten of the adapter's named failures are gone. Step 4 left the table with 68
entries waiting on something; these ten were not waiting on an ESP-IDF
component at all, they were waiting on a decision about how to build a
FreeRTOS primitive that RTEMS does not have.

| | before | after |
|---|---:|---:|
| implemented on RTEMS primitives | 49 | 59 |
| waiting on a component not built yet | 69 | 59 |
| `_version`, `_magic` | 2 | 2 |

## What a FreeRTOS event group is

One `uint32_t` of bits shared by everybody who holds the handle, plus a
waiter list. A waiter names a mask and a rule -- *all of* these bits, or *any
of* them -- and blocks until the mask is satisfied or its timeout expires.
`xEventGroupWaitBits` returns the value of the whole group as it stood when
the wait was satisfied, and optionally clears the bits it waited for on the
way out. Several waiters can be blocked on the same group at once, each on a
different mask and a different rule, and one `xEventGroupSetBits` can satisfy
all of them, some of them, or none.

That last sentence is the whole difficulty. It is a broadcast, a
condition-per-waiter, and a read-modify-write of shared state.

## Why RTEMS events are not it

`rtems_event_send` and `rtems_event_receive` look like the same thing: a
32-bit set, a send, a receive with `RTEMS_EVENT_ALL` or `RTEMS_EVENT_ANY`, a
timeout. The rule flags even line up with `wait_for_all_bits`.

They are not the same thing, because **RTEMS events belong to a task.** The
event set lives in the TCB of the receiver and `rtems_event_send` takes an
`rtems_id` for a task. There is no object to hand out as the event group's
handle, and a `set_bits` that has to wake three waiters has nowhere to send
to -- it would have to know who all the waiters are and send each of them a
copy, which is the bookkeeping the event group was supposed to be.

Building it on events would mean keeping a waiter registry beside the bits,
which is strictly more code than keeping the bits under a lock. So:

* a `pthread_mutex_t` guards the bits,
* a `pthread_cond_t` is the waiter list,
* `set_bits` broadcasts.

pthreads were already in the file -- the per-thread semaphore is a POSIX key
-- so this adds no new dependency.

Broadcast rather than signal, deliberately. The waiters are waiting on
different masks; waking one can wake exactly the one this set did not satisfy
and leave a satisfied waiter asleep. `pthread_cond_broadcast` with a re-check
loop is the only correct shape when the predicate differs per waiter.

The wait loop re-checks the bits after `ETIMEDOUT` before giving up, so a set
that lands in the same tick as the deadline is still seen. The condition
variable is created on `CLOCK_MONOTONIC` rather than the default
`CLOCK_REALTIME`: this port sets the clock from SNTP, and a step backwards
there would stretch a 100 ms wait into a long one.

### Limits, stated rather than hidden

**`set_bits` returns the value as of the set, not as of the return.**
FreeRTOS returns it after any waiter it woke has had a chance to take its
bits back out with clear-on-exit. Ours returns the mask that was actually
set, because it is computed under the lock. Nothing in the WiFi libraries
reads this value; if something ever does, the mask that was set is the more
useful of the two answers.

**Deleting a group with waiters is not supported.** `vEventGroupDelete`
unblocks the waiters first; there is no way to do that here and then free the
memory they are about to touch. The callers delete these during teardown,
after the tasks that wait on them are gone.

**Neither `set_bits` nor `clear_bits` may be called from an ISR.** They take
a pthread mutex. ESP-IDF has a separate `xEventGroupSetBitsFromISR` for that
case and this table has no entry for it, so no caller should need one.

**Waiting for no bits returns immediately** with the current mask. Zero bits
can never satisfy the *any of* rule and always satisfies *all of*; FreeRTOS
asserts on it. Returning the mask is the one answer that cannot hang.

## ETS timers on the timer server

`_timer_setfn` / `_timer_arm` / `_timer_arm_us` / `_timer_disarm` /
`_timer_done` are the ROM's `ets_timer_*`, and two things about them shape the
implementation.

**The caller owns the storage.** It passes a pointer to its own `ETSTimer`,
five 32-bit words, often embedded in a larger structure of its own. `setfn`
is the constructor, `done` the destructor, and neither allocates. So the
RTEMS state has to fit *inside* those five words: an `rtems_id`, the callback,
its argument, the interval in ticks, a repeat flag and a 16-bit magic --
exactly 20 bytes, held to that by an `RTEMS_STATIC_ASSERT`.

The magic is what separates an initialised timer from whatever the caller's
memory held before. It earns its place on the re-`setfn` path: the libraries
call `setfn` again on a live timer to change the callback, and creating a
second RTEMS timer for the same `ETSTimer` would leak the first and leave it
firing.

**The callbacks expect to be in a task.** ESP-IDF dispatches them from the
`esp_timer` task, and they take mutexes and post to queues. A plain
`rtems_timer_fire_after` would run them in the clock tick's interrupt context,
where that is not allowed, so these use `rtems_timer_server_fire_after` and
start the timer server on first use. Its priority is the inversion of
ESP-IDF's `esp_timer` priority of 22, by the same rule
`rtems_wifi_task_create` uses, which puts it one step below the WiFi task in
RTEMS' numbering too.

A periodic timer re-arms itself *before* calling the callback, so that a
callback which disarms or frees its own timer -- which these libraries do --
wins rather than being undone by a re-arm behind it. `_timer_disarm` clears
the repeat flag as well as cancelling, for the same reason: a periodic
callback running on the server right now has already re-armed.

### Sub-tick intervals round up to one tick

`_timer_arm` takes milliseconds and `_timer_arm_us` microseconds; RTEMS
timers take ticks. The conversion rounds **up**, with a floor of one tick.

Rounding down is the trap. `rtems_timer_server_fire_after` rejects zero, so a
one-shot armed for 640 us would silently never fire, and a periodic one that
re-armed with zero would either stop or fire every tick forever. Rounding up
costs resolution instead: with the usual 10 ms tick, the ROM's documented
640 us minimum becomes 10 ms. A WiFi timeout that is late is recoverable; a
timer that fires continuously is not, and a timer that never fires is a hang
with no message.

If sub-tick fidelity turns out to matter, the fix is a faster tick
(`CONFIGURE_MICROSECONDS_PER_TICK`) or a rewrite onto a timecounter-driven
mechanism -- not a change to the rounding.

## Configuration this adds

The RTEMS timers are classic API objects, so the application needs
`CONFIGURE_MAXIMUM_TIMERS` large enough for however many `ETSTimer`s the
libraries keep alive, and the timer server needs a task. Running out reports
itself:

```
wifi.osi: out of RTEMS timers, CONFIGURE_MAXIMUM_TIMERS
```

The event groups need no configuration: pthread mutexes and condition
variables are self-contained objects in RTEMS and come out of the heap with
the group.

## What is verified, and what is not

Verified:

* it compiles clean with `-Wall` against the real BSP headers;
* `tools/table-check.py` reports `members 120 initialised 120`, nothing
  missing, extra or duplicated, and still fails when one of the ten new
  initialisers is deleted from a copy.

**Not verified: none of this has ever been called.** The libraries reach the
table through the ROM's `g_osi_funcs_p`, and the test image stalls in PHY
calibration before `esp_wifi_init_internal()` runs, so nothing has yet
reached an event group or an ETS timer. These are compile-correct and
reasoned-correct against the FreeRTOS and ROM contracts; they are not tested.
The failure mode of a wrong `xEventGroupWaitBits` is a hang rather than an
error, so treat the first WiFi bring-up that blocks in a wait as suspicious
of this file before anything else.
