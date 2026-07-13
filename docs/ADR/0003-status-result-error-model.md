# ADR-0003: 使用 Status / Result 表达可恢复错误

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../ERROR_MODEL.md](../ERROR_MODEL.md)、[../API_CONTRACT.md](../API_CONTRACT.md) |

## 背景

`langgraph-cpp` 是 C++ runtime，但它面向 storage、checkpoint、schema、tool、model、HTTP、cancellation 等大量可恢复失败场景。如果公共 API 依赖异常或 bool，会让调用方难以区分 invalid input、unavailable backend、cancelled run、schema mismatch 和 unsupported feature。

## 决策

公共可恢复错误通过 `Status` 或 `Result<T>` 返回。异常只在边界处作为防护捕获，并转换成 `Status`。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 公共 API 抛异常 | 调用方难以统一处理 recoverable runtime failures。 |
| 返回 bool + out-param | 丢失错误码和上下文。 |
| 使用 `std::optional<T>` 表达失败 | 无法携带错误原因。 |

## 后果

- 调用方可以显式处理错误。
- 测试可以断言具体失败类别。
- public API 变更需要维护错误合同。
- 内部仍需要在异步、callback、thread entry 边界捕获异常。

## 验证

- `status_result_test`
- `json_schema_test`
- `checkpointer_test`
- `http_client_test`
- `tool` / provider 相关测试
