# ADR-0002: checkpoint 写入 runtime 边界而不是任意副作用边界

| 字段 | 内容 |
| --- | --- |
| 状态 | Accepted |
| 日期 | 2026-07-10 |
| 关联 | [../PERSISTENCE_MODEL.md](../PERSISTENCE_MODEL.md)、[../ERROR_MODEL.md](../ERROR_MODEL.md) |

## 背景

runtime 需要支持 resume、history、replay、interrupt 和 crash recovery。用户 node/tool 可能执行外部副作用，例如调用模型、写设备、发请求或操作硬件。runtime 无法知道这些副作用是否幂等，也无法安全地在任意用户代码边界写 checkpoint。

## 决策

checkpoint 只在 runtime 定义的边界写入：

- initial；
- super-step completion；
- interrupt；
- failure；
- completion；
- `updateState()` fork；
- `Durability::Sync` task-level writes。

外部副作用的幂等性、去重和补偿由应用或 adapter 负责。

## 被拒绝的方案

| 方案 | 拒绝原因 |
| --- | --- |
| 在每个用户副作用前后自动 checkpoint | runtime 无法识别所有副作用，且会显著增加耦合。 |
| 完全不写中间 checkpoint | 无法支持 interrupt/resume、pending writes 和 crash recovery。 |
| 把外部副作用纳入 checkpoint 事务 | 对硬件、HTTP、模型 provider 不现实。 |

## 后果

- checkpoint 语义清晰，与 super-step 模型一致。
- crash recovery 可以围绕 checkpoint/pending writes 测试。
- 应用需要对真实外部副作用设计 idempotency key。
- 文档必须明确 runtime 恢复的是图状态，不是外部世界状态。

## 验证

- `checkpoint_resume`
- `sqlite_checkpoint_resume`
- `time_travel_history`
- `crash_recovery_test`
- `failure_injection_test`

