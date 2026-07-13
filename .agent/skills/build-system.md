# Skill: Build System

Use this when changing C++ build configuration such as CMake, Bazel, Meson, Make, package manager
files, generated code, link dependencies, or test targets.

Read project context first, as defined in `../README.md`.

## Configure and Regenerate Hygiene

- Prefer the existing build directory, preset, generator, and toolchain when the project already has
  one configured.
- Reconfigure or regenerate when build files, package inputs, toolchain files, presets, generated-code
  inputs, source discovery rules, or cache-dependent options change.
- Do not edit generated files, package-manager output, build caches, or IDE/project files unless the
  project explicitly treats them as source.
- Treat cache, preset, generator, toolchain, sanitizer, warning, and compile-command changes as
  build-environment changes; keep them intentional and easy to explain.

## Source Files

- Follow the source discovery model defined in `STACK.md`: explicit source lists, globbed sources, or
  per-directory build files.
- If sources are globbed, new files may still require reconfigure or regeneration.
- If sources are explicit, add new `.cpp` files to the owning target only once.
- New top-level modules usually require a build-system entry and documentation in external project
  context.
- Keep headers visible to IDE/project generation according to local convention.

## Test Targets

- Prefer adding a test file/case to an existing target when it tests the same contract.
- Create a new test executable only for a distinct contract or fixture boundary.
- Link tests to the smallest required project library and test framework target.
- Do not compile production `.cpp` files directly into multiple tests unless there is no library seam.
- Avoid custom test mains when the project test framework already provides one.

## Target Ownership and Usage Requirements

- Add sources, headers, generated files, and resources to the target that owns the behavior.
- Keep include directories, compile definitions, compile features, compile options, precompiled
  headers, warnings-as-errors, sanitizer settings, static analysis, and linker options scoped to the
  narrowest correct target.
- Avoid global build settings unless the project intentionally uses them for every target.
- When changing a public header or link interface, verify that usage requirements flow to at least
  one real consumer target.

## Link Scope

- Use `PRIVATE` for dependencies used only by a target's implementation.
- Use `PUBLIC` only when consumers need the dependency's headers, compile definitions, or link
  interface through this target.
- Use `INTERFACE` only for header-only or usage-requirement targets.
- Do not make a dependency `PUBLIC` to fix one test if the production API does not expose it.
- If a header includes a third-party type, the owning library may need a `PUBLIC` dependency. If only
  an implementation file uses it, keep it `PRIVATE`.

## Package Managers

- Do not upgrade, add, or replace dependencies unless the task explicitly requires it.
- Use the package manager and approved repositories/remotes defined in `STACK.md`.
- If dependency targets are missing, inspect the configured install/bootstrap command before changing
  package versions.
- Do not work around missing packages by vendoring source or adding machine-local include paths unless
  the project explicitly allows it.

## Generated Code

- Keep generated outputs in the build/generated location defined by `STACK.md`.
- Do not commit generated files unless the project explicitly says to.
- Add new IDL/schema/proto inputs and expected outputs to the owning generated-code target.
- Link generated types through the project-defined generated-code library/target.
- For generated contract changes, update consumers, tests, and external docs together.

## Runtime and Packaging Assets

- Add runtime configuration, data files, shared libraries, plugins, certificates, schemas, and other
  assets to the copy, install, package, or deployment path defined by the project.
- Keep runtime lookup paths, rpath/install-name behavior, working directories, and test fixture paths
  aligned with the way the binary or test is launched.
- When a build change adds an installed artifact, update packaging manifests, container/image inputs,
  smoke tests, and docs when the project has them.

## Diagnose Build Errors

- `header not found`: check include path ownership, header location, self-contained includes, and
  generated include directories.
- `undefined symbol`: check whether the `.cpp` is in the target, whether the right library is linked,
  and whether declaration and definition signatures match.
- `duplicate symbol`: check non-inline definitions in headers and duplicate compilation into multiple
  targets.
- `target not found`: check package install/bootstrap output, target name, and generated build files.
- `duplicate main`: a test target probably links a framework-provided main and also defines a custom
  main.

## Build Graph Verification

- After build-system, package, preset, or generated-input changes, configure or regenerate before
  building.
- Build generated-code targets before consumers when generated interfaces changed.
- Build the owning library or executable for changed production sources.
- Build the primary binary or at least one real consumer when public headers, exported include paths,
  compile definitions, or link interfaces changed.
- Build and run the new or changed test target when test build files changed.

## Before Finishing

- Run the smallest verification sequence from `STACK.md` that covers the changed build graph.
- State any configure, build, install/package, generated-code, or test target that should run but was
  skipped.
- Use `definition-of-done.md` before summarizing build-system work that is part of a larger change.
