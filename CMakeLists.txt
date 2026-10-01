cmake_minimum_required(VERSION 3.16)

project(NocteDMXCoreTests LANGUAGES C CXX)

enable_testing()

add_executable(
  noctedmx_core_tests
  tests/native/test_core.cpp
  src/nocte/core/DmxFrame.cpp
  src/nocte/core/RdmPacket.cpp
  src/rdm/rdm_utility.c
)

target_include_directories(noctedmx_core_tests PRIVATE src)
target_compile_features(noctedmx_core_tests PRIVATE cxx_std_11)

if(MSVC)
  target_compile_options(noctedmx_core_tests PRIVATE /W4 /WX)
else()
  target_compile_options(
    noctedmx_core_tests
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
  )
endif()

add_test(NAME noctedmx_core COMMAND noctedmx_core_tests)
