# 测试目录说明

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | `tests/` 中的单元、组件、行为契约、故障注入、文档和 conformance 测试 |
| 关联文档 | [../docs/TEST_CATALOG.md](../docs/TEST_CATALOG.md)、[../docs/QUALITY_MODEL.md](../docs/QUALITY_MODEL.md)、[../docs/TRACEABILITY_MATRIX.md](../docs/TRACEABILITY_MATRIX.md) |

本目录的测试目标是证明 runtime 行为契约，而不是只证明 happy path 可以运行。

## 1. 默认原则

- 默认测试不依赖 Python、真实 cloud provider、真实硬件或外部 llama.cpp。
- recoverable error 必须通过 `Status` / `Result<T>` 被断言。
- graph、checkpoint、interrupt、stream、subgraph、store、crash recovery 需要覆盖失败路径。
- 文档中的 public API 片段应由 docs snippet compile test 保护。
- 可选 Python LangGraph conformance 单独用 preset 开启。

## 2. 常用命令

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --test-dir build/unix-debug --output-on-failure
```

聚焦文档片段：

```sh
ctest --test-dir build/unix-debug -L docs --output-on-failure
```

聚焦 runtime 行为契约：

```sh
ctest --test-dir build/unix-debug -R "graph|compat|checkpoint|store|crash|stream" --output-on-failure
```

可选 conformance：

```sh
cmake --preset unix-debug-conformance
cmake --build --preset unix-debug-conformance
ctest --preset unix-debug-conformance -L langgraph_conformance
```

## 3. 新增测试规则

- 测试名应表达场景和期望。
- 不要通过 sleep-based timing 断言制造不稳定测试。
- 并发测试应有明确同步点、超时和 shutdown。
- crash/corruption 测试应使用临时目录，不能写入源码树外的未知路径。
- provider tests 使用 fake HTTP transport，不访问真实网络。
- hardware tests 使用 mock adapter，不要求真实设备。

更完整的测试分类见 [../docs/TEST_CATALOG.md](../docs/TEST_CATALOG.md)。

