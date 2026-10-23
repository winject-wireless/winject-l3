# Reactor-driven TX scheduling

TxMux shares its owner's reactor. It creates no thread and performs no blocking
wait or sleep. `start(reactor)` binds wake, arm and cancel operations to either an
IO reactor or a `bfc::cv_reactor<>`; `stop()` cancels pending pacing work and
invalidates callbacks already extracted by the reactor. The reactor must outlive
stop. The owner serializes start/stop on that reactor thread, or while stopped.

App currently binds TxMux to its manager IO reactor. Queue upstream ingress can
run on the host CV reactor and notify the IO reactor. This removes the dedicated
`winject_tx` thread; it does not convert radio data/management sockets to queues
or introduce an upstream RX queue. A fully queue-based deployment can bind the
same TxMux to a CV reactor when the remaining transports support that model.

## Notifications and pacing

Upstream TX queues remain bounded, non-reactive deques guarded by their existing
mutex. The producer requests a tick when encoded/raw output first becomes
available. Requests coalesce into one pending reactor wake. Request work after
releasing queue/table locks, because a callback may be using those locks.

Startup discovers prequeued data. App explicitly requests work after adding or
removing an upstream and after scheduling/configuration changes; queued radio
address changes wake TxMux too. There is no former 5 ms fallback poll. A custom
producer that pushes without notifying can leave data waiting indefinitely.

Each callback applies pending control events, schedules and builds MPDUs, and
submits them through WifiUdp. Pacing still uses:

```
(whole-frame airtime, including FCS, + modulation gap) / 90%
```

The previous credit of up to two full-frame intervals remains. A callback emits
at most eight MPDUs before yielding; if catch-up work remains due, it schedules
another timer turn. If pacing closes, one timer is armed for the next deadline.
No pacing timer remains once there is no sendable backlog.

WiFi UDP submission and forwarding registration use nonblocking sends. A failed
MPDU submission is counted as lost, as before, and further queued attempts are
paced rather than stalled or retried in a busy loop. No extra packet queue is
inserted between TxMux and WifiUdp.

## Shutdown

`App::stop()` retains its signal-handler-safe behavior: set an atomic flag and
stop/wake the IO reactor. Once that run returns, App stops manager console
ingress, fences upstream input, and flushes partial FEC blocks. It then resumes
the same reactor for a drain bounded to 500 ms.

TxMux pacing timers continue sending during this drain. A shutdown-only 1 ms
timer checks whether the pending output is empty or the deadline has expired;
there is no synchronous tick/sleep loop. A second stop can interrupt the drain.
Finally App cancels shutdown/TX scheduling. This bounds the drain wait, not the
execution time of arbitrary transport callbacks.

## Reactor budget

The selected reactor now performs MPDU building and submission alongside its
other callbacks. Keep operations nonblocking and provision its stack accordingly.
On ESP32-P4, the removed TX task had an 8 KiB stack; the example's manager thread
has 16 KiB. A host binding TxMux to its own CV reactor must account for TX stack
usage as well as any FEC or other component work on that thread. No hardware
throughput or latency improvement is claimed by this change.
