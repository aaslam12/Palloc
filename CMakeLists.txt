cmake_minimum_required(VERSION 3.10)
project(slaballocator VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------
# Options
# -----------------------
option(SLABALLOCATOR_BUILD_TESTS "Build unit tests" OFF)
option(SLABALLOCATOR_BUILD_STRESS_TESTS "Build performance stress tests" OFF)
option(SLABALLOCATOR_STATIC_LINKING "Link libraries statically" OFF)
option(SLABALLOCATOR_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (only in Debug)" OFF)
option(SLABALLOCATOR_USE_CLANG_TIDY "Run clang-tidy during builds if available" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Note: cxx_std_20 has been set somewhere below as well
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------
# Global warnings and includes
# -----------------------
add_compile_options(-Wall -Wextra -Wpedantic)

# Add color to g++ output
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always")
include_directories(${CMAKE_SOURCE_DIR}/include)

if(SLABALLOCATOR_USE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
  if(CLANG_TIDY_EXE)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
  else()
    message(WARNING "clang-tidy requested but not found.")
  endif()
endif()

# -----------------------
# Sources
# -----------------------
file(GLOB_RECURSE ALL_SRC CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/*.c"
)

# exclude main.cpp from core library if present
set(MAIN_CPP "${CMAKE_SOURCE_DIR}/src/main.cpp")
list(FILTER ALL_SRC EXCLUDE REGEX ".*/main.cpp$")

# -----------------------
# Core library
# -----------------------
add_library(core STATIC ${ALL_SRC})
target_include_directories(core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(core PUBLIC cxx_std_20)
target_link_libraries(core PUBLIC)

# Define macros for build type and testing
if(SLABALLOCATOR_BUILD_TESTS)
  target_compile_definitions(core PUBLIC SLABALLOCATOR_TESTING)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(core PUBLIC SLABALLOCATOR_DEBUG)
else()
  target_compile_definitions(core PUBLIC SLABALLOCATOR_RELEASE)
endif()

if(SLABALLOCATOR_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(core PRIVATE -fsanitize=address,undefined)
endif()

# -----------------------
# Executable
# -----------------------
add_executable(slaballocator "${MAIN_CPP}")
target_link_libraries(slaballocator PRIVATE core)

if(SLABALLOCATOR_STATIC_LINKING)
  target_link_options(slaballocator PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(SLABALLOCATOR_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(slaballocator PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(slaballocator PRIVATE -fsanitize=address,undefined)
endif()

# -----------------------
# Tests
# -----------------------
include(CTest)
if(SLABALLOCATOR_BUILD_TESTS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  find_package(Catch2 3 REQUIRED)

  file(GLOB_RECURSE TEST_SRCS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
  if(TEST_SRCS)
    add_executable(tests ${TEST_SRCS})
    target_link_libraries(tests PRIVATE core Catch2::Catch2WithMain)
    if(SLABALLOCATOR_STATIC_LINKING)
      target_link_options(tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME unit_tests COMMAND tests --colour-mode ansi)
    set_tests_properties(unit_tests PROPERTIES LABELS "unit")
  endif()
endif()

# -----------------------
# Stress Tests
# -----------------------
if(SLABALLOCATOR_BUILD_STRESS_TESTS)
  file(GLOB STRESS_SRCS "stress_tests/*.cpp")
  foreach(stress_src ${STRESS_SRCS})
    get_filename_component(test_name ${stress_src} NAME_WE)
    add_executable(${test_name} ${stress_src})
    target_link_libraries(${test_name} PRIVATE core)
    if(SLABALLOCATOR_STATIC_LINKING)
      target_link_options(${test_name} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endforeach()
endif()

# -----------------------
# Format target
# -----------------------
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-14)
if(CLANG_FORMAT_EXE)
  file(GLOB_RECURSE FORMAT_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.c" "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/include/*.h" "${CMAKE_SOURCE_DIR}/include/*.hpp"
  )
  add_custom_target(format
      COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_FILES}
      COMMENT "Running clang-format on sources"
  )
endif()

# -----------------------
# Helpful messages
# -----------------------
message(STATUS "Project: ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build tests: ${SLABALLOCATOR_BUILD_TESTS}")
