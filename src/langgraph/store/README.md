# src/langgraph/store

本目录实现 LangGraph-style long-term store。Store 保存长期应用记忆；Checkpoint 保存执行历史，两者不能混用。

## 1. 核心类型

| 类型 | 职责 |
| --- | --- |
| `BaseStore` | store contract，核心虚接口是 `batch()`。 |
| `InMemoryStore` | memory-backed store，用于测试和 demo。 |
| `StorageStore` | 基于 `IStorage` 的 store。 |
| `StoreItem` | namespace/key/value/metadata 记录。 |
| `StoreSearchOptions` | namespace prefix、query、filter、limit、offset。 |
| `StoreOp` | get/search/put/listNamespaces batch operation variant。 |

## 2. Contract

- `batch()` 是核心虚接口。
- `abatch()` 是 async facade。
- 便捷方法包括 `put()`、`get()`、`search()`、`deleteItem()` 和 `listNamespaces()`。
- `deleteItem()` 是 C++ 对官方 `delete` 方法名的关键字避让。
- 未配置 semantic/vector backend 时，带 `query_` 的 search 返回 `StatusCode::Unimplemented`。

## 3. 设计规则

- Store 不拥有 graph execution lifecycle。
- Store 不记录 checkpoint step 或 next tasks。
- Store backend 自己负责同步、持久化和 filter/search 能力。
- Store maintenance/admin 能力不应混入 `BaseStore` 当前 public contract。

## 4. 关联文档

- [../../../docs/PERSISTENCE_MODEL.md](../../../docs/PERSISTENCE_MODEL.md)
- [../../../docs/API_CONTRACT.md](../../../docs/API_CONTRACT.md)
- [../../../docs/LIMITATIONS.md](../../../docs/LIMITATIONS.md)

