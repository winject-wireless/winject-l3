# Configuration layouts

Manager configs use the schema in [docs/mplane.md](../docs/mplane.md). This tree holds examples only; keys are not repeated here.

## Production / demo hosts

| Path | Role |
|---|---|
| `host_arm/config.cfg` | Video source (camera → manager on the ARM board) |
| `host_x86/config.cfg` | Video sink (manager → GStreamer display) |

Pair `host_arm` and `host_x86` on the same domain and bus pairs (`b2`/`a1`, `c3`/`d4`). See comments in each file and `gst.env` / `start_gst.sh`.

## Bench scenarios (`winject-tests/<scenario>/`)

Scripts take `--scenario` (default `esp32`) and read `bw_{a,b}.cfg`, `lat_udp_{a,b}.cfg`, or Realtek radio configs from the matching subdirectory.

| Scenario | Manager configs | Radio process configs | Scripts |
|---|---|---|---|
| `esp32/` | `bw_*.cfg`, `lat_udp_*.cfg` | (radios run on the boards; no files here) | `manager_bw_test.sh`, `manager_lat_test.sh`, `manager_iperf_bw_test.sh` |
| `realtek/` | `bw_*.cfg` | `radio_{a,b}.cfg` for [winject-radio-realtek](https://github.com/winject-wireless/winject-radio-realtek) | `manager_iperf_bw_test.sh --scenario realtek` (`realtek_radios.sh` starts radios) |
| `realtek-cross/` | `bw_*.cfg` (B-side hosts patched to `--b`) | Radio A: `realtek/radio_a.cfg` on this host; radio B on peer | `manager_iperf_bw_test.sh --scenario realtek-cross` |

### Manager config pattern

Every bench manager file sets:

- `radio.mplane` and `radio.dplane` as `host:port` (bench scripts rewrite the host to `--a` / `--b` where noted)
- `radio.channel`, `radio.modulation`, `radio.power`, `winject.domain`
- `upstream.size` and one or more `upstream-N.*` blocks with `tx_bus` / `rx_bus` and `rx` / `tx` endpoints

UDP latency pairs use two upstreams (`UDP_SERVER_FORWARDING` on A, `UDP_CLIENT_FORWARDING` on B) with buses `b2`/`a1` and `c3`/`d4`.

### Realtek radio configs vs manager configs

Files named `radio_*.cfg` in `winject-tests/realtek/` are consumed by **winject-radio-realtek**, not winject-manager. They select the USB interface (`radio.device`) and `net.*` listen addresses. Manager configs in the same directory use `radio.mplane` / `radio.dplane` and must agree on host and ports with the running radio process.
