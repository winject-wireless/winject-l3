# winject-l3

Host-side **winject-manager**: programs winject radios over their UDP console (m-plane), stamps 802.11 MPDUs, and bridges UDP applications onto the air through the radios' d-plane (inject UDP 9000, forward UDP 9210).

Two radios speak this protocol:

| Radio | Repo | Runs on | FCS (`radio_caps_info`) |
|---|---|---|---|
| ESP32 (WT32-ETH01) | [winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32) | Its own board, reached over Ethernet | `SIGNAL` |
| Realtek RTL8812AU | [winject-radio-realtek](https://github.com/winject-wireless/winject-radio-realtek) | A Linux host, one process per USB dongle, `rtl88xxau_wfb` driver | `ACTUAL` |

Both expose the same UDP console and d-plane ports, so the manager drives either one with the same config keys. See [Realtek radio](#realtek-radio) for the settings that differ.

## Layout

```
src/manager/            winject-manager binary (802.11 MPDU in frames/Mpdu.*, IEEE layout in frames/Frame.*)
src/test/               Host unit tests (GoogleTest)
configuration/          Manager configs: host_x86/, host_arm/ (video demo), winject-tests/<scenario>/ (bench pairs: esp32/, realtek/)
scripts/                Bench scripts (bw/lat tests, ensure_manager.sh, check_guidelines.sh)
tools/                  Python bench tools (radio_stats.py, bw_test.py, lat_test.py) and CMake toolchains
docs/mplane.md         Manager config and m-plane (client console)
docs/versioning.md     vX.Y.Z protocol rules and frozen `version` discovery
docs/radio.md           Radio m-plane: commands the manager forwards vs radio-local ones
docs/winject.md         802.11 MPDU layout, LC header, buses
docs/fec-spreading.md   FEC block spreading: burst loss, sizing, bench results
docs/radio-capa-support.md  radio_caps_info and FCS trailer modes (SIGNAL / ACTUAL)
docs/no-traffic.md      Bench report: zero-traffic investigation and loss analysis
docs/contributing.md    Code style, tests, and ./scripts/check_guidelines.sh
```

## Code style and checks

Google C++ with Allman braces; see [docs/contributing.md](docs/contributing.md). Before pushing:

```bash
./scripts/check_guidelines.sh
```

## Build

**Host requirement:** Linux kernel **5.11+** (the manager reactor uses `epoll_pwait2` for accurate timers).

Native build (x86_64 or aarch64):

```bash
cmake -S src/manager -B build_manager_host -DCMAKE_BUILD_TYPE=Release
cmake --build build_manager_host -j"$(nproc)"
./build_manager_host/winject-manager configuration/host_x86/config.cfg
```

Configuration keys and the manager console are described in [docs/mplane.md](docs/mplane.md).

Cross-build for Allwinner H3 / Orange Pi PC (32-bit armhf; needs the `arm-linux-gnueabihf` toolchain):

```bash
cmake -S src/manager -B build_manager_h3 \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_manager_h3 -j"$(nproc)"
```

Host tests:

```bash
cmake -S src/test -B build_test_ci -DCMAKE_BUILD_TYPE=Release
cmake --build build_test_ci -j"$(nproc)"
ctest --test-dir build_test_ci --output-on-failure
```

## Realtek radio

[winject-radio-realtek](https://github.com/winject-wireless/winject-radio-realtek) turns an RTL8812AU USB dongle into a winject radio. It puts the dongle in monitor mode, injects with radiotap (`NOACK`, per-frame rate/MCS), and forwards received MPDUs with their on-air FCS. Run one copy per dongle, either on the manager's host or on another Linux board.

Manager config for a Realtek radio on the same host:

```ini
winject.device       = 127.0.0.1
winject.console      = 2201        # the radio's net.console_port (not 22: sshd owns it on Linux)
winject.dplane_port  = 9000        # the radio's net.dplane_port (inject + registration + forward)
winject.radio_fcs    = actual      # or leave unset to auto-detect; never signal
# no winject.cca = 0
```

You can also set the radio at runtime with `radio_device` on the manager m-plane (`mplane`, `dplane`, `fcs`). A second radio on the same host needs distinct console and d-plane ports. The bench pair uses 2201/9000 for radio A and 2202/9003 for radio B. Avoid 9001 and 9002, because `tools/bw_test.py` listens there.

Differences from the ESP32:

| Topic | Realtek radio |
|---|---|
| CCA | `cca=false` is rejected (`NOK EINVAL`). Leave `winject.cca` unset or `1`, and do not pass `--no-cca` to the bench scripts |
| Modulations | `DSS_*_L`, `CCK_*_L`, `OFDM_6M`…`OFDM_54M`, `OFDM_MCS0-7_LGI/SGI`. Short preamble (`*_S`) is rejected because the driver ignores it |
| Channels | Whatever the dongle's regulatory domain allows, 2.4 and 5 GHz. The manager still accepts only 1–14 |
| TX power | `winject.power` is mapped through a measured index table (`txpower.csv` in the radio repo). The driver's power override is global: **all Realtek radios on one host must use the same `winject.power`** |
| Radio tests | `test_wifi_*` / `test_ether_*` reply `NOK ENODEV`. They measure ESP32 Wi-Fi/EMAC contention, which this radio does not have |
| Bad-FCS frames | Dropped by the driver before monitor mode, so on-air errors show up as lost frames rather than in `fcs_error_pkt` |

Measured throughput on the bench (two RTL8812AU, channel 13, 1400-byte payload, `tools/bw_test.py`), in Mbit/s:

| Modulation | A→B | B→A |
|---|---|---|
| `OFDM_24M` | 16.2 | 15.6 |
| `OFDM_54M` | 25.9 | 26.2 |
| `OFDM_MCS7_SGI` | 28.7 | 29.5 |
| `OFDM_MCS7_SGI`, 40 Mbit/s offered | 36.7 | |

The radios dropped no frames themselves on these runs, and the 4–10 % loss was all on air. The ESP32 bench configs cap the manager at `winject.max_rate_kbps = 20000`; the Realtek ones use 80000, so they can run faster modulations than `OFDM_24M`. `OFDM_24M` itself tops out near 18 Mbit/s of payload.

`configuration/winject-tests/realtek/` holds the Realtek bench scenario. `bw_a.cfg` / `bw_b.cfg` are the manager configs, with the ports and FCS setting above and `max_rate_kbps = 80000`. `radio_a.cfg` / `radio_b.cfg` are the radio configs, one per dongle, selected by interface name. Run it with `manager_iperf_bw_test.sh --scenario realtek` (see below). `manager_bw_test.sh` and `manager_lat_test.sh` only cover the ESP32 scenario for now.

## Bandwidth test (two radios)

`scripts/manager_iperf_bw_test.sh` starts one manager per radio (configs `configuration/winject-tests/<scenario>/bw_a.cfg` / `bw_b.cfg`), runs iperf2 UDP through them one direction at a time, and prints a radio drop-stage table per direction from the radio counters (see `radio_stats` in [docs/mplane.md](docs/mplane.md)).

Requirements:

- iperf2 (`apt install iperf`; iperf3 is not supported).
- Radio firmware with `radio_caps_info` and the `tx_info` / `rx_info` counters.

The script never talks to the radios directly. Each manager programs its radio at startup (PHY, CCA, domain filter), and the drop-stage counters are read through the managers' `radio_stats` / `get_metrics` commands.

Run it from an interactive terminal, in the winject-l3 checkout:

```bash
./scripts/manager_iperf_bw_test.sh \
  --a 192.168.253.11 --b 192.168.253.12 --host 192.168.253.106 \
  --channel 1 --modulation OFDM_24M --power 20 --no-cca \
  --dir both --time 15 --bitrate 20M
```

`--a` / `--b` default to the bench radios shown (192.168.253.11 / .12), and `--host` is auto-detected when omitted.

### Scenarios

`--scenario` picks the radios under test and the config directory `configuration/winject-tests/<scenario>/`:

| Scenario | Radios | What the script does first |
|---|---|---|
| `esp32` (default) | Two ESP32 boards on Ethernet (`--a` / `--b`) | Nothing: the radios are already running |
| `realtek` | Two RTL8812AU dongles on this host | Builds [winject-radio-realtek](https://github.com/winject-wireless/winject-radio-realtek) and starts one instance per dongle with `radio_a.cfg` / `radio_b.cfg` under `sudo` |
| `realtek-cross` | Radio A on this host, radio B on a peer (`--b`) | Starts radio A locally; starts radio B on `--radio-b-ssh` (default `ubuntu@<--b>`) with `net.bind = 0.0.0.0`. Both managers run on this host; `winject.device` for B is `--b`. |

```bash
./scripts/manager_iperf_bw_test.sh --scenario realtek --bitrate 16M
./scripts/manager_iperf_bw_test.sh --scenario realtek --modulation OFDM_MCS7_SGI --bitrate 30M
./scripts/manager_iperf_bw_test.sh --scenario realtek-cross \
  --a 127.0.0.1 --b 192.168.253.127 --radio-b-ssh ubuntu@192.168.253.127 \
  --channel 13 --modulation OFDM_24M --dir both --time 15 --bitrate 16M
```

Notes for `realtek` / `realtek-cross`:

- Each RTL8812AU must be bound to **`rtl88xxau_wfb`** (`modprobe 88XXau_wfb`), not stock **`rtl88XXau`**. Blacklist `88XXau` in `/etc/modprobe.d/` so replugs stay on the wfb driver; `scripts/realtek_radios.sh` rebinds any dongles still on `rtl88XXau` before starting the radios.
- The radio repo is expected at `../winject-radio-realtek`. To use another location, set `WINJECT_RADIO_REALTEK=<checkout>`; to skip the build, set `WINJECT_RADIO_REALTEK_BIN=<binary>`. The TX power table is `configuration/txpower.csv` in that checkout.
- On exit, including Ctrl-C, the script stops the radios and returns both dongles to NetworkManager in managed mode. The radio logs `radio_{a,b}.log` go into the log directory.
- `--a` / `--b` / `--host` and `--no-cca` are rejected: both radios are on `127.0.0.1`, and the radio has no CCA control.
- `--bitrate` above about 17M needs a faster `--modulation` than the default `OFDM_24M`.

Each run first does an incremental build of the manager from this checkout (`build_manager_arm/` on aarch64, `build_manager_x86/` otherwise; override with `WINJECT_MANAGER_BUILD`), so it always tests the current source. To test a specific binary without rebuilding, set `WINJECT_MANAGER=/path/to/winject-manager`.

Useful options:

| Option | Effect |
|---|---|
| `--bitrate 15M` | ESP32: stays under the radios' sustained TX rate (~1,450–1,550 frames/s). `20M` overflows the radio inject queue (`dropped_tx_queue`) |
| `--dir ab` / `--dir ba` | One direction only (`both` runs A→B then B→A) |
| `-t` / `--time SEC` | Duration per direction (default 10) |
| `-- -l <bytes>` | iperf datagram size, passed after `--` (default 1400; the script rejects more than 1445) |
| `--channel` / `--modulation` / `--power` / `--cca` / `--no-cca` | Radio PHY written into the managers' configs. Omitted: channel, modulation and power come from `bw_a.cfg` / `bw_b.cfg`, and CCA keeps the radio's setting |

Output:

- iperf results and, per direction, a radio drop-stage table with two residual checks. Both should print `OK`; a non-zero residual means a loss point is not counted.
- A log directory, printed at start as `logs=/tmp/winject-iperf-<pid>`, containing the iperf client and server logs, both manager logs, and the radio counter snapshots `radio_<dir>_before.json` / `radio_<dir>_after.json`. The script prints an **iperf server summary** line per direction (parsed from `iperf_srv_<dir>.log`). The iperf client's inline "Server Report" often matches, but it can disagree or show garbage (e.g. `4294966796/0`) when the UDP feedback path is confused — **trust the server log**. Bench traffic uses manager `UDP_SERVER` (ingress) and `UDP_CLIENT` (egress to `iperf -s`); iperf's own report datagrams may reach the client directly on loopback as well as via the manager relay.
