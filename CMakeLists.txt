cmake_minimum_required(VERSION 3.24)

project(langgraph_cpp
    VERSION 0.1.0
    DESCRIPTION "C++ edge runtime for stateful agents"
    LANGUAGES C CXX)

option(LANGGRAPH_CPP_BUILD_TESTS "Build langgraph-cpp tests" ON)
option(LANGGRAPH_CPP_BUILD_EXAMPLES "Build langgraph-cpp examples" ON)
option(LANGGRAPH_CPP_BUILD_FUZZERS "Build libFuzzer harnesses for pure logic modules" OFF)
option(LANGGRAPH_CPP_ENABLE_PYTHON_LANGGRAPH_CONFORMANCE
    "Enable optional Python LangGraph conformance tests when python langgraph is installed" OFF)
option(LANGGRAPH_CPP_WITH_CRYPTO "Build OpenSSL-backed crypto foundation components" ON)
option(LANGGRAPH_CPP_WITH_NETWORK "Build cpp-httplib-backed network foundation components" ON)
option(LANGGRAPH_CPP_WITH_SQLITE "Build SQLite-backed storage foundation components" ON)
option(LANGGRAPH_CPP_WITH_GZIP "Build zlib-backed gzip compression support when zlib is available" ON)
option(LANGGRAPH_CPP_WITH_SPDLOG "Build spdlog-backed logging support when spdlog is available" ON)
option(LANGGRAPH_CPP_WITH_LLAMA_CPP "Build optional llama.cpp chat model adapter" OFF)
set(LANGGRAPH_CPP_LLAMA_CPP_DIR "" CACHE PATH "Path to a llama.cpp source tree or install prefix")
set(LANGGRAPH_CPP_LLAMA_TARGET "" CACHE STRING "Existing CMake target to link for llama.cpp")
set(LANGGRAPH_CPP_FUZZER_SANITIZERS "address,undefined" CACHE STRING
    "Sanitizers to pair with libFuzzer when LANGGRAPH_CPP_BUILD_FUZZERS=ON")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_XCODE_GENERATE_SCHEME ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)

if(LANGGRAPH_CPP_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "LANGGRAPH_CPP_BUILD_FUZZERS=ON requires Clang/AppleClang with libFuzzer support")
    endif()
    if(LANGGRAPH_CPP_FUZZER_SANITIZERS STREQUAL "")
        message(FATAL_ERROR "LANGGRAPH_CPP_FUZZER_SANITIZERS must not be empty")
    endif()
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,${LANGGRAPH_CPP_FUZZER_SANITIZERS}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer,${LANGGRAPH_CPP_FUZZER_SANITIZERS}")
    check_cxx_source_compiles("
        #include <cstddef>
        #include <cstdint>
        extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
    " LANGGRAPH_CPP_HAS_LIBFUZZER_RUNTIME)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT LANGGRAPH_CPP_HAS_LIBFUZZER_RUNTIME)
        message(FATAL_ERROR
            "LANGGRAPH_CPP_BUILD_FUZZERS=ON requires a Clang toolchain with a linkable "
            "libFuzzer runtime. On macOS, install/use a full LLVM toolchain such as "
            "Homebrew llvm and set CMAKE_CXX_COMPILER to its clang++.")
    endif()
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=fuzzer-no-link,${LANGGRAPH_CPP_FUZZER_SANITIZERS}>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-omit-frame-pointer>)
    add_link_options(-fsanitize=${LANGGRAPH_CPP_FUZZER_SANITIZERS})
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_XCODE_GENERATE_SCHEME ON CACHE BOOL "Generate Xcode schemes" FORCE)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
    set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "YES")
endif()

include(GNUInstallDirs)
include(CTest)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(LANGGRAPH_CPP_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
function(langgraph_cpp_require_file required_file help_text)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "${help_text}")
    endif()
endfunction()

function(langgraph_cpp_collect_sources out_var root_dir)
    file(GLOB_RECURSE collected CONFIGURE_DEPENDS
        "${root_dir}/*.h"
        "${root_dir}/*.hpp"
        "${root_dir}/*.hh"
        "${root_dir}/*.hxx"
        "${root_dir}/*.c"
        "${root_dir}/*.cc"
        "${root_dir}/*.cpp"
        "${root_dir}/*.cxx")

    list(SORT collected)
    set(${out_var} ${collected} PARENT_SCOPE)
endfunction()

function(langgraph_cpp_source_groups root_dir)
    foreach(source_file IN LISTS ARGN)
        get_filename_component(source_dir "${source_file}" DIRECTORY)
        file(RELATIVE_PATH group_name "${root_dir}" "${source_dir}")
        if(group_name STREQUAL "")
            set(group_name "\\")
        endif()
        string(REPLACE "/" "\\" group_name "${group_name}")
        source_group("${group_name}" FILES "${source_file}")
    endforeach()
endfunction()

function(langgraph_cpp_configure_xcode_scheme target_name)
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(${target_name} PROPERTIES
            XCODE_GENERATE_SCHEME ON
            XCODE_SCHEME_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            XCODE_SCHEME_ENABLE_ADDRESS_SANITIZER OFF
            XCODE_SCHEME_ENABLE_THREAD_SANITIZER OFF
            XCODE_SCHEME_ENABLE_UNDEFINED_BEHAVIOUR_SANITIZER OFF)
    endif()
endfunction()

langgraph_cpp_require_file("${LANGGRAPH_CPP_THIRD_PARTY_DIR}/nlohmann_json/include/nlohmann/json.hpp"
    "Missing third_party/nlohmann_json. Run: git submodule update --init --recursive")

set(LANGGRAPH_CPP_PUBLIC_INCLUDE_DIRS
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set(LANGGRAPH_CPP_SYSTEM_INCLUDE_DIRS
    "$<BUILD_INTERFACE:${LANGGRAPH_CPP_THIRD_PARTY_DIR}/nlohmann_json/include>")

set(LANGGRAPH_CPP_PRIVATE_INCLUDE_DIRS "")
set(LANGGRAPH_CPP_PUBLIC_LIBS "")
set(LANGGRAPH_CPP_PUBLIC_DEFINITIONS
    LANGGRAPH_CPP_WITH_CRYPTO=$<BOOL:${LANGGRAPH_CPP_WITH_CRYPTO}>
    LANGGRAPH_CPP_WITH_NETWORK=$<BOOL:${LANGGRAPH_CPP_WITH_NETWORK}>
    LANGGRAPH_CPP_WITH_SQLITE=$<BOOL:${LANGGRAPH_CPP_WITH_SQLITE}>
    LANGGRAPH_CPP_WITH_GZIP=$<BOOL:${LANGGRAPH_CPP_WITH_GZIP}>
    LANGGRAPH_CPP_WITH_LLAMA_CPP=$<BOOL:${LANGGRAPH_CPP_WITH_LLAMA_CPP}>)

find_package(Threads REQUIRED)
list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS ${CMAKE_THREAD_LIBS_INIT})

if(LANGGRAPH_CPP_WITH_SPDLOG)
    find_package(spdlog QUIET)
endif()

if(spdlog_FOUND)
    list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS spdlog::spdlog)
    list(APPEND LANGGRAPH_CPP_PUBLIC_DEFINITIONS LC_HAS_EXTERNAL_SPDLOG=1)
else()
    list(APPEND LANGGRAPH_CPP_PUBLIC_DEFINITIONS LC_HAS_EXTERNAL_SPDLOG=0)
    if(LANGGRAPH_CPP_WITH_SPDLOG)
        message(STATUS "spdlog not found. langgraph_cpp SpdLogger will be unavailable.")
    endif()
endif()

if(LANGGRAPH_CPP_WITH_GZIP)
    find_package(ZLIB QUIET)
endif()

if(ZLIB_FOUND)
    list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS ZLIB::ZLIB)
    list(APPEND LANGGRAPH_CPP_PUBLIC_DEFINITIONS LANGGRAPH_CPP_HAS_ZLIB=1)
else()
    list(APPEND LANGGRAPH_CPP_PUBLIC_DEFINITIONS LANGGRAPH_CPP_HAS_ZLIB=0)
    if(LANGGRAPH_CPP_WITH_GZIP)
        message(STATUS "zlib not found. langgraph_cpp gzip compression will be unavailable.")
    endif()
endif()

if(LANGGRAPH_CPP_WITH_CRYPTO)
    find_package(OpenSSL REQUIRED)
elseif(LANGGRAPH_CPP_WITH_NETWORK)
    find_package(OpenSSL QUIET)
endif()

if(OpenSSL_FOUND)
    list(APPEND LANGGRAPH_CPP_SYSTEM_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
endif()

if(LANGGRAPH_CPP_WITH_CRYPTO)
    list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS OpenSSL::Crypto)
endif()

if(WIN32)
    list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS bcrypt)
endif()

# llama.cpp is a runtime (langgraph) concern, not a foundation concern. Its link
# requirements go into runtime-specific variables so foundation never sees them.
set(LANGGRAPH_CPP_RUNTIME_LIBS "")
set(LANGGRAPH_CPP_RUNTIME_SYSTEM_INCLUDE_DIRS "")

if(LANGGRAPH_CPP_WITH_LLAMA_CPP)
    set(LANGGRAPH_CPP_LLAMA_SOURCE_CANDIDATE "${LANGGRAPH_CPP_LLAMA_CPP_DIR}")
    if(LANGGRAPH_CPP_LLAMA_SOURCE_CANDIDATE STREQUAL "")
        set(LANGGRAPH_CPP_LLAMA_SOURCE_CANDIDATE "${LANGGRAPH_CPP_THIRD_PARTY_DIR}/llama.cpp")
    endif()

    if(NOT LANGGRAPH_CPP_LLAMA_TARGET STREQUAL "")
        list(APPEND LANGGRAPH_CPP_RUNTIME_LIBS "${LANGGRAPH_CPP_LLAMA_TARGET}")
    elseif(EXISTS "${LANGGRAPH_CPP_LLAMA_SOURCE_CANDIDATE}/CMakeLists.txt")
        set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            "${LANGGRAPH_CPP_LLAMA_SOURCE_CANDIDATE}"
            "${CMAKE_BINARY_DIR}/llama.cpp"
            EXCLUDE_FROM_ALL)
        if(TARGET llama)
            list(APPEND LANGGRAPH_CPP_RUNTIME_LIBS llama)
        else()
            message(FATAL_ERROR "LANGGRAPH_CPP_WITH_LLAMA_CPP=ON but llama.cpp did not define a 'llama' target")
        endif()
    else()
        find_package(llama CONFIG QUIET)
        if(TARGET llama)
            list(APPEND LANGGRAPH_CPP_RUNTIME_LIBS llama)
        elseif(TARGET llama::llama)
            list(APPEND LANGGRAPH_CPP_RUNTIME_LIBS llama::llama)
        else()
            find_path(LANGGRAPH_CPP_LLAMA_INCLUDE_DIR
                NAMES llama.h
                HINTS
                    "${LANGGRAPH_CPP_LLAMA_CPP_DIR}/include"
                    "${LANGGRAPH_CPP_LLAMA_CPP_DIR}")
            find_library(LANGGRAPH_CPP_LLAMA_LIBRARY
                NAMES llama
                HINTS
                    "${LANGGRAPH_CPP_LLAMA_CPP_DIR}/lib"
                    "${LANGGRAPH_CPP_LLAMA_CPP_DIR}/build"
                    "${LANGGRAPH_CPP_LLAMA_CPP_DIR}/build/src")
            if(NOT LANGGRAPH_CPP_LLAMA_INCLUDE_DIR OR NOT LANGGRAPH_CPP_LLAMA_LIBRARY)
                message(FATAL_ERROR
                    "LANGGRAPH_CPP_WITH_LLAMA_CPP=ON requires llama.cpp. "
                    "Set LANGGRAPH_CPP_LLAMA_CPP_DIR to a llama.cpp source/install path "
                    "or LANGGRAPH_CPP_LLAMA_TARGET to an existing CMake target.")
            endif()
            list(APPEND LANGGRAPH_CPP_RUNTIME_SYSTEM_INCLUDE_DIRS "${LANGGRAPH_CPP_LLAMA_INCLUDE_DIR}")
            list(APPEND LANGGRAPH_CPP_RUNTIME_LIBS "${LANGGRAPH_CPP_LLAMA_LIBRARY}")
        endif()
    endif()
endif()

function(langgraph_cpp_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    else()
        target_compile_options(${target_name} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
    endif()
endfunction()

# ============================================================================
#  foundation — standalone, general-purpose library.
#  Must NOT depend on anything under src/langgraph. Owns every optional
#  third-party dependency (OpenSSL, cpp-httplib, SQLite, zlib, spdlog).
# ============================================================================
langgraph_cpp_collect_sources(LC_FOUNDATION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/foundation")

if(NOT LANGGRAPH_CPP_WITH_CRYPTO)
    list(FILTER LC_FOUNDATION_SOURCES EXCLUDE REGEX "/crypto/")
endif()

if(LANGGRAPH_CPP_WITH_NETWORK)
    langgraph_cpp_require_file("${LANGGRAPH_CPP_THIRD_PARTY_DIR}/cpp-httplib/httplib.h"
        "Missing third_party/cpp-httplib. Run: git submodule update --init --recursive")

    list(APPEND LANGGRAPH_CPP_SYSTEM_INCLUDE_DIRS
        "$<BUILD_INTERFACE:${LANGGRAPH_CPP_THIRD_PARTY_DIR}/cpp-httplib>")

    if(OpenSSL_FOUND)
        list(APPEND LANGGRAPH_CPP_PUBLIC_DEFINITIONS CPPHTTPLIB_OPENSSL_SUPPORT)
        list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS OpenSSL::SSL OpenSSL::Crypto)
        if(APPLE)
            find_library(LANGGRAPH_CPP_SECURITY_FRAMEWORK Security REQUIRED)
            find_library(LANGGRAPH_CPP_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
            list(APPEND LANGGRAPH_CPP_PUBLIC_LIBS
                ${LANGGRAPH_CPP_SECURITY_FRAMEWORK}
                ${LANGGRAPH_CPP_COREFOUNDATION_FRAMEWORK})
        endif()
    else()
        message(STATUS "OpenSSL not found. foundation HTTP client will compile without HTTPS support.")
    endif()
else()
    list(FILTER LC_FOUNDATION_SOURCES EXCLUDE REGEX "/network/")
endif()

if(LANGGRAPH_CPP_WITH_SQLITE)
    langgraph_cpp_require_file("${LANGGRAPH_CPP_THIRD_PARTY_DIR}/sqlite/sqlite3.c"
        "Missing third_party/sqlite/sqlite3.c. Run: git submodule update --init --recursive")
    langgraph_cpp_require_file("${LANGGRAPH_CPP_THIRD_PARTY_DIR}/sqlite/sqlite3.h"
        "Missing third_party/sqlite/sqlite3.h. Run: git submodule update --init --recursive")

    list(APPEND LC_FOUNDATION_SOURCES
        "${LANGGRAPH_CPP_THIRD_PARTY_DIR}/sqlite/sqlite3.c")
    list(APPEND LANGGRAPH_CPP_SYSTEM_INCLUDE_DIRS
        "$<BUILD_INTERFACE:${LANGGRAPH_CPP_THIRD_PARTY_DIR}/sqlite>")
    list(APPEND LANGGRAPH_CPP_PUBLIC_DEFINITIONS
        SQLITE_THREADSAFE=1
        SQLITE_OMIT_LOAD_EXTENSION)
else()
    list(FILTER LC_FOUNDATION_SOURCES EXCLUDE REGEX "/storage/sqlite_storage\\.cpp$")
endif()

langgraph_cpp_source_groups("${CMAKE_CURRENT_SOURCE_DIR}" ${LC_FOUNDATION_SOURCES})

add_library(foundation STATIC ${LC_FOUNDATION_SOURCES})
add_library(lc::foundation ALIAS foundation)
set_target_properties(foundation PROPERTIES FOLDER "langgraph-cpp")
langgraph_cpp_configure_xcode_scheme(foundation)

target_include_directories(foundation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(foundation
    SYSTEM PUBLIC
        ${LANGGRAPH_CPP_SYSTEM_INCLUDE_DIRS})
target_compile_features(foundation PUBLIC cxx_std_23)
target_compile_definitions(foundation
    PUBLIC
        ${LANGGRAPH_CPP_PUBLIC_DEFINITIONS})
if(LANGGRAPH_CPP_PUBLIC_LIBS)
    target_link_libraries(foundation PUBLIC ${LANGGRAPH_CPP_PUBLIC_LIBS})
endif()
langgraph_cpp_apply_warnings(foundation)

# ============================================================================
#  core — application-assembly layer (composition root + lifecycle adapters).
#  Opinionated wiring that intentionally couples to many subsystems; kept out
#  of foundation so the foundation stays a set of decoupled primitives.
# ============================================================================
langgraph_cpp_collect_sources(LC_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core")

langgraph_cpp_source_groups("${CMAKE_CURRENT_SOURCE_DIR}" ${LC_CORE_SOURCES})

add_library(core STATIC ${LC_CORE_SOURCES})
add_library(lc::core ALIAS core)
set_target_properties(core PROPERTIES FOLDER "langgraph-cpp")
langgraph_cpp_configure_xcode_scheme(core)

target_include_directories(core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(core PUBLIC lc::foundation)
target_compile_features(core PUBLIC cxx_std_23)
langgraph_cpp_apply_warnings(core)

# ============================================================================
#  langgraph_cpp — stateful agent graph runtime.
#  Depends only on lc::foundation (which transitively carries the foundation
#  headers, defs and third-party deps) plus the optional llama.cpp adapter.
# ============================================================================
langgraph_cpp_collect_sources(LC_RUNTIME_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/langgraph")

if(NOT LANGGRAPH_CPP_WITH_LLAMA_CPP)
    list(FILTER LC_RUNTIME_SOURCES EXCLUDE REGEX "/model/llamacpp_chat_model\\.cpp$")
endif()

langgraph_cpp_source_groups("${CMAKE_CURRENT_SOURCE_DIR}" ${LC_RUNTIME_SOURCES})

add_library(langgraph STATIC ${LC_RUNTIME_SOURCES})
add_library(lc::langgraph ALIAS langgraph)
set_target_properties(langgraph PROPERTIES FOLDER "langgraph-cpp")
langgraph_cpp_configure_xcode_scheme(langgraph)

target_include_directories(langgraph
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(langgraph PUBLIC lc::foundation)
if(LANGGRAPH_CPP_RUNTIME_LIBS)
    target_link_libraries(langgraph PUBLIC ${LANGGRAPH_CPP_RUNTIME_LIBS})
endif()
if(LANGGRAPH_CPP_RUNTIME_SYSTEM_INCLUDE_DIRS)
    target_include_directories(langgraph
        SYSTEM PUBLIC
            ${LANGGRAPH_CPP_RUNTIME_SYSTEM_INCLUDE_DIRS})
endif()
target_compile_features(langgraph PUBLIC cxx_std_23)
langgraph_cpp_apply_warnings(langgraph)

# ---- Install / export: lc::foundation is consumable on its own ----
install(TARGETS foundation core langgraph
    EXPORT langgraphCppTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/foundation"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/langgraph"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT langgraphCppTargets
    NAMESPACE lc::
    FILE langgraphCppTargets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/langgraph_cpp")

if(LANGGRAPH_CPP_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(LANGGRAPH_CPP_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(LANGGRAPH_CPP_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
