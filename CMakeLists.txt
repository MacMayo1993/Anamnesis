cmake_minimum_required(VERSION 3.16)
project(anamnesis VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Anamnesis requires 64-bit architecture")
endif()

option(ANAM_BUILD_TESTS "Build unit tests" ON)
option(ANAM_BUILD_BENCH "Build benchmarks" ON)
option(ANAM_ASAN "Enable AddressSanitizer" OFF)
option(ANAM_TRACE "Enable tracing infrastructure" OFF)

if(ANAM_ASAN AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(MSVC)
    add_compile_options(/W4 /WX /wd4100)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()

# Core library
add_library(anamnesis STATIC src/anamnesis.c src/anamnesis_trace.c)
target_include_directories(anamnesis PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(ANAM_TRACE)
    target_compile_definitions(anamnesis PUBLIC ANAM_TRACE_ENABLED)
endif()

# Queue library
add_library(anamnesis_queue STATIC src/anamnesis_queue.c)
target_include_directories(anamnesis_queue PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(anamnesis_queue PUBLIC anamnesis)

# Combined
add_library(anam INTERFACE)
target_link_libraries(anam INTERFACE anamnesis anamnesis_queue)

# Aliases
add_library(anam::core ALIAS anamnesis)
add_library(anam::queue ALIAS anamnesis_queue)
add_library(anam::anam ALIAS anam)

# Tests
if(ANAM_BUILD_TESTS)
    enable_testing()

    add_executable(test_anamnesis tests/test_anamnesis.c)
    target_link_libraries(test_anamnesis PRIVATE anamnesis)

    add_executable(test_queue tests/test_queue.c)
    target_link_libraries(test_queue PRIVATE anamnesis_queue)

    add_executable(stress_test tests/stress_test.c)
    target_link_libraries(stress_test PRIVATE anamnesis_queue)
    target_compile_definitions(stress_test PRIVATE STRESS_DURATION_SEC=10)

    add_executable(trace_test tests/trace_test.c)
    target_link_libraries(trace_test PRIVATE anamnesis)

    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(test_anamnesis PRIVATE Threads::Threads)
        target_link_libraries(test_queue PRIVATE Threads::Threads)
        target_link_libraries(stress_test PRIVATE Threads::Threads)
        target_link_libraries(trace_test PRIVATE Threads::Threads)
    endif()

    add_test(NAME anamnesis_tests COMMAND test_anamnesis)
    add_test(NAME queue_tests COMMAND test_queue)
    add_test(NAME stress_tests COMMAND stress_test)
endif()

# Benchmarks
if(ANAM_BUILD_BENCH)
    add_executable(bench_anamnesis bench/bench.cpp)
    target_compile_features(bench_anamnesis PRIVATE cxx_std_20)
    target_link_libraries(bench_anamnesis PRIVATE anamnesis)

    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(bench_anamnesis PRIVATE Threads::Threads)
    endif()
endif()

# Install
include(GNUInstallDirs)
install(TARGETS anamnesis anamnesis_queue
    EXPORT anamnesis_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
