# Contributing to winject-l3

## Code style

- **C/C++:** [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with **Allman braces** (opening `{` on its own line). When they conflict, Allman wins for braces.
- **Formatting:** Root [`.clang-format`](../.clang-format). Format touched files with `clang-format -i` or rely on the check script below.
- **Types:** PascalCase class/struct/enum names match **PascalCase** file names (e.g. `ConsoleService` in `ConsoleService.h`, `UdpEndpoint` in `UdpEndpoint.cpp`, entry `Main.cpp`, CMake helpers `BfcFetch.cmake` / `IsalEc.cmake`). **`CMakeLists.txt`** stays as-is (CMake requirement).
- **802.11 MPDU:** Winject slot/domain stamping in [`src/manager/frames/Mpdu.*`](../src/manager/frames/) Manager code lives in `winject` (`App`, `Config`, `Mpdu`, radio/endpoint types, etc.); nested `winject::ieee_802_11` groups 802.11 layout; air LCP [`LCSequence`](../src/manager/frames/LCSequence.h) (logical channel sequence) with counters on [`UpstreamStats`](../src/manager/endpoint/UpstreamStats.h). `src/bfcext/` stays in `bfcext` on top of `bfc`. IEEE 802.11 layout in [`Frame.*`](../src/manager/frames/) (`Frame` in `winject::ieee_802_11`, from archived [winject](https://github.com/therooftopprinz/winject)). UDP transport: [`src/manager/radio/`](../src/manager/radio/).

## Refactors

Finish architectural changes in one coherent pass:

1. Define what the new flow is and what old code stops running.
2. Trace entry → core → exit; update **both** sides of each boundary (not only a downstream adapter).
3. Remove obsolete paths, fields, and signatures in the same change when possible.
4. Grep for leftovers; build `winject-manager` and run `winject-tests` before push.

Global refactor discipline for agents also lives in machine **User Rules** / `coding-standards.mdc`.

## Tests

- Host unit tests: GoogleTest target **`winject-tests`** under [`src/test/`](../src/test/).
- Add `your_module_test.cpp` and register it in [`src/test/CMakeLists.txt`](../src/test/CMakeLists.txt); link any required manager `.cpp` sources the same way existing tests do.
- Logging in manager code: `LOG_INF`, `LOG_ERR`, `LOG_WRN` from [`log.h`](../src/manager/utils/log.h).

## IDE (clangd)

After configuring manager and test build dirs, refresh the compilation database at the repo root (gitignored):

```bash
./scripts/update_compile_commands.sh
```

Override build dirs with `WINJECT_MANAGER_BUILD_DIR` / `WINJECT_TEST_BUILD_DIR` if needed. Reload the editor or restart clangd after CMake reconfigures.

## Checks before push

From the repo root:

```bash
./scripts/check_guidelines.sh
```

This runs `clang-format --dry-run --Werror` on tracked sources (excluding paths in [`.clang-format-ignore`](../.clang-format-ignore)) and builds/runs **CTest** for `winject-tests` in `build_test_ci` (override with `WINJECT_TEST_BUILD_DIR`).

CI: [`.github/workflows/guidelines.yml`](../.github/workflows/guidelines.yml) runs the same script on push/PR.

## Agent / Cursor rules

Project-specific rules live in [`.cursor/rules/`](../.cursor/rules/). Personal coding standards may also be configured as global User Rules in Cursor.
