# Contributing to Prefix Fabric

Thank you for contributing to Prefix Fabric. This document covers how to build, test, and contribute, plus the conventions we follow and the release checklist.

## Set up your git identity

Commit under your own configured Git identity so your work is attributed to you. If you have not set one yet:

```sh
git config user.name "Your Name"
git config user.email "you@example.com"
```

## Prerequisites

- C++20 compiler (MSVC on Windows 11 x64 is the validated toolchain for releases).
- CMake 3.20 or newer.
- Git.
- No ML or GPU toolchain is required. There is no third-party runtime dependency.

## Build

From the repository root:

```sh
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

The build produces the `prefix_fabric` static library and, when `PREFIX_FABRIC_BUILD_CLI` is on (the default), the `prefix-fabric` CLI executable.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `PREFIX_FABRIC_BUILD_TESTS` | ON | Build the test executables |
| `PREFIX_FABRIC_BUILD_EXAMPLES` | ON | Add the `examples` subdirectory |
| `PREFIX_FABRIC_BUILD_BENCHMARKS` | ON | Add the `benchmarks` subdirectory |
| `PREFIX_FABRIC_BUILD_CLI` | ON | Build the `prefix-fabric` CLI |
| `PREFIX_FABRIC_WARNINGS_AS_ERRORS` | ON | Promote warnings to errors |

## Run the tests

```sh
ctest --test-dir build -C Release --output-on-failure
```

The test suite is registered as five CTest cases:

- `core` - `tests/test_core.cpp`
- `prop` - `tests/test_prop.cpp`
- `concurrency` - `tests/test_concurrency.cpp`
- `pers` - `tests/test_persist.cpp`
- `distributed` - `tests/test_distributed.cpp`

Run a single test directly after building, for example:

```sh
build\tests\Release\test_core.exe
```

The tests also exercise the property (`prop`), concurrency, persistence (`pers`), and distributed suites; the last two cover the atomic persistence path and the fencing/directory surfaces.

## Run examples and benchmarks

Examples and benchmarks are gated by `PREFIX_FABRIC_BUILD_EXAMPLES` and `PREFIX_FABRIC_BUILD_BENCHMARKS` (both default ON) and are added as subdirectories by `CMakeLists.txt`:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" \
  -DPREFIX_FABRIC_BUILD_EXAMPLES=ON \
  -DPREFIX_FABRIC_BUILD_BENCHMARKS=ON
cmake --build build --config Release
```

`examples/CMakeLists.txt` builds three runnable examples (`example_basic`, `example_persistence`, `example_distributed`) that are also registered as CTest tests. `benchmarks/CMakeLists.txt` builds the `prefix-fabric-benchmark` binary, which measures exact/longest lookup, insertion, segmentation, state-reference attachment, serialization, and snapshot at scaling points 1..100000 prefixes. The CLI (`cli/main.cpp`) implements the `info`, `add`, `match`/`longest-match`, `get`, `list`, `ancestors`/`descendants`/`branches`/`segments`, `attach-state`, `retire-state`, `invalidate`, `stats`, `explain`, `snapshot`, `save`/`restore`, `coordinator`, `node`, `remote-publish`/`remote-match`, and `benchmark` subcommands.

## Coding conventions

### Language and standard

- C++20. No exceptions to this: `CMAKE_CXX_STANDARD 20`, standard required, extensions off.
- Use the enclosing `prefix_fabric` namespace for public API and `pf_internal` for internal helpers.

### Error handling

- Expected, recoverable runtime errors are surfaced as `prefix_fabric::Result<T>` and `prefix_fabric::Error`. Do not throw or `abort()` across the public `Result` boundary for expected errors.
- `Result<void>` is used for operations that have no value. Callers check `is_ok()` / `is_err()` before accessing the value.
- The only place exceptions are permitted is at the explicit `value_or_throw()` escape hatch and for truly exceptional, unrecoverable conditions. Prefer `Result` over exceptions for anything that can happen in normal operation (capacity exceeded, stale authority, corruption, not found, and so on).
- Hard bounds are enforced as errors, never as silent unbounded allocation or coercion.

### Thread safety

- Every public entry point must be safe to call concurrently.
- Structural mutation is serialized per shard. Readers on a compat tree must run without a global lock.
- Callbacks must never run while an internal lock is held; subscribers must be able to re-enter without deadlocking.
- Lock ordering is a contract: never acquire a shard lock while holding the id-index lock. The id mutex is acquired only after the relevant shard locks, never before. See `docs/DESIGN.md` for the full ordering.

### Determinism

- Identical inputs under identical configuration must produce identical output. Never let behavior depend on unordered container iteration - use sorted traversal, stable ordering, and deterministic rounds.
- This applies to ids, segmentation, longest-prefix choice, serialization, eviction, candidate ranking, and snapshots. Determinism is what makes persistence, replay, and cross-node agreement correct.

### State ownership

- Prefix Fabric owns metadata and structure only. Never allocate, move, persist, reclaim, restore, or interpret the bytes referenced by a `StateReference`. The referenced payload belongs to the owning subsystem.

### Code style

- Keep headers self-contained and include what you use.
- Match the existing formatting (brace on its own line, 2-space indentation, `//` comments). MSVC builds use `/W4 /EHsc /utf-8`; non-MSVC builds use `-Wall -Wextra`. Keep the build warning-clean - `PREFIX_FABRIC_WARNINGS_AS_ERRORS` is ON by default.

### No generated garbage committed

- The tree must not contain generated binaries or build artifacts. `build/`, `x64/`, `Debug/`, `Release/`, `*.obj`, `*.exe`, `*.lib`, `*.dll`, `*.log`, and other outputs are covered by `.gitignore`. Do not commit generated files, CMake artifact directories, or editor/solution scratch files.
- Only source, headers, CMake files, test sources, docs, and the LICENSE belong in the tree.

## Pull request guidance

- Make focused, single-purpose changes.
- Add or update tests alongside any behavior change, and run the full `ctest` suite before pushing.
- For anything that touches serialization, persistence, id derivation, determinism, or lock ordering, add a specific test and explain the correctness reasoning in the PR description.

## Release checklist

Use this checklist for a `1.0.x` release.

1. **Version.** Confirm the version is consistent in `include/prefix_fabric/version.hpp` and the `project(...)` call in `CMakeLists.txt`, and that the package version file derives from the project version.
2. **Warning-clean build.** Configure `Release` with `PREFIX_FABRIC_WARNINGS_AS_ERRORS=ON` (the default) and build with no warnings.
3. **Full test pass.** Run `ctest --test-dir build -C Release --output-on-failure` and confirm all five cases (core, prop, concurrency, pers, distributed) pass.
4. **Examples and benchmarks.** Build with examples and benchmarks enabled. If new targets were added, run them and confirm no regression.
5. **Persistence round-trip.** Exercise `SaveToFile` / `LoadFromFile` and the corruption- and truncation-rejection paths.
6. **Install and consume.** Run `cmake --install build --config Release --prefix <install-dir>`, then build a small downstream consumer that uses `find_package(prefix_fabric CONFIG REQUIRED)` and links `prefix_fabric::prefix_fabric`.
7. **No generated garbage.** Confirm `git status` is clean of build artifacts and that `.gitignore` still covers everything. Remove any stray generated files.
8. **Docs and changelog.** Review `README.md`, `docs/DESIGN.md`, and `CONTRIBUTING.md` for accuracy against the released behavior. Note any API additions.
9. **Commit.** Commit under your own configured git identity with a descriptive message.
10. **Tag and sign.** Tag the release (for example `v1.0.0`) and, if your workflow uses them, sign the tag.
11. **Verify the release artifacts.** Confirm the install layout exposes `include/prefix_fabric`, the library, and `prefix_fabricConfig.cmake` under the CMake package directory.

> **Platform note (this release):** Linux is supported through portable seams, but releases are validated on Windows 11 x64 only. Do not claim Linux validation unless you have actually built and tested on Linux in the release branch.

## License

By contributing you agree that your contributions are licensed under Apache-2.0, Copyright 2026 Summon Software Labs.
