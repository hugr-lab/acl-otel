# Spec 019: extension-ci-tools `main` again

- **Status**: implemented
- **Date**: 2026-10-08
- **Supersedes**: spec 017's pin (its `/GF-` stays)

This is the same change as duckdb-acl spec 110.

extension-ci-tools #432 (merged 2026-10-08 as 5326077) makes clang-cl **opt-in** for the windows job,
so windows_amd64 now builds with MSVC again. Neither problem from spec 017 applies under MSVC:
- #431's clang-cl vcpkg triplets have no resource compiler;
- #428's mixing of clang-cl and MSVC broke the alignment of pooled literals.

`distribution.yml` goes back to `@main` for both reusable workflows and for `ci_tools_version`.

`/GF-` stays in `extension_config.cmake`. It is passed only when the compiler is clang-cl, so it
keeps a later opt-in safe for as long as the OpenTelemetry stack comes from MSVC ports.

## Testing

The distribution build dispatched on this branch is green on every platform, windows_amd64 included.
