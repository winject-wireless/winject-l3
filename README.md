# winject-l3

Host-side **winject-manager**: programs winject radios over the UDP console, stamps 802.11 MPDUs, and bridges UDP applications onto the air bus.

Radio firmware lives in [winject-wireless/winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32) (and Realtek NIC radio when available).

## Layout

```
src/manager/          winject-manager binary (802.11 MPDU in frames/Mpdu.*, IEEE layout in frames/Frame.*)
src/test/             Host unit tests (GoogleTest)
configuration/        Example manager configs and bench scripts
scripts/              Manager bench helpers (bw/lat tests, ensure_manager.sh)
docs/manager.md       Manager m-plane (client console)
docs/radio.md         Radio UDP console (PHY, upstream, network)
docs/winject.md       802.11 MPDU layout, LC header, radio console (manager view)
docs/contributing.md  Code style, tests, and ./scripts/check_guidelines.sh
```

## Code style and checks

Google C++ with Allman braces; see [docs/contributing.md](docs/contributing.md). Before pushing:

```bash
./scripts/check_guidelines.sh
```

## Build

**Host requirement:** Linux kernel **5.11+** (manager reactor uses `epoll_pwait2` for accurate timers).

```bash
cmake -S src/manager -B build_manager_host -DCMAKE_BUILD_TYPE=Release
cmake --build build_manager_host -j"$(nproc)"
./build_manager_host/winject-manager configuration/host_x86/config.cfg
```

Cross-build for Orange Pi H3 (armhf): use `tools/cmake/arm-linux-gnueabihf.cmake` with a dedicated build directory (see [docs/manager.md](docs/manager.md)).

Host tests:

```bash
cmake -S src/test -B build_test_ci -DCMAKE_BUILD_TYPE=Release
cmake --build build_test_ci -j"$(nproc)"
ctest --test-dir build_test_ci --output-on-failure
```

See [docs/manager.md](docs/manager.md).
