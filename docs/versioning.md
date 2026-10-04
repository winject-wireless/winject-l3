# Versioning

Every winject component uses `vX.Y.Z`:

| Part | Meaning | Bump when |
|---|---|---|
| `X.Y` | Protocol version (shared) | Any observable wire change |
| `Z` | Implementation patch (per repo) | Bug fixes, performance, docs |

Two components are compatible exactly when their `X.Y` are equal. `Z` is never compared.

**winject-l3:** `cmake/WinjectVersion.cmake` and generated `WinjectBuildVersion.h` from `src/manager/WinjectBuildVersion.h.in`. The manager answers `version|ver` with `OK version ver=<vX.Y.Z> proto=X.Y`.

**Version discovery (frozen):** transport UDP/IPv4; request `version` (alias `ver`) with no arguments; reply `OK version ver=... proto=...`; tagged `cmd:<u8>` / `OK:<u8>` / `NOK:<u8>`; unknown command `NOK ENOSYS`. See `docs/mplane.md`.

Release: wire change → bump `X.Y` in all three repos together, reset `Z` to 0; else bump `Z` here only; tag `vX.Y.Z`.
