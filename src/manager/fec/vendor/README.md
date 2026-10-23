# Vendored vstreamer FEC

Source: vstreamer commit `5cd91b7e8dfb5190ca08852b6e066147be780921`.
Original files: `src/core/rs_block_erasure*` and `src/core/rs_encode_scalar*`.
The corresponding receive/deadline tests are in `src/test/VendoredFecTest.cpp`.

This is a local vendor copy, pending a separate shared-library extraction task.
There is no build-time dependency on the vstreamer checkout.

Local changes:

- Namespace `winject::fec_vendor`, local include paths and `WINJECT_` backend
  macros/symbols. Host NEON uses the existing winject ISA-L wrapper.
- Two read-only adapter queries: pending original TX datagrams/bytes and whether
  receive blocks need periodic polling. Encoded output is returned to the caller.
- Reject virtual pad indices in received headers, preserving winject validation.
- `RsBlockErasure.h` supplies winject's 1445-byte shard budget and k<n<=31 config
  profile. The vendored codec's n=k behavior remains covered by its own tests.
- ESP32-P4 PIE selection uses `CONFIG_WINJECT_RS_PIE` in the IDF component Kconfig.

Source SHA-256 values before adapting paths/names:

```
923285317adfd36b918cfb0d8699283d47d054a3435810b794af91ec8bbf16aa  src/core/rs_block_erasure.hpp
5ccd63ac5384d9b6f5d020821abeadfe57832c2fd2162e7be3cf7301e6f9bae1  src/core/rs_block_erasure.cpp
7429c1a32b44b97ff40157bf8fc6a5431f243033f529f9dbc2eb4cb6fe74129f  src/core/rs_block_erasure_rx.cpp
3489f05f5ecf13650adf19f537f8b7506e56ae00307de176b5a804063bc6fb4b  src/core/rs_encode_scalar.hpp
6f59453b07651956f1a0acba55172f3c41289da73a8e3ab067ed5798efc924fc  src/core/rs_encode_scalar.cpp
```
