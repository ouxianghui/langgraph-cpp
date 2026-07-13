# 维护者指南

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | 新功能、API 变更、测试、文档、发布和代码审查流程 |
| 关联文档 | [AI_INDEX.md](AI_INDEX.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md)、[API_CONTRACT.md](API_CONTRACT.md)、[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) |

本文面向维护者和 AI coding agents，说明如何在不降低工程质量的前提下演进 `langgraph-cpp`。

## 1. 修改前先确认范围

| 问题 | 判断方式 |
| --- | --- |
| 是否改变 public API？ | 检查 `include/langgraph_cpp/langgraph.hpp` 可达类型和 public headers。 |
| 是否改变持久化语义？ | 检查 checkpoint、store、storage、serialization、content envelope。 |
| 是否改变 runtime 行为？ | 检查 graph execution、stream、interrupt、subgraph、resume/replay。 |
| 是否引入外部依赖？ | 必须 behind CMake option 或注入接口。 |
| 是否涉及 secret 或外部副作用？ | 必须检查 [SECURITY_MODEL.md](SECURITY_MODEL.md)。 |

## 2. 新功能落地顺序

1. 在 [PRD.md](PRD.md)、[ROADMAP.md](ROADMAP.md) 或 [internal/WBS.md](internal/WBS.md) 中确认需求归属。
2. 设计 public contract，必要时更新 [API_CONTRACT.md](API_CONTRACT.md)。
3. 先补行为契约测试或 conformance probe。
4. 实现最小清晰接口，不引入新旧接口双轨兼容。
5. 更新示例或 docs snippet。
6. 更新 [TRACEABILITY_MATRIX.md](TRACEABILITY_MATRIX.md)、[TEST_CATALOG.md](TEST_CATALOG.md) 和 [LIMITATIONS.md](LIMITATIONS.md)。
7. 跑对应验证命令，并在 PR/提交说明中列出。

## 3. 代码审查重点

| 区域 | 审查问题 |
| --- | --- |
| Graph runtime | super-step 边界是否清楚，merge 是否确定，checkpoint 是否在正确边界。 |
| Checkpoint/store | namespace、pending writes、latest pointer、delete/copy/prune 是否一致。 |
| Stream/event | envelope 字段是否稳定，错误和 interrupt 是否可见。 |
| Concurrency | 队列是否 bounded，shutdown 是否显式，borrowed view 是否跨 async 边界。 |
| Security | secret 是否 redacted，工具是否显式注册和校验。 |
| Foundation | 是否保持通用，不反向依赖 `src/langgraph`。 |
| API | 是否删除旧接口，是否更新 contract version。 |

## 4. 测试选择

| 修改类型 | 最小验证 |
| --- | --- |
| 文档-only | docs CTest label + Markdown link check + `git diff --check`。 |
| Public API | docs snippet compile + `versioning_test` + affected tests。 |
| Graph runtime | `graph_runtime_test`、`langgraph_compat_test`、相关 examples。 |
| Checkpoint/storage | `checkpointer_test`、`storage_test`、`crash_recovery_test`。 |
| Stream/interrupt/subgraph | `graph_runtime_test`、`langgraph_compat_test`、`stream_projection`、`human_interrupt`。 |
| HTTP/auth/provider | `http_client_test`、`provider_chat_model_test`、redaction tests。 |
| Concurrency | focused tests + TSAN gate。 |

## 5. 发布前维护动作

- 清理 tracked build artifacts 和本地临时文件。
- 确认 `README.md` 的状态描述没有超前。
- 确认 [LIMITATIONS.md](LIMITATIONS.md) 如实记录未支持能力。
- 确认 [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md) 的验证日期和命令真实。
- 确认 [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) 每项都有结果。
- 确认 [LIMITATIONS.md](LIMITATIONS.md) 的风险摘要中没有 release-blocking 风险未处理。

## 6. AI 工具使用注意事项

- 先读 [AI_INDEX.md](AI_INDEX.md)，再读对应模块 README。
- 不要把 roadmap 或 planned 文档当成已实现能力。
- 不要为了通过编译而吞掉 `Status` / `Result<T>` 错误。
- 不要编辑 `third_party` 或 build output，除非任务明确要求。
- 不要把旧 API 重新加回来做兼容，除非 API stability policy 明确批准。
