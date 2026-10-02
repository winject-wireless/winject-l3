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
docs/more-metrics.md    Radio drop counters and how the bench scripts use them
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

`scripts/manager_iperf_bw_test.sh` starts one manager per radio (configs `configuration/winject-tests/bw_a.cfg` / `bw_b.cfg`), runs iperf2 UDP through them one direction at a time, and prints a radio drop-stage table per direction from the radio counters (see [docs/more-metrics.md](docs/more-metrics.md)).

Requirements:

- iperf2 (`apt install iperf`; iperf3 is not supported).
- A winject-radio-esp32 checkout, by default next to this repo.
- Radio firmware with `radio_caps_info` and the `tx_info` / `rx_info` counters.

Run it from an interactive terminal, in the winject-l3 checkout:

```bash
RADIO_REPO=../winject-radio-esp32

# 1. Build the current manager
cmake --build build_manager_host -j"$(nproc)"

# 2. Use that build: on aarch64 the script otherwise runs build_manager_arm/,
#    and only rebuilds it when the binary is missing. The script imports
#    mpdu.py / bw_test.py from the radio repo.
export WINJECT_MANAGER=$PWD/build_manager_host/winject-manager
export PYTHONPATH=$(realpath "$RADIO_REPO/tools")

# 3. Prepare the radios with the radio repo's script (domain filter, PHY, CCA)
python3 "$RADIO_REPO/scripts/prepare_radios_for_manager.py" \
  --a 192.168.253.11 --b 192.168.253.12 \
  --channel 1 --modulation OFDM_24M --power 20 --no-cca

# 4. Run the test
./scripts/manager_iperf_bw_test.sh \
  --a 192.168.253.11 --b 192.168.253.12 --host 192.168.253.106 \
  --dir both --time 15 --bitrate 20M --no-cca
```

`--a` / `--b` default to the bench radios shown (192.168.253.11 / .12), and `--host` is auto-detected when omitted.

Useful options:

| Option | Effect |
|---|---|
| `--bitrate 15M` | Stays under the radios' sustained TX rate (~1,450–1,550 frames/s). `20M` overflows the radio inject queue (`dropped_tx_queue`) |
| `--dir ab` / `--dir ba` | One direction only (`both` runs A→B then B→A) |
| `-t` / `--time SEC` | Duration per direction (default 10) |
| `-- -l <bytes>` | iperf datagram size, passed after `--` (default 1400; the script rejects more than 1445) |

Output:

- iperf results and, per direction, a radio drop-stage table with two residual checks. Both should print `OK`; a non-zero residual means a loss point is not counted.
- A log directory, printed at start as `logs=/tmp/winject-iperf-<pid>`, containing the iperf client and server logs, both manager logs, and the radio counter snapshots `radio_<dir>_before.json` / `radio_<dir>_after.json`.

Known issues:

- **B→A result:** read it from `iperf_srv_ba.log` in the log directory. The "Server Report" the client prints for B→A can be garbage.
- **Prepare noise:** the script still calls the legacy `scripts/prepare_radios_for_manager.py`, which prints `NOK ENOSYS` lines and `forward ... B=9220`. They are harmless once step 3 has run.
- **`pkill` at startup:** the script runs `pkill -f "winject-manager.*winject"`, which also kills any parent shell whose command line matches. Don't wrap the exports and the script call in one `bash -c "..."` line. After an interrupted run, clean up with `killall winject-manager iperf`.
