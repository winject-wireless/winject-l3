# Reactor teardown and `wake_up` ordering

winject-manager builds on BFC reactors (`bfc::epoll_reactor` on Linux,
`bfc::poll_reactor` on ESP-IDF, and `bfc::cv_reactor` for queue-style upstreams
when enabled). Teardown must not free callback owners or sockets while a reactor
pass might still invoke a registered handler.

This document is the manager-side contract. BFC implementation details live in
the [bfc](https://github.com/winject-wireless/bfc) tree; a longer BFC task
write-up is kept in the bench `aidocs/reactor-deferred-wake-ordering.md`.

## Guarantee (all BFC reactors)

> A `wake_up(cb)` callback runs on the reactor thread **after every registered
> handler of the pass in which it was queued** has returned.

Registered handlers are:

- **epoll / poll:** fd read/write callbacks (and the internal wake fd) for the
  current `epoll_wait` / `poll` batch.
- **cv_reactor:** queue `notify_callback()` dispatches for the current wake
  (only when `m_wakeup_req` is set that iteration).

Deferred `wake_up` callbacks are drained in the **next phase** of the same
reactor iteration, after that batch of registered handlers.

## Reactor turn (one loop iteration)

| Phase | `cv_reactor` | `epoll` / `poll` |
|-------|----------------|------------------|
| 1. Wait | `wait_for` on `m_wakeup_req` (or timeout) | `epoll_wait` / `poll` |
| 2. Registered handlers | If `m_wakeup_req`: snapshot `m_contexts`, `notify_callback()` on ready queues | fd (+ wake fd) callbacks |
| 3. Deferred | Swap out `m_wake_up_cb`, run FIFO | same |
| 4. Cleanup | — | `m_pending_cleanup` (erase fd entries, close dup write fds); poll clears retired callbacks |
| 5. Timers | `m_timer.schedule` | same |

Consequences:

- `wake_up(cb)` queued during phase 2 (any thread) runs in phase 3 of the **same**
  pass, after phase 2 handlers return.
- `wake_up` queued during phases 3–5 (including from another deferred callback
  or a timer) runs in phase 3 of the **next** pass (after that pass's phase 2).
- `wake_up` queued before `run()`: first pass.
- `wake_up` still pending when `stop()` ends the loop: **never runs** (not
  drained by `stop()` or the reactor destructor).
- `wake_up` is **not** a timer fence: FEC and other reactor timers run in phase
  5, after deferred callbacks.

## Teardown recipe

1. **Deregister** (remove read interest / null queue callback).
2. **`wake_up(done)`** on the same reactor.
3. **Release** the callback owner, socket, or queue in `done`, or keep it alive
   inside the `wake_up` closure until `done` runs.

When `done` runs, no handler that could still use the old registration is
running, and none will start for that registration.

### Why cv uses sync `remove_read_rdy` plus a lease `wake_up`

`cv_reactor` copies a snapshot of raw `context*` pointers, then calls
`notify_callback()`. A synchronous `remove_read_rdy` from another thread is
safe **if** a trailing `wake_up` keeps queue storage alive until the snapshot
pass finishes. Without that `wake_up`, freeing the queue when `remove_read_rdy`
returns is a use-after-free.

Do **not** defer cv `remove_read_rdy` onto the reactor: a later deferred remove
can clobber a **new** `add_read_rdy` on the same queue (remove-then-re-add during
hot-swap).

Example (queue upstream CV path in
[`Upstream::unbind_callbacks`](../src/manager/endpoint/Upstream.cpp) when
present):

```cpp
reactor->remove_read_rdy(*host_rx_queue);
reactor->remove_read_rdy(*app_tx_queue);
host_rx_queue->set_reactor(nullptr);
app_tx_queue->set_reactor(nullptr);
auto leases = std::make_shared<...>(host_rx_queue, app_tx_queue);
reactor->wake_up([leases] {});
```

### Current `rem_*_rdy` behavior (BFC)

| Reactor | On reactor thread | From another thread |
|---------|-------------------|---------------------|
| epoll | `rem` inline; **`done_cb` inline** (not a full-batch fence) | `rem` + `done_cb` in phase 3 |
| poll | `rem` inline; `done_cb` via `wake_up` (phase 3) | `rem` + `done_cb` in phase 3 |
| cv `remove_read_rdy` | sync, no `done_cb` | sync, no `done_cb` |

Inline `rem` on the reactor thread is intentional so the caller may close the fd
immediately afterward on embedded `poll()` stacks.

## winject-manager usage

### Main `IOReactor` (Linux epoll)

Most manager code runs on one reactor thread ([`IOReactor`](../src/manager/utils/IOReactor.h)):

| Component | Register | Teardown today |
|-----------|----------|----------------|
| [`UDPUpstream`](../src/manager/endpoint/UDPUpstream.cpp) | `add_read_rdy` on app UDP socket | `teardown()` / `rem_read_rdy(fd, done_cb)`; `done_cb` holds the socket, so it closes after removal |
| [`WifiUdp`](../src/manager/radio/WifiUdp.cpp) | d-plane forward socket | `rem_read_rdy` then close in `close()` |
| [`ConsoleService`](../src/manager/console/ConsoleService.cpp) | manager m-plane socket | `rem_read_rdy` on shutdown |
| [`App`](../src/manager/App.cpp) | console client fd | `rem_read_rdy` when dropping console |

Cross-thread control that must run on the reactor thread should use
`reactor.wake_up(...)`, as [`WifiUdp::post_rx_event`](../src/manager/radio/WifiUdp.cpp)
does for d-plane address changes.

**Rules for new epoll/poll registrations:**

- Prefer `rem_read_rdy(fd, done_cb)` from a **non-reactor** thread when `done_cb`
  must run only after the current I/O batch (proper fence).
- Do not rely on `done_cb` as a fence when `rem_read_rdy` is called **from inside**
  the fd handler on epoll until BFC defers on-thread `done_cb` (poll already
  defers).
- After `rem_read_rdy` from another thread, avoid reusing the same fd number
  until removal has run on the reactor thread, or close the socket from `done_cb`.
  See [Known issues](#known-issues).

### CV upstream reactor (queue upstream path)

When queue upstreams are enabled, `App` runs a dedicated `cv_reactor` thread for
UDP/CV processing while queue upstreams may use a host-registered CV reactor from
BFC's registry. That reactor may be stopped or stuck in a long app handler, so
`Upstream::teardown()` never waits for a reactor fence. It uses the sync remove
plus lease `wake_up` above for queue storage. It then calls `DatagramAir::stop()`
on the calling thread, which is safe from any thread except the CV reactor's:
a data-path mutex waits out an in-flight call, and `DeadlineTimer::unbind`
waits out a firing FEC timer (its token mutex is held while firing). A handler
already dispatched for a removed queue sees `!active()` and returns. The
destructor does the same for paths that never call `teardown()`.

`reactive_circular_lossy_queue::notify_callback` copies the callback and runs
it without `cb_mtx`, so a sync `remove_read_rdy` does **not** wait for a running
handler; the data-path mutex is what fences it.

Instance replacement (`replace_upstream`) calls `Upstream::teardown()` on the IO
thread before `reset()`; `QueueUpstream::teardown()` releases the queue claim.

[`WifiQueue`](../src/manager/radio/WifiQueue.cpp) follows the same rule for its
`wifi_rx` handler and posted RX events: callbacks capture a shared gate, not
`this`. The gate's mutex is held while one runs, so `close()` waits out a
running callback on another thread without a reactor fence, and the destructor
clears the gate's owner so a late callback does nothing. `close()` may run from
inside `on_rx`/`on_idle`.

## Rules for new code

1. Deregister, then `wake_up(done)`; free or reuse resources in `done` or in the
   closure.
2. After sync cv remove, re-`add_read_rdy` on the same queue is allowed
   immediately; use `done` if the **old** handler must finish before the **new**
   one runs.
3. Avoid cv `remove_read_rdy` from inside that queue's own handler on a plain
   `reactive_event_queue` (deadlock on `cb_mtx`). Manager reactive queues are
   `reactive_circular_lossy_queue` (`ReactiveLossyQueue`); still prefer teardown
   from a foreign thread or a `wake_up` callback.
4. Never block shutdown on a `wake_up` `done` if the reactor may already be
   stopped.
5. `wake_up` alone is not a timer fence. Cancel timers on the reactor thread,
   or through a cancel that waits out a firing callback (`DeadlineTimer`), so
   none is running and no handler can re-arm one.

## Known issues

- **UDP upstream replace:** fixed: `UDPUpstream` closes its socket from the
  removal `done_cb`, so the fd number is not reused before the deferred `rem`.
  Legacy path: `close` calls `rem_read_rdy` then closes
  the socket immediately. If `close` runs off the I/O reactor thread, epoll
  removal is deferred while the fd is already closed; a new endpoint can obtain
  the same fd number and lose its registration when the deferred `rem` runs.
  Mitigation: `rem_read_rdy(fd, done_cb)` and close/register in `done_cb`, or
  serialize replace on the reactor thread.
- **epoll self-removal:** removing the fd currently executing can destroy the
  running callback closure; poll uses `retire_cb` instead (BFC follow-up).
- **epoll fd generation:** rem + close + re-add the same fd in one batch can
  deliver one stale event to the new registration (poll guards with generation).

## Related reading

- [winject.md](winject.md) — manager components and threads
- [mplane.md](mplane.md) — runtime `update_upstream` / FEC (in-place on current
  `UdpEndpoint`; full instance replace on queue-upstream branches)
