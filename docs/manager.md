`help|?` - list commands
`ping` - send to maanger, if manager is alive it will respond `pong`
`get_channel_info|gci` - cumulative radio inject/forward totals plus per-upstream payload / seq-loss / FEC counters
`get_upstream_fec|guf <index>` - TX FEC on that upstream
set_upstream_fec <index> <NONE|RS_BLOCK_ERASURE> <k> <n>	suf	TX encode; UDP only
get_upstream_scheduler_budget <index>	gus	bytes per scheduler wakeup
set_upstream_scheduler_budget <index> <budget>	sus	budget > 0
set_modulation <modulation>	sd	radio TX rate (set_modulation on the radio UDP console)
get_modulation	gd	last applied / configured modulation
set_tx_pacing …	stp	runtime scheduler: max_rate_kbps, max_data_per_tick, tx_burst_size, tx_burst_interval_us (any subset, key=value tokens)
get_tx_pacing	gtp	current scheduler pacing knobs

# New console/control plane protocol
## Message Exchange
Control plane protocol is always client initiated.
```
client -> manager : request
client <- manager : response (optional)
```
## Messages
* `help|?` - List this document
* `ping` - Query manager availability, response `pong`
* `add_upstream id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> rx=<address:port> tx=<interface:port> fec=<NONE|BLOCK> k=<10> n=<16> fec_timeout=<50ms> quanta=<bytes>`
  	* OK Response `OK upstream id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> rx=<address:port> tx=<interface:port> fec=<NONE|BLOCK> k=<10> n=<16> fec_timeout=<50ms> quanta=<bytes>`
  	* NOK Response `NOK <message>`
  		* NOT_FOUND
      * INVALID_ARGUMENT
* `update_upstream_fec id=<u8> fec=<NONE|BLOCK> k=<10> n=<16> fec_timeout=<50ms>`
		* OK Response `OK upstream id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> rx=<address:port> tx=<interface:port> fec=<NONE|BLOCK> k=<10> n=<16> fec_timeout=<50ms> quanta=<bymtes>`
  	* NOK Response `NOK <message>`
  		* NOT_FOUND
      * INVALID_ARGUMENT
* `update_upstream_quanta id=<u8> quanta=<byte>`
  	* OK Response `OK upstream id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> rx=<address:port> tx=<interface:port> fec=<NONE|BLOCK> k=<10> n=<16> fec_timeout=<50ms> quanta=<bytes>`
* `remove_upstream id=<u8>`
  	* OK Response `OK`
  	* NOK Response `NOK <message>`
  		* NOT_FOUND
