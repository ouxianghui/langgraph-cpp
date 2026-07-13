# 依赖规则

| 字段 | 内容 |
| --- | --- |
| 状态 | 生效中 |
| 最后更新 | 2026-07-10 |
| 范围 | 模块依赖方向、可选依赖边界、公共头约束和静态检查入口 |
| 关联文档 | [ARCHITECTURE.md](ARCHITECTURE.md)、[API_CONTRACT.md](API_CONTRACT.md)、[SECURITY_MODEL.md](SECURITY_MODEL.md)、[QUALITY_MODEL.md](QUALITY_MODEL.md) |

依赖规则的目标是让架构边界可以被脚本检查，而不是只停留在架构图里。

## 核心规则

| 规则 | 原因 | 检查入口 |
| --- | --- | --- |
| `src/foundation` 不得 include `src/langgraph` 头。 | foundation 是可复用基础设施，不能反向依赖业务 runtime。 | [../scripts/check-dependency-policy.sh](../scripts/check-dependency-policy.sh) |
| `include/langgraph_cpp/langgraph.hpp` 不得 include 内部 `.hh`。 | public aggregate header 不能泄露内部实现头。 | [../scripts/check-dependency-policy.sh](../scripts/check-dependency-policy.sh) |
| public aggregate header 不得直接 include `third_party` 路径。 | 第三方依赖应通过目标 include path 或模块实现隔离。 | [../scripts/check-dependency-policy.sh](../scripts/check-dependency-policy.sh) |
| network、SQLite、crypto、gzip、spdlog、llama.cpp 必须有 CMake option。 | 默认 core 需要保持 edge-friendly 和可裁剪。 | [../scripts/check-dependency-policy.sh](../scripts/check-dependency-policy.sh) |
| llama.cpp 必须 behind `LANGGRAPH_CPP_WITH_LLAMA_CPP`。 | 本地模型 adapter 是可选集成，不属于默认 core。 | [../scripts/check-dependency-policy.sh](../scripts/check-dependency-policy.sh) |

## 运行检查

```sh
scripts/check-dependency-policy.sh
```

该脚本是保守的第一层规则。它不会替代代码审查，但能防止最关键的分层错误悄悄进入仓库。

## 扩展规则

新增规则前应满足：

- 规则可以稳定自动检查；
- 不会误伤现有合法实现；
- 失败输出能直接指向文件和行；
- 已在本文说明规则意图。

