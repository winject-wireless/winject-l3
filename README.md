# winject-l3

Host-side **winject-manager**: programs winject radios over their UDP console (m-plane), stamps 802.11 MPDUs, and bridges UDP applications onto the air through the radios' d-plane (inject UDP 9000, forward UDP 9210).

Radio firmware lives in [winject-wireless/winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32) (and a Realtek NIC radio when available).

## Layout

```
src/manager/            winject-manager binary (802.11 MPDU in frames/Mpdu.*, IEEE layout in frames/Frame.*)
src/test/               Host unit tests (GoogleTest)
configuration/          Manager configs: host_x86/, host_arm/ (video demo), winject-tests/ (bench pairs)
scripts/                Bench scripts (bw/lat tests, ensure_manager.sh, check_guidelines.sh)
tools/                  Python bench tools (radio_stats.py, bw_test.py, lat_test.py) and CMake toolchains
docs/mplane.md         Manager config and m-plane (client console)
docs/radio.md           Radio m-plane: commands the manager forwards vs radio-local ones
docs/winject.md         802.11 MPDU layout, LC header, buses
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

## Bandwidth test (two radios)

`scripts/manager_iperf_bw_test.sh` starts one manager per radio (configs `configuration/winject-tests/bw_a.cfg` / `bw_b.cfg`), runs iperf2 UDP through them one direction at a time, and prints a radio drop-stage table per direction from the radio counters (see `radio_stats` in [docs/mplane.md](docs/mplane.md)).

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

Each run first does an incremental build of the manager from this checkout (`build_manager_arm/` on aarch64, `build_manager_x86/` otherwise; override with `WINJECT_MANAGER_BUILD`), so it always tests the current source. To test a specific binary without rebuilding, set `WINJECT_MANAGER=/path/to/winject-manager`.

Useful options:

| Option | Effect |
|---|---|
| `--bitrate 15M` | Stays under the radios' sustained TX rate (~1,450–1,550 frames/s). `20M` overflows the radio inject queue (`dropped_tx_queue`) |
| `--dir ab` / `--dir ba` | One direction only (`both` runs A→B then B→A) |
| `-t` / `--time SEC` | Duration per direction (default 10) |
| `-- -l <bytes>` | iperf datagram size, passed after `--` (default 1400; the script rejects more than 1445) |
| `--channel` / `--modulation` / `--power` / `--cca` / `--no-cca` | Radio PHY written into the managers' configs. Omitted: channel, modulation and power come from `bw_a.cfg` / `bw_b.cfg`, and CCA keeps the radio's setting |

Output:

- iperf results and, per direction, a radio drop-stage table with two residual checks. Both should print `OK`; a non-zero residual means a loss point is not counted.
- A log directory, printed at start as `logs=/tmp/winject-iperf-<pid>`, containing the iperf client and server logs, both manager logs, and the radio counter snapshots `radio_<dir>_before.json` / `radio_<dir>_after.json`. The client's "Server Report" should match the `Lost/Total` summary in `iperf_srv_<dir>.log`. If they differ, trust the server log.
