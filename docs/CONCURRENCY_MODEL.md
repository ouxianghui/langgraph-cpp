# 并发模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | graph runtime、runtime context、stream、checkpoint/store 端口和 foundation owner 机制 |
| 关联文档 | [ARCHITECTURE.md](ARCHITECTURE.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md) |

本文说明 `langgraph-cpp` 的线程和并发设计。重点是区分 LangGraph logical `thread_id` 与 C++ OS thread / owner thread。

## 1. 核心结论

`src/langgraph` 的图运行时没有绑定到单一 owner thread。它采用：

- 编译后的不可变 graph plan；
- 每次 invocation 独立的 run-local state；
- 可选 executor 驱动 parallel super-step；
- node 输出收集后串行 reducer merge；
- checkpoint/store/storage 各自负责自己的同步；
- live stream 使用 bounded channel 和明确 close 行为。

这使 runtime 同时支持 deterministic merge 和 fan-out 并发，而不把整个业务层锁死在一个串行线程上。

## 2. 概念区分

| 概念 | 含义 | 所在层 |
| --- | --- | --- |
| `threadId_` / `thread_id` | LangGraph-style logical thread，用于 checkpoint/resume/history 查询。 | graph/checkpoint protocol |
| C++ thread | OS 执行线程。 | foundation threading |
| `OwnerThread` / `OwnerExecutor` | “某组件 mutable state 只能在一个串行 executor 上修改”的 rule object。 | foundation |
| `IExecutor` | 图运行时可注入的 node task 执行器。 | foundation executor / graph runtime |
| super-step | 图执行批次；ready nodes 观察同一个 state snapshot。 | graph runtime |

## 3. Graph runtime 所有权

| 状态 | Owner | 并发规则 |
| --- | --- | --- |
| `CompiledStateGraph::graph_` | immutable shared graph plan | 可被多个 runs 共享读取。 |
| `RunResult::state_` | 当前 `runFrom()` 调用栈 | node 并发期间不被修改；merge 阶段串行修改。 |
| `activeTasks` / `nextTasks` | 当前 super-step | 创建后作为 task input；routing 后替换。 |
| `Runtime` | 单个 node attempt | 每个 task 创建自己的 runtime object。 |
| `StreamWriter` | 单个 node runtime 的 lightweight writer | 通过 publisher 进入统一 event path。 |
| `RunControl` | run-scoped cooperative drain flag | 用 mutex 保护小型共享状态。 |
| checkpointer/store | 注入的实现对象 | 各实现负责同步和持久化一致性。 |

## 4. Parallel super-step 规则

1. runtime 复制当前 `result.state_` 作为本轮 `inputState`。
2. ready tasks 从同一个 state snapshot 或自己的 `Send` branch-local state 读取。
3. node handler 并发执行，返回 `NodeOutput`。
4. runtime 收集全部 `NodeExecution`。
5. 对 writes 进行确定性排序。
6. 串行调用 reducer merge 更新 `result.state_`。
7. 根据 merged state routing，生成下一轮 tasks。
8. 在 runtime 定义的边界写入 checkpoint。

这个模型的关键是：并发发生在 node execution；共享 graph state 的修改发生在 merge/checkpoint 边界。

## 5. 为什么不是全局 owner thread

全局 owner thread 的好处是“所有 mutable state 串行”，但代价是：

- parallel fan-out 会被串行化；
- node handler 阻塞会阻塞整个图；
- provider/tool/hardware 的等待会污染 graph scheduler；
- 嵌入式应用无法复用自己的 executor/thread pool 策略；
- C++ OS thread 概念容易和 LangGraph logical `thread_id` 混淆。

因此 owner thread 保留在 foundation 层，供长期后台组件使用。graph runtime 则使用 run-local state、显式 executor 和串行 merge 保护行为确定性。

## 6. 什么时候应该用 OwnerThread

适合：

- HTTP client async queue；
- scheduler；
- GUI bridge；
- 长期后台 adapter；
- actor-like service；
- 需要固定线程访问第三方库的 hardware/provider binding。

不适合：

- `CompiledStateGraph` 的不可变执行计划；
- 单次 graph invocation 的临时状态；
- super-step 中可并发执行的 node tasks。

## 7. 开发规则

- node handler 必须把输入 `State` 当作不可变值。
- node 只能通过 `StateUpdate` 或 `Command` 修改图状态。
- node 不应持有对 `Runtime`、`State`、`StreamWriter` 的跨异步生命周期引用。
- 并发 node 共享外部资源时，应由应用、store、checkpointer、tool 或 adapter 自己定义同步。
- 新增 async callback 时必须定义：callback 线程、owner、shutdown、取消、队列容量和错误传播。

## 8. 验证入口

| 风险 | 验证 |
| --- | --- |
| parallel merge 不确定 | fan-out/fan-in reducer tests。 |
| stream 并发发布 | stream projection 和 live stream tests。 |
| resume/pending writes 竞态 | checkpoint 和 graph runtime tests。 |
| shutdown/cancellation race | TSAN repeat stress、cancellation tests。 |
| owner 组件错误线程访问 | owner executor tests、TSAN。 |
