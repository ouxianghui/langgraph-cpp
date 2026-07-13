function(langgraph_cpp_add_fuzzer target_name source_file)
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target_name}
        PRIVATE
            langgraph)
    target_link_options(${target_name}
        PRIVATE
            -fsanitize=fuzzer,${LANGGRAPH_CPP_FUZZER_SANITIZERS})
    set_target_properties(${target_name} PROPERTIES
        FOLDER "langgraph-cpp/fuzz")
endfunction()

langgraph_cpp_add_fuzzer(langgraph_cpp_json_schema_fuzzer json_schema_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_state_reducer_fuzzer state_reducer_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_checkpoint_codec_fuzzer checkpoint_codec_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_content_envelope_fuzzer content_envelope_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_sse_parser_fuzzer sse_parser_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_message_tool_json_fuzzer message_tool_json_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_graph_namespace_fuzzer graph_namespace_fuzzer.cpp)
langgraph_cpp_add_fuzzer(langgraph_cpp_run_config_fuzzer run_config_fuzzer.cpp)
