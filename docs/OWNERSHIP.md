# Ownership 模型

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-15 |
| 范围 | 代码所有权、review 边界、高风险目录和变更门禁 |
| 关联文档 | [ARCHITECTURE.md](ARCHITECTURE.md)、[API_CONTRACT.md](API_CONTRACT.md)、[DEPENDENCY_POLICY.md](DEPENDENCY_POLICY.md)、[MAINTAINER_GUIDE.md](MAINTAINER_GUIDE.md) |

本文定义维护者 review 责任。根目录 [../CODEOWNERS](../CODEOWNERS) 使用 `@langgraph-cpp/maintainers` 作为占位 team；公开发布前应替换为真实 GitHub team 或 maintainer handles。

## 1. 模块所有权

| 区域 | 责任 | 典型风险 | 必须同步检查 |
| --- | --- | --- | --- |
| `include/langgraph_cpp` | 公共聚合头和源码 API 入口。 | public surface 变化、第三方类型泄露。 | [API_CONTRACT.md](API_CONTRACT.md)、docs snippets。 |
| `src/langgraph/graph` | graph builder、compiled runtime、stream、subgraph、replay。 | super-step 语义、checkpoint 边界、LangGraph-style 行为漂移。 | graph/compat tests、[LANGGRAPH_COMPATIBILITY.md](LANGGRAPH_COMPATIBILITY.md)。 |
| `src/langgraph/checkpoint` | saver contract、pending writes、history、maintenance。 | resume 失败、schema 破坏、数据迁移。 | checkpoint/storage/crash tests、[PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md)。 |
| `src/langgraph/store` | 长期 memory store。 | checkpoint/store 概念混淆、filter/search 语义漂移。 | store tests、[PERSISTENCE_MODEL.md](PERSISTENCE_MODEL.md)。 |
| `src/langgraph/runtime` | node runtime context、interrupt、stream writer。 | handler 生命周期、borrowed reference、resume payload。 | runtime/interrupt tests、[CONCURRENCY_MODEL.md](CONCURRENCY_MODEL.md)。 |
| `src/langgraph/message/model/tool` | message、provider model、tool execution。 | provider delta、tool safety、secret logging。 | model/tool tests、[SECURITY_MODEL.md](SECURITY_MODEL.md)。 |
| `src/langgraph/edge` | hardware adapter interfaces。 | 真实硬件权限和外部副作用。 | edge tests、tool policy、[SECURITY_MODEL.md](SECURITY_MODEL.md)。 |
| `src/core` (`lgc::core`) | `RuntimeServices`、`RuntimeContainer`、lifecycle component factories。 | 把组装细节泄漏进 foundation，或反向依赖 langgraph。 | `scripts/check-dependency-policy.sh`、`runtime_services_test`、`lifecycle_test`、[ARCHITECTURE.md](ARCHITECTURE.md)。 |
| `src/foundation` | reusable infrastructure。 | 反向依赖、并发、I/O、logging、crypto。 | `scripts/check-dependency-policy.sh`、foundation tests。 |
| `context/` | agent 项目约定与 library skills。 | skill 与 docs/源码漂移、文件缺失、权威版本钉扎过期。 | `scripts/check-context-skills.sh`、[../context/REVIEW_CHECKLIST.md](../context/REVIEW_CHECKLIST.md)。 |
| `.github` / `scripts` | CI、quality gates、automation。 | release gate 漂移、脚本误报漏报。 | CI dry review、local script run。 |
| `docs` / `PROJECT_MANIFEST.json` | 用户和 AI 工具入口。 | 文档超前实现、链接漂移。 | Markdown link check、docs CTest、quality report。 |

## 2. 高风险变更

以下变更需要更严格 review：

- public headers 或 `include/langgraph_cpp/langgraph.hpp` 变化；
- checkpoint、storage、content envelope 或 persisted schema 变化；
- graph execution、stream、interrupt、subgraph、resume/replay 行为变化；
- new optional dependency、build option 或 CI gate；
- logs、metrics、HTTP auth、secret/redaction 变化；
- tool/hardware/provider external side effect 变化；
- `src/foundation` 引入对 `src/langgraph` 或 `src/core` 的依赖；
- `src/core` 引入对 `src/langgraph` 的依赖，或 `src/langgraph` 链接/include `lgc::core`；
- `context/` skills、权威钉扎或 inventory 与 docs/合同版本不一致。

## 3. Review 规则

- public API 变化必须更新 [API_CONTRACT.md](API_CONTRACT.md)。
- 行为契约变化必须更新测试或 golden fixture。
- 支持边界变化必须更新 [LIMITATIONS.md](LIMITATIONS.md)。
- 默认示例变化必须更新 [EXAMPLE_MATRIX.md](EXAMPLE_MATRIX.md) 并运行 [../scripts/run-examples.sh](../scripts/run-examples.sh)。
- 分层或 optional dependency 变化必须运行 [../scripts/check-dependency-policy.sh](../scripts/check-dependency-policy.sh)。
- `context/` 或合同版本钉扎变化必须运行 [../scripts/check-context-skills.sh](../scripts/check-context-skills.sh)。
- release readiness 相关变化应运行 [../scripts/generate-quality-report.sh](../scripts/generate-quality-report.sh)。
