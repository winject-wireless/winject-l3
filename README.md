# winject-l3

Host-side **winject-manager**: programs winject radios over the UDP console, stamps 802.11 MPDUs, and bridges UDP applications onto the air bus.

Radio firmware lives in [winject-wireless/winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32) (and Realtek NIC radio when available).

## Layout

```
src/manager/          winject-manager binary (802.11 MPDU in frames/Mpdu.*, IEEE layout in frames/Frame.*)
src/test/             Host unit tests (GoogleTest)
configuration/        Example manager configs and bench scripts
scripts/              Manager bench helpers (bw/lat tests, ensure_manager.sh)
docs/manager.md       Config reference and upstream modes
docs/winject.md       802.11 MPDU layout, LC seq, radio console (manager view)
docs/cd-protocol.md   Manager ↔ radio inject pacing
docs/contributing.md  Code style, tests, and ./scripts/check_guidelines.sh
```

## Code style and checks

Google C++ with Allman braces; see [docs/contributing.md](docs/contributing.md). Before pushing:

```bash
./scripts/check_guidelines.sh
```

## Build

```bash
cmake -S src/manager -B build_manager_arm -DCMAKE_BUILD_TYPE=Release
cmake --build build_manager_arm -j"$(nproc)"
./build_manager_arm/winject-manager src/manager/winject.conf.example
```

Cross-build for Orange Pi H3 (armhf): use `tools/cmake/arm-linux-gnueabihf.cmake` with `build_manager_h3`.

Host tests:

```bash
cmake -S src/test -B build_test_arm -DCMAKE_BUILD_TYPE=Release
cmake --build build_test_arm -j"$(nproc)"
ctest --test-dir build_test_arm --output-on-failure
```

See [docs/manager.md](docs/manager.md).
