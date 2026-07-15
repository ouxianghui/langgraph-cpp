# ADR-0011: 将公共 C++ namespace 从 `lc` 重命名为 `lgc`

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-15 |
| 关联 | [../API_CONTRACT.md](../API_CONTRACT.md)、[../ARCHITECTURE.md](../ARCHITECTURE.md) |

## 背景

公共源码 API 原先使用 `namespace lc`，并与 CMake 导出命名空间 `lc::`（`lc::foundation`、`lc::core`、`lc::langgraph`）对齐。`lc` 过短，可读性弱，也更容易与其它库的缩写冲突。项目仍处于 pre-1.0，允许通过显式 API 合同提升完成破坏性重命名。

## 决策

- 公共 C++ namespace 统一为 `lgc`（**L**an**G**raph-**C**pp）。
- CMake / install 导出命名空间同步为 `lgc::`（`lgc::foundation`、`lgc::core`、`lgc::langgraph`）。
- 不保留 `namespace lc` 或 `lc::` 长期兼容别名；调用方随 API 合同版本升级后从源码重建。
- API 合同版本提升至 `26`；持久化 schema 合同不变。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 保持 `lc` | 命名不直观，与偏好的公共可读性目标不符。 |
| 使用 `langgraph` / `langgraph_cpp` | 过长，且全名易被误认为官方 LangGraph C++ port。 |
| `namespace lc = lgc` 双轨兼容 | 违反“删除旧公共名、不长期双轨”的 API 合同惯例。 |

## 后果

- 所有 `#include <langgraph_cpp/langgraph.hpp>` 调用方需将 `lc::` 改为 `lgc::`。
- 链接 `lc::foundation` / `lc::langgraph` / `lc::core` 的下游 CMake 需改为 `lgc::*`。
- docs、examples、tests、context skills 与 dependency-policy 脚本同步更新。

## 验证

- `rg` 在源码与文档中不再出现公共 `namespace lc` / `lc::` 别名（除历史 ADR/变更说明中的叙述）。
- `scripts/check-dependency-policy.sh` 与 `scripts/check-context-skills.sh` 通过。
- 默认 `unix-debug` 配置构建并通过 focused CTest。
