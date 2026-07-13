# ADR-0004: 明确 LangGraph-style 兼容边界

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../API_CONTRACT.md](../API_CONTRACT.md)、[../LIMITATIONS.md](../LIMITATIONS.md)、[../QUALITY_MODEL.md](../QUALITY_MODEL.md) |

## 背景

项目目标是把 LangGraph-style 的执行思想带到 C++ edge runtime。上游 Python LangGraph 生态包含 RunnableConfig、saver、stream/event、interrupt、subgraph、serializer、LangSmith、LangChain provider 等大量语义。如果宣称完整同构，会误导用户；如果完全不对齐，又会降低生态可理解性。

## 决策

项目使用“LangGraph-style compatibility slice”：

- 公共命名尽量贴近官方生态；
- checkpoint、store、stream、interrupt、subgraph、config 的主语义尽量对齐；
- 通过 API contract 记录已支持 surface；
- 通过 conformance probe 验证关键行为；
- 通过 limitations 明确未支持完整 parity 的部分。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 声称完整 Python parity | 当前 serializer、trace backend、provider 生态和所有 event 细节并未完全同构。 |
| 完全 C++ 自创命名 | 降低 LangGraph 用户迁移和理解成本。 |
| 长期保留新旧 API 双轨 | 增加维护负担并让公共合同含混。 |

## 后果

- 用户可以用 LangGraph 概念理解 C++ runtime。
- 文档必须清楚区分“已支持”、“兼容切片”和“延后”。
- breaking rename 需要更新 API contract。
- 新兼容能力应补 conformance 或 golden tests。

## 验证

- `langgraph_compat_test`
- `tests/python_langgraph_conformance.py`
- [../API_CONTRACT.md](../API_CONTRACT.md)
- [../LIMITATIONS.md](../LIMITATIONS.md)
