function(langgraph_cpp_add_example example_name)
    add_executable(${example_name}
        ${example_name}.cpp)

    target_link_libraries(${example_name}
        PRIVATE
            langgraph)

    set_target_properties(${example_name} PROPERTIES
        FOLDER "langgraph-cpp/examples")
    langgraph_cpp_configure_xcode_scheme(${example_name})
endfunction()

langgraph_cpp_add_example(minimal_graph)
langgraph_cpp_add_example(parallel_fanout)
langgraph_cpp_add_example(conditional_fanout)
langgraph_cpp_add_example(send_map_reduce)
langgraph_cpp_add_example(command_goto)
langgraph_cpp_add_example(conditional_graph)
langgraph_cpp_add_example(loop_graph)
langgraph_cpp_add_example(checkpoint_resume)
if(LANGGRAPH_CPP_WITH_SQLITE)
    langgraph_cpp_add_example(sqlite_checkpoint_resume)
endif()
langgraph_cpp_add_example(time_travel_history)
langgraph_cpp_add_example(long_term_memory_store)
langgraph_cpp_add_example(stream_projection)
langgraph_cpp_add_example(subgraph_module)
langgraph_cpp_add_example(model_tool_model_loop)
if(LANGGRAPH_CPP_WITH_LLAMA_CPP)
    langgraph_cpp_add_example(llama_cpp_chat)
    langgraph_cpp_add_example(llama_cpp_tool_calling)
endif()
langgraph_cpp_add_example(human_interrupt)
langgraph_cpp_add_example(tool_approval_loop)
langgraph_cpp_add_example(edge_mock_tool_adapter)
langgraph_cpp_add_example(mock_edge_repair)
