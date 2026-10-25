# Reactor-driven TX scheduling

TxMux shares its owner's reactor. It creates no thread and performs no blocking
wait or sleep. `start(reactor)` binds wake, arm and cancel operations to either an
IO reactor or a `bfc::cv_reactor<>`; `stop()` cancels pending pacing work and
invalidates callbacks already extracted by the reactor. The reactor must outlive
stop. The owner serializes start/stop on that reactor thread, or while stopped.

App currently binds TxMux to its manager IO reactor. Queue upstream ingress can
run on the host CV reactor and notify the IO reactor. This removes the dedicated
`winject_tx` thread. Upstream RX queues move FEC/application delivery onto supplied
CV reactors. Radio data/management sockets still use the IO reactor. A queue
deployment can bind TxMux to a CV reactor when its radio transport supports it.

## Notifications and pacing

`TxMux::make_tx_queue()` returns a bounded FIFO with a push callback that
requests scheduler work automatically. The queue has no reactor binding or read
callback interface. The caller passes this queue to the upstream
constructor. Upstream pushes raw/encoded output immediately; TxMux accesses
queue heads directly and never calls upstream TX methods. Notifications coalesce
into one pending reactor wake. The queue releases its mutex before notifying.

Startup discovers prequeued data. App explicitly requests work after topology or
scheduling changes; queued radio-address changes wake TxMux too. No periodic
fallback polling is needed. Queue wake bindings become inert when TxMux is
destroyed, even if a producer retains a queue handle.

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

`App::stop()` sets an atomic flag and stops/wakes the IO reactor. Once that run
returns, App stops management ingress, unregisters upstream queue handles,
releases upstream instances, and stops TX scheduling. Teardown is lossy: partial
FEC blocks and queued packets are discarded without a flush/drain interval.
Upstream destructors unregister callbacks and cancel timers; in-flight callbacks
hold shared leases until their current turn finishes.

## Reactor budget

The selected reactor now performs MPDU building and submission alongside its
other callbacks. Keep operations nonblocking and provision its stack accordingly.
On ESP32-P4, the removed TX task had an 8 KiB stack; the example's manager thread
has 16 KiB. A host binding TxMux to its own CV reactor must account for TX stack
usage as well as any FEC or other component work on that thread. No hardware
throughput or latency improvement is claimed by this change.
