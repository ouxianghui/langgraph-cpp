# Contributing to langgraph-cpp

Thanks for helping improve `langgraph-cpp`. This project is currently a
pre-1.0 developer preview, so API and persisted schema changes are still
possible, but they should be intentional and documented.

## Development Setup

Clone with submodules:

```sh
git submodule update --init --recursive
```

Build and test the default development configuration:

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

The default build must not require Python, real model providers, real hardware,
or an external llama.cpp setup.

## Expected Quality Gate

Before opening a pull request, run the narrowest relevant test first, then the
default suite:

```sh
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
git diff --check
```

For runtime, threading, storage, checkpoint, or public-header changes, also run
Linux compiler and sanitizer coverage when available:

```sh
cmake -S . -B build/linux-gcc-debug -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build/linux-gcc-debug
ctest --test-dir build/linux-gcc-debug --output-on-failure
```

Sanitizer and clang-tidy gates are defined in `.github/workflows/ci.yml`.

## Coding Guidelines

- Preserve C++23 and CMake-based builds.
- Keep core runtime code independent of real providers and hardware libraries.
- Use `Status` or `Result<T>` for recoverable errors.
- Do not silently swallow checkpoint, storage, cancellation, schema, or tool
  execution errors.
- Keep public API changes explicit and update `docs/API_CONTRACT.md` when
  the source contract changes. Use `docs/API_CONTRACT.md` for the
  breaking-change process and `docs/MAINTAINER_GUIDE.md` for maintainer
  workflow.
- Avoid unrelated formatting churn and broad refactors in feature or bug-fix
  pull requests.

## Security

The runtime does not ship privileged tools by default. Do not add built-in shell,
file, network, or hardware tools without an explicit safety design. See
`SECURITY.md` for reporting security issues and `docs/SECURITY_MODEL.md` for
the runtime permission, auth, redaction, and observability boundary.
