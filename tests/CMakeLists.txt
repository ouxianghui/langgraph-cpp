add_executable(langgraph_cpp_status_result_test
    status_result_test.cpp)

target_link_libraries(langgraph_cpp_status_result_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_status_result_test
    COMMAND langgraph_cpp_status_result_test)

add_executable(langgraph_cpp_async_channel_test
    async_channel_test.cpp)

target_link_libraries(langgraph_cpp_async_channel_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_async_channel_test
    COMMAND langgraph_cpp_async_channel_test)
set_tests_properties(langgraph_cpp_async_channel_test
    PROPERTIES
        LABELS "async;tsan_repeat")

add_executable(langgraph_cpp_id_generator_test
    id_generator_test.cpp)

target_link_libraries(langgraph_cpp_id_generator_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_id_generator_test
    COMMAND langgraph_cpp_id_generator_test)

add_executable(langgraph_cpp_storage_test
    storage_test.cpp)

target_link_libraries(langgraph_cpp_storage_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_storage_test
    COMMAND langgraph_cpp_storage_test)

add_executable(langgraph_cpp_blob_store_test
    blob_store_test.cpp)

target_link_libraries(langgraph_cpp_blob_store_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_blob_store_test
    COMMAND langgraph_cpp_blob_store_test)

add_executable(langgraph_cpp_serialization_test
    serialization_test.cpp)

target_link_libraries(langgraph_cpp_serialization_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_serialization_test
    COMMAND langgraph_cpp_serialization_test)

add_executable(langgraph_cpp_checkpointer_test
    checkpointer_test.cpp)

target_link_libraries(langgraph_cpp_checkpointer_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_checkpointer_test
    COMMAND langgraph_cpp_checkpointer_test)

add_executable(langgraph_cpp_graph_runtime_test
    graph_runtime_test.cpp)

target_link_libraries(langgraph_cpp_graph_runtime_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_graph_runtime_test
    COMMAND langgraph_cpp_graph_runtime_test)
set_tests_properties(langgraph_cpp_graph_runtime_test
    PROPERTIES
        LABELS "graph;contract;tsan_repeat")

add_executable(langgraph_cpp_langgraph_compat_test
    langgraph_compat_test.cpp)

target_link_libraries(langgraph_cpp_langgraph_compat_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_langgraph_compat_test
    COMMAND langgraph_cpp_langgraph_compat_test)
set_tests_properties(langgraph_cpp_langgraph_compat_test
    PROPERTIES
        LABELS "langgraph_compat;contract")

add_executable(langgraph_cpp_fuzz_pressure_test
    fuzz_pressure_test.cpp)

target_link_libraries(langgraph_cpp_fuzz_pressure_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_fuzz_pressure_test
    COMMAND langgraph_cpp_fuzz_pressure_test)
set_tests_properties(langgraph_cpp_fuzz_pressure_test
    PROPERTIES
        LABELS "stress;fuzz;tsan_repeat")

add_executable(langgraph_cpp_failure_injection_test
    failure_injection_test.cpp)

target_link_libraries(langgraph_cpp_failure_injection_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_failure_injection_test
    COMMAND langgraph_cpp_failure_injection_test)
set_tests_properties(langgraph_cpp_failure_injection_test
    PROPERTIES
        LABELS "failure_injection;contract;tsan_repeat")

add_executable(langgraph_cpp_docs_snippet_compile_test
    docs_snippet_compile_test.cpp)

target_link_libraries(langgraph_cpp_docs_snippet_compile_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_docs_snippet_compile_test
    COMMAND langgraph_cpp_docs_snippet_compile_test)
set_tests_properties(langgraph_cpp_docs_snippet_compile_test
    PROPERTIES
        LABELS "docs;contract")

if(LANGGRAPH_CPP_ENABLE_PYTHON_LANGGRAPH_CONFORMANCE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_executable(langgraph_cpp_langgraph_conformance_probe
        langgraph_conformance_probe.cpp)

    target_link_libraries(langgraph_cpp_langgraph_conformance_probe
        PRIVATE
            langgraph)

    add_test(NAME langgraph_cpp_python_langgraph_conformance
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/python_langgraph_conformance.py"
            --cpp-probe $<TARGET_FILE:langgraph_cpp_langgraph_conformance_probe>)
    set_tests_properties(langgraph_cpp_python_langgraph_conformance
        PROPERTIES
            LABELS "langgraph_conformance;python")
endif()

add_executable(langgraph_cpp_crash_recovery_test
    crash_recovery_test.cpp)

target_link_libraries(langgraph_cpp_crash_recovery_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_crash_recovery_test
    COMMAND langgraph_cpp_crash_recovery_test)
set_tests_properties(langgraph_cpp_crash_recovery_test
    PROPERTIES
        LABELS "crash_recovery;stress;tsan_repeat")

add_executable(langgraph_cpp_provider_chat_model_test
    provider_chat_model_test.cpp)

target_link_libraries(langgraph_cpp_provider_chat_model_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_provider_chat_model_test
    COMMAND langgraph_cpp_provider_chat_model_test)

add_executable(langgraph_cpp_agent_loop_test
    agent_loop_test.cpp)

target_link_libraries(langgraph_cpp_agent_loop_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_agent_loop_test
    COMMAND langgraph_cpp_agent_loop_test)

add_executable(langgraph_cpp_edge_adapter_test
    edge_adapter_test.cpp)

target_link_libraries(langgraph_cpp_edge_adapter_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_edge_adapter_test
    COMMAND langgraph_cpp_edge_adapter_test)

add_executable(langgraph_cpp_versioning_test
    versioning_test.cpp)

target_link_libraries(langgraph_cpp_versioning_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_versioning_test
    COMMAND langgraph_cpp_versioning_test)

add_executable(langgraph_cpp_content_envelope_test
    content_envelope_test.cpp)

target_link_libraries(langgraph_cpp_content_envelope_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_content_envelope_test
    COMMAND langgraph_cpp_content_envelope_test)

add_executable(langgraph_cpp_time_test
    time_test.cpp)

target_link_libraries(langgraph_cpp_time_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_time_test
    COMMAND langgraph_cpp_time_test)

add_executable(langgraph_cpp_threading_timer_test
    threading_timer_test.cpp)

target_link_libraries(langgraph_cpp_threading_timer_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_threading_timer_test
    COMMAND langgraph_cpp_threading_timer_test)
set_tests_properties(langgraph_cpp_threading_timer_test
    PROPERTIES
        LABELS "threading;tsan_repeat")

add_executable(langgraph_cpp_cancellation_test
    cancellation_test.cpp)

target_link_libraries(langgraph_cpp_cancellation_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_cancellation_test
    COMMAND langgraph_cpp_cancellation_test)
set_tests_properties(langgraph_cpp_cancellation_test
    PROPERTIES
        LABELS "cancellation;tsan_repeat")

add_executable(langgraph_cpp_executor_test
    executor_test.cpp)

target_link_libraries(langgraph_cpp_executor_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_executor_test
    COMMAND langgraph_cpp_executor_test)
set_tests_properties(langgraph_cpp_executor_test
    PROPERTIES
        LABELS "executor;tsan_repeat")

add_executable(langgraph_cpp_config_test
    config_test.cpp)

target_link_libraries(langgraph_cpp_config_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_config_test
    COMMAND langgraph_cpp_config_test)

add_executable(langgraph_cpp_secrets_test
    secrets_test.cpp)

target_link_libraries(langgraph_cpp_secrets_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_secrets_test
    COMMAND langgraph_cpp_secrets_test)

add_executable(langgraph_cpp_process_test
    process_test.cpp)

target_link_libraries(langgraph_cpp_process_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_process_test
    COMMAND langgraph_cpp_process_test)

add_executable(langgraph_cpp_cache_test
    cache_test.cpp)

target_link_libraries(langgraph_cpp_cache_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_cache_test
    COMMAND langgraph_cpp_cache_test)

add_executable(langgraph_cpp_json_schema_test
    json_schema_test.cpp)

target_link_libraries(langgraph_cpp_json_schema_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_json_schema_test
    COMMAND langgraph_cpp_json_schema_test)

add_executable(langgraph_cpp_event_test
    event_test.cpp)

target_link_libraries(langgraph_cpp_event_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_event_test
    COMMAND langgraph_cpp_event_test)

add_executable(langgraph_cpp_resource_limits_test
    resource_limits_test.cpp)

target_link_libraries(langgraph_cpp_resource_limits_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_resource_limits_test
    COMMAND langgraph_cpp_resource_limits_test)

add_executable(langgraph_cpp_retry_policy_test
    retry_policy_test.cpp)

target_link_libraries(langgraph_cpp_retry_policy_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_retry_policy_test
    COMMAND langgraph_cpp_retry_policy_test)

add_executable(langgraph_cpp_rate_limit_test
    rate_limit_test.cpp)

target_link_libraries(langgraph_cpp_rate_limit_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_rate_limit_test
    COMMAND langgraph_cpp_rate_limit_test)

add_executable(langgraph_cpp_scheduler_test
    scheduler_test.cpp)

target_link_libraries(langgraph_cpp_scheduler_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_scheduler_test
    COMMAND langgraph_cpp_scheduler_test)

add_executable(langgraph_cpp_lifecycle_test
    lifecycle_test.cpp)

target_link_libraries(langgraph_cpp_lifecycle_test
    PRIVATE
        core)

add_test(NAME langgraph_cpp_lifecycle_test
    COMMAND langgraph_cpp_lifecycle_test)

add_executable(langgraph_cpp_runtime_services_test
    runtime_services_test.cpp)

target_link_libraries(langgraph_cpp_runtime_services_test
    PRIVATE
        core)

add_test(NAME langgraph_cpp_runtime_services_test
    COMMAND langgraph_cpp_runtime_services_test)

add_executable(langgraph_cpp_filesystem_test
    filesystem_test.cpp)

target_link_libraries(langgraph_cpp_filesystem_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_filesystem_test
    COMMAND langgraph_cpp_filesystem_test)

add_executable(langgraph_cpp_compression_test
    compression_test.cpp)

target_link_libraries(langgraph_cpp_compression_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_compression_test
    COMMAND langgraph_cpp_compression_test)

add_executable(langgraph_cpp_observability_test
    observability_test.cpp)

target_link_libraries(langgraph_cpp_observability_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_observability_test
    COMMAND langgraph_cpp_observability_test)

add_executable(langgraph_cpp_redaction_test
    redaction_test.cpp)

target_link_libraries(langgraph_cpp_redaction_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_redaction_test
    COMMAND langgraph_cpp_redaction_test)

if(LANGGRAPH_CPP_WITH_CRYPTO)
    add_executable(langgraph_cpp_crypto_test
        crypto_test.cpp)

    target_link_libraries(langgraph_cpp_crypto_test
        PRIVATE
            langgraph)

    add_test(NAME langgraph_cpp_crypto_test
        COMMAND langgraph_cpp_crypto_test)

    add_executable(langgraph_cpp_encryption_test
        encryption_test.cpp)

    target_link_libraries(langgraph_cpp_encryption_test
        PRIVATE
            langgraph)

    add_test(NAME langgraph_cpp_encryption_test
        COMMAND langgraph_cpp_encryption_test)
endif()

if(LANGGRAPH_CPP_WITH_NETWORK)
    add_executable(langgraph_cpp_http_client_test
        http_client_test.cpp)

    target_link_libraries(langgraph_cpp_http_client_test
        PRIVATE
            langgraph)

    add_test(NAME langgraph_cpp_http_client_test
        COMMAND langgraph_cpp_http_client_test)
endif()

add_executable(langgraph_cpp_logging_test
    logging_test.cpp)

target_link_libraries(langgraph_cpp_logging_test
    PRIVATE
        langgraph)

add_test(NAME langgraph_cpp_logging_test
    COMMAND langgraph_cpp_logging_test)
