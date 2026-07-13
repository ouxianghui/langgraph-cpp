# 最新质量报告

| 字段 | 内容 |
| --- | --- |
| 状态 | `passed` |
| 生成时间 | `2026-07-10T08:19:15Z` |
| Git branch | `main` |
| Git commit | `a8debfd` |
| Build directory | `build/unix-debug` |
| 生成命令 | `scripts/generate-quality-report.sh --full` |

本文由脚本生成，用于给维护者和 AI 工具提供最近一次本地质量门禁证据。它不是 release 证明；公开发布仍以 [../RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md) 和 CI 为准。

## 汇总

| Check | Status | Command |
| --- | --- | --- |
| Dependency policy | passed | `scripts/check-dependency-policy.sh` |
| Docs snippet compile | passed | `ctest --test-dir build/unix-debug -L docs --output-on-failure` |
| Git diff whitespace | passed | `git diff --check` |
| Default CTest suite | passed | `ctest --test-dir build/unix-debug --output-on-failure` |

## 详情

### Dependency policy

- Status: `passed`
- Command: `scripts/check-dependency-policy.sh`

```text
Dependency policy check passed.
```

### Docs snippet compile

- Status: `passed`
- Command: `ctest --test-dir build/unix-debug -L docs --output-on-failure`

```text
Internal ctest changing into directory: <repo>/build/unix-debug
Test project <repo>/build/unix-debug
    Start 12: langgraph_cpp_docs_snippet_compile_test
1/1 Test #12: langgraph_cpp_docs_snippet_compile_test ...   Passed    0.04 sec

100% tests passed, 0 tests failed out of 1

Label Time Summary:
contract    =   0.04 sec*proc (1 test)
docs        =   0.04 sec*proc (1 test)

Total Test time (real) =   0.04 sec
```

### Git diff whitespace

- Status: `passed`
- Command: `git diff --check`

```text
<no output>
```

### Default CTest suite

- Status: `passed`
- Command: `ctest --test-dir build/unix-debug --output-on-failure`

```text
Internal ctest changing into directory: <repo>/build/unix-debug
Test project <repo>/build/unix-debug
      Start  1: langgraph_cpp_status_result_test
 1/42 Test  #1: langgraph_cpp_status_result_test ..........   Passed    0.02 sec
      Start  2: langgraph_cpp_async_channel_test
 2/42 Test  #2: langgraph_cpp_async_channel_test ..........   Passed    0.10 sec
      Start  3: langgraph_cpp_id_generator_test
 3/42 Test  #3: langgraph_cpp_id_generator_test ...........   Passed    0.04 sec
      Start  4: langgraph_cpp_storage_test
 4/42 Test  #4: langgraph_cpp_storage_test ................   Passed    0.10 sec
      Start  5: langgraph_cpp_blob_store_test
 5/42 Test  #5: langgraph_cpp_blob_store_test .............   Passed    0.38 sec
      Start  6: langgraph_cpp_serialization_test
 6/42 Test  #6: langgraph_cpp_serialization_test ..........   Passed    0.04 sec
      Start  7: langgraph_cpp_checkpointer_test
 7/42 Test  #7: langgraph_cpp_checkpointer_test ...........   Passed    0.04 sec
      Start  8: langgraph_cpp_graph_runtime_test
 8/42 Test  #8: langgraph_cpp_graph_runtime_test ..........   Passed    0.13 sec
      Start  9: langgraph_cpp_langgraph_compat_test
 9/42 Test  #9: langgraph_cpp_langgraph_compat_test .......   Passed    0.07 sec
      Start 10: langgraph_cpp_fuzz_pressure_test
10/42 Test #10: langgraph_cpp_fuzz_pressure_test ..........   Passed    7.30 sec
      Start 11: langgraph_cpp_failure_injection_test
11/42 Test #11: langgraph_cpp_failure_injection_test ......   Passed    0.05 sec
      Start 12: langgraph_cpp_docs_snippet_compile_test
12/42 Test #12: langgraph_cpp_docs_snippet_compile_test ...   Passed    0.03 sec
      Start 13: langgraph_cpp_crash_recovery_test
13/42 Test #13: langgraph_cpp_crash_recovery_test .........   Passed    1.38 sec
      Start 14: langgraph_cpp_provider_chat_model_test
14/42 Test #14: langgraph_cpp_provider_chat_model_test ....   Passed    0.04 sec
      Start 15: langgraph_cpp_agent_loop_test
15/42 Test #15: langgraph_cpp_agent_loop_test .............   Passed    0.04 sec
      Start 16: langgraph_cpp_edge_adapter_test
16/42 Test #16: langgraph_cpp_edge_adapter_test ...........   Passed    0.03 sec
      Start 17: langgraph_cpp_versioning_test
17/42 Test #17: langgraph_cpp_versioning_test .............   Passed    0.03 sec
      Start 18: langgraph_cpp_content_envelope_test
18/42 Test #18: langgraph_cpp_content_envelope_test .......   Passed    0.02 sec
      Start 19: langgraph_cpp_time_test
19/42 Test #19: langgraph_cpp_time_test ...................   Passed    0.02 sec
      Start 20: langgraph_cpp_threading_timer_test
20/42 Test #20: langgraph_cpp_threading_timer_test ........   Passed    0.06 sec
      Start 21: langgraph_cpp_cancellation_test
21/42 Test #21: langgraph_cpp_cancellation_test ...........   Passed    0.02 sec
      Start 22: langgraph_cpp_executor_test
22/42 Test #22: langgraph_cpp_executor_test ...............   Passed    0.04 sec
      Start 23: langgraph_cpp_config_test
23/42 Test #23: langgraph_cpp_config_test .................   Passed    0.02 sec
      Start 24: langgraph_cpp_secrets_test
24/42 Test #24: langgraph_cpp_secrets_test ................   Passed    0.02 sec
      Start 25: langgraph_cpp_process_test
25/42 Test #25: langgraph_cpp_process_test ................   Passed    1.82 sec
      Start 26: langgraph_cpp_cache_test
26/42 Test #26: langgraph_cpp_cache_test ..................   Passed    1.23 sec
      Start 27: langgraph_cpp_json_schema_test
27/42 Test #27: langgraph_cpp_json_schema_test ............   Passed    0.05 sec
      Start 28: langgraph_cpp_event_test
28/42 Test #28: langgraph_cpp_event_test ..................   Passed    0.04 sec
      Start 29: langgraph_cpp_resource_limits_test
29/42 Test #29: langgraph_cpp_resource_limits_test ........   Passed    0.02 sec
      Start 30: langgraph_cpp_retry_policy_test
30/42 Test #30: langgraph_cpp_retry_policy_test ...........   Passed    0.02 sec
      Start 31: langgraph_cpp_rate_limit_test
31/42 Test #31: langgraph_cpp_rate_limit_test .............   Passed    0.03 sec
      Start 32: langgraph_cpp_scheduler_test
32/42 Test #32: langgraph_cpp_scheduler_test ..............   Passed    0.44 sec
      Start 33: langgraph_cpp_lifecycle_test
33/42 Test #33: langgraph_cpp_lifecycle_test ..............   Passed    0.17 sec
      Start 34: langgraph_cpp_runtime_services_test
34/42 Test #34: langgraph_cpp_runtime_services_test .......   Passed    0.06 sec
      Start 35: langgraph_cpp_filesystem_test
35/42 Test #35: langgraph_cpp_filesystem_test .............   Passed    0.04 sec
      Start 36: langgraph_cpp_compression_test
36/42 Test #36: langgraph_cpp_compression_test ............   Passed    0.02 sec
      Start 37: langgraph_cpp_observability_test
37/42 Test #37: langgraph_cpp_observability_test ..........   Passed    0.03 sec
      Start 38: langgraph_cpp_redaction_test
38/42 Test #38: langgraph_cpp_redaction_test ..............   Passed    0.03 sec
      Start 39: langgraph_cpp_crypto_test
39/42 Test #39: langgraph_cpp_crypto_test .................   Passed    0.02 sec
      Start 40: langgraph_cpp_encryption_test
40/42 Test #40: langgraph_cpp_encryption_test .............   Passed    0.02 sec
      Start 41: langgraph_cpp_http_client_test
41/42 Test #41: langgraph_cpp_http_client_test ............   Passed    0.13 sec
      Start 42: langgraph_cpp_logging_test
42/42 Test #42: langgraph_cpp_logging_test ................   Passed    0.07 sec

100% tests passed, 0 tests failed out of 42

Label Time Summary:
async                =   0.10 sec*proc (1 test)
cancellation         =   0.02 sec*proc (1 test)
contract             =   0.27 sec*proc (4 tests)
crash_recovery       =   1.38 sec*proc (1 test)
docs                 =   0.03 sec*proc (1 test)
executor             =   0.04 sec*proc (1 test)
failure_injection    =   0.05 sec*proc (1 test)
fuzz                 =   7.30 sec*proc (1 test)
graph                =   0.13 sec*proc (1 test)
langgraph_compat     =   0.07 sec*proc (1 test)
stress               =   8.68 sec*proc (2 tests)
threading            =   0.06 sec*proc (1 test)
tsan_repeat          =   9.07 sec*proc (8 tests)

Total Test time (real) =  14.24 sec
```
