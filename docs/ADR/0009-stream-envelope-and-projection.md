# ADR-0009: 使用 LangGraph-style stream envelope 和 projection

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../API_CONTRACT.md](../API_CONTRACT.md)、[../LANGGRAPH_COMPATIBILITY.md](../LANGGRAPH_COMPATIBILITY.md)、[../QUALITY_MODEL.md](../QUALITY_MODEL.md) |

## 背景

streaming 既要服务本地 C++ 用户，也要尽量贴近 LangGraph-style 上层生态。runtime 需要输出 updates、values、tasks、checkpoints、interrupts、errors、messages chunk 和 custom events，同时还要支持 output_keys、namespace/subgraph projection 和不同消费方式。

## 决策

stream 输出采用显式 event envelope，而不是裸 JSON 或任意 callback payload。runtime 在 super-step、node、checkpoint、interrupt、error 和 message chunk 边界生成事件；用户通过 stream options 选择 modes、output_keys、subgraph namespace 和 projection。

golden fixtures 用于固定关键 envelope 形状，避免 stream 字段在重构中漂移。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 只返回最终 state | 无法支持 UI、debug、human-in-the-loop 和长任务进度。 |
| 裸 callback 参数随实现变化 | 缺少协议边界，上层生态难以适配。 |
| 强行声明完整 Python stream parity | 当前并未覆盖 Python runtime 所有 event 细节。 |

## 后果

- stream consumer 可以按 mode 处理事件。
- projection 逻辑是公共 contract 的一部分。
- 新增事件字段需要更新 API contract、测试和 fixtures。
- limitations 需要继续说明非完整 upstream parity。

## 验证

- `stream_projection`
- `langgraph_compat_test`
- `testdata/langgraph/stream_envelope_v3.json`
- docs snippet compile tests
