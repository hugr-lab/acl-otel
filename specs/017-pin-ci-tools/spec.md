# Spec 017: pin extension-ci-tools to f3fccb8, and /GF- for clang-cl

- **Status**: implemented
- **Date**: 2026-10-08
- **Found by**: the distribution build on main d689ef5 (run 37767298423), windows_amd64.
- **Temporary**: the pin goes once upstream fixes extension-ci-tools#431; `/GF-` goes once the ports are clang-cl builds.

This is the same change as duckdb-acl's specs 106 and 108.

- **The pin.**
  - extension-ci-tools #431 (2026-10-08) builds the vcpkg ports with clang-cl triplets. Its chainload
    toolchain leaves the resource compiler unset, so every port with a `.rc` file fails. protobuf
    failed first: `clang-cl: error: no such file or directory: '/c65001'`. The fix is suggested on #431.
  - `distribution.yml` pins both reusable workflows and `ci_tools_version` to
    f3fccb8e79f4c50b02447907207e2f3caf404946, which is #428 (the clang-cl switch) without #431.
- **`/GF-`.**
  - Under that commit the extension is compiled by clang-cl, while vcpkg's OpenTelemetry stack
    (protobuf, gRPC, abseil) comes from MSVC.
  - Both compilers pool string literals into same-named COMDATs, with different alignments. When the
    linker keeps clang-cl's 1-byte-aligned copy and MSVC code reads it with `movaps`, it faults
    before `main`: duckdb-acl spec 106, extension-ci-tools#430.
  - `extension_config.cmake` therefore passes `/GF-` to everything clang-cl compiles, duckdb
    included, since the file is read before duckdb adds `src/`.

## Testing

The distribution build dispatched on this branch is green on every platform, windows_amd64 included.
