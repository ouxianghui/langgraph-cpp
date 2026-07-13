# 发布检查清单

发布 `v0.1.0-alpha` 这类公开 tag 前，请使用本清单逐项确认。

## 1. 必须通过的门禁

- [ ] worktree 只包含本次 release commit 预期内的变更。
- [ ] 没有 `.DS_Store`、构建输出、本地数据库、日志或生成缓存被 tracked。
- [ ] submodule 已通过 `git submodule update --init --recursive` 初始化。
- [ ] fresh checkout 可以在 Linux GCC 上 configure、build、test。
- [ ] fresh checkout 可以在 Linux Clang 上 configure、build、test。
- [ ] Linux ASAN/UBSAN 通过。
- [ ] Linux TSAN 通过。
- [ ] `tsan_repeat` CTest label 的 Linux TSAN repeat stress 通过。
- [ ] libFuzzer harness 可以构建并完成 smoke run。
- [ ] Python LangGraph conformance 针对当前 upstream package 通过。
- [ ] `scripts/check-dependency-policy.sh` 通过。
- [ ] `scripts/run-examples.sh` 通过并生成最新 [reports/example-smoke-report.md](reports/example-smoke-report.md)。
- [ ] `scripts/generate-quality-report.sh --full` 已生成最新 [reports/latest-quality-report.md](reports/latest-quality-report.md)。
- [ ] docs snippet compile test 通过。
- [ ] coverage 入口 `scripts/coverage.sh` 可运行，或 release notes 明确说明本次未生成 coverage 报告的原因。
- [ ] clang-tidy release gate 通过。
- [ ] macOS smoke build 和 tests 通过。
- [ ] `README.md`、`docs/LIMITATIONS.md` 和 `CHANGELOG.md` 描述了当前 release 的真实状态。
- [ ] [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、[TEST_CATALOG.md](TEST_CATALOG.md)、[SECURITY_MODEL.md](SECURITY_MODEL.md)、[LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)、[PERFORMANCE_MODEL.md](PERFORMANCE_MODEL.md) 和 [LIMITATIONS.md](LIMITATIONS.md) 与当前实现一致。
- [ ] `LICENSE`、`CONTRIBUTING.md`、`SECURITY.md` 和 `THIRD_PARTY_NOTICES.md` 已存在。

## 2. 本地开发命令

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
ctest --test-dir build/unix-debug -L docs --output-on-failure
scripts/check-dependency-policy.sh
scripts/run-examples.sh
scripts/generate-quality-report.sh
git diff --check
```

可选本地门禁：

```sh
cmake --preset unix-debug-conformance
cmake --build --preset unix-debug-conformance
ctest --preset unix-debug-conformance -L langgraph_conformance

cmake --preset unix-fuzz
cmake --build --preset unix-fuzz
build/unix-fuzz/fuzz/langgraph_cpp_json_schema_fuzzer -runs=1000
```

## 3. CI 门禁

权威公开 release matrix 定义在 `.github/workflows/ci.yml`：

- Linux GCC Debug 和 Release。
- Linux Clang Debug。
- Linux Clang ASAN/UBSAN。
- Linux Clang TSAN，以及 `tsan_repeat` repeat stress。
- Linux Clang libFuzzer harness build 和 smoke runs。
- Python LangGraph conformance。
- 带项目噪声过滤的 clang-tidy。
- macOS AppleClang smoke build 和 tests。

## 4. 发布定位

对于 `v0.1.0-alpha`，项目应被描述为：

> 一个独立的 C++23 developer preview，用于构建 LangGraph-style 的 stateful agents edge runtime。

不要把项目描述为 production-stable，也不要描述为官方 LangGraph / LangChain C++ port。

## 5. 发布后检查

- [ ] tag、release notes 和文档中的版本号一致。
- [ ] [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md) 中的示例验证日期与实际命令一致。
- [ ] [LIMITATIONS.md](LIMITATIONS.md) 明确列出未支持能力和 deferred work。
- [ ] [API_CONTRACT.md](API_CONTRACT.md) 中的 API contract version 与测试一致。
- [ ] [QUALITY_MODEL.md](QUALITY_MODEL.md)、[CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md)、[ERROR_MODEL.md](ERROR_MODEL.md) 和 [PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md) 与当前实现一致。
- [ ] [API_CONTRACT.md](API_CONTRACT.md) 和 [MAINTAINER_GUIDE.md](MAINTAINER_GUIDE.md) 没有与本次 release 流程冲突。
- [ ] [OWNERSHIP.md](OWNERSHIP.md) 与 [../CODEOWNERS](../CODEOWNERS) 中的 owner/team 配置一致。
- [ ] [LIMITATIONS.md](LIMITATIONS.md) 的风险摘要中没有未处理的 release-blocking 风险。
- [ ] GitHub Actions release gate 全部绿色。

## 6. Release-blocking 风险

以下情况应阻止公开 release：

- 默认 build/test 在 Linux GCC 或 Linux Clang 失败；
- sanitizer gate 暴露未解释的内存或数据竞争问题；
- checkpoint/storage 破坏 persisted schema 且没有 migration；
- README/API docs 声称支持实际未实现能力；
- security/redaction tests 失败；
- [LIMITATIONS.md](LIMITATIONS.md) 未记录已知的高影响缺口。
