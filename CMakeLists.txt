cmake_minimum_required(VERSION 3.16)
project(vmtouch LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vmtouch_core STATIC
    src/core/platform.cpp
    src/core/cache.cpp
    src/core/session.cpp
    src/core/crawl.cpp
    src/core/file_handle.cpp
)
add_executable(vmtouch src/cli/main.cpp)
target_link_libraries(vmtouch PRIVATE vmtouch_core)

target_include_directories(vmtouch_core PUBLIC src tests)
target_include_directories(vmtouch PRIVATE src)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(vmtouch_core PUBLIC _FILE_OFFSET_BITS=64 _DEFAULT_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "HP-UX" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(vmtouch_core PUBLIC _FILE_OFFSET_BITS=64)
endif()

# Warning flags: -Wall -Wextra always; CI's plain job passes
# -DWARN_FLAGS=-Werror to make any warning fatal.
set(WARN_FLAGS "" CACHE STRING "Extra warning flags (e.g. -Werror)")
target_compile_options(vmtouch PRIVATE -Wall -Wextra ${WARN_FLAGS})
target_compile_options(vmtouch_core PRIVATE -Wall -Wextra ${WARN_FLAGS})
if(TARGET unit_tests)
    target_compile_options(unit_tests PRIVATE -Wall -Wextra ${WARN_FLAGS})
endif()

option(VMFTOUCH_SANITIZE "Build with ASan+UBSan" OFF)
if(VMFTOUCH_SANITIZE)
    set(SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    set(SANITIZE_TARGETS vmtouch vmtouch_core unit_tests)
else()
    set(SANITIZE_TARGETS "")
endif()

include(GNUInstallDirs)

install(TARGETS vmtouch
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# --- unit tests (dependency-free harness in tests/unit/) -----------------
add_executable(unit_tests
    tests/unit/main.cpp
    tests/unit/test_platform.cpp
    tests/unit/test_file_handle.cpp
    tests/unit/test_cache.cpp
    tests/unit/test_session.cpp
    tests/unit/test_crawl.cpp
)
target_link_libraries(unit_tests PRIVATE vmtouch_core)
target_include_directories(unit_tests PRIVATE src tests)

if(VMFTOUCH_SANITIZE)
    foreach(t ${SANITIZE_TARGETS})
        target_compile_options(${t} PRIVATE ${SANITIZE_FLAGS})
        target_link_options(${t} PRIVATE ${SANITIZE_FLAGS})
    endforeach()
endif()

enable_testing()
add_test(NAME characterization
    COMMAND ${CMAKE_SOURCE_DIR}/tests/run.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(characterization PROPERTIES ENVIRONMENT "VMFILE=$<TARGET_FILE:vmtouch>")

# The mlock-fatal characterization needs a non-sanitizer binary (ASan's mlock
# interceptor succeeds where the real syscall fails). In the sanitizer build,
# also look for a sibling plain build directory; in the plain build, VMFILE
# itself is non-sanitized and the test uses it directly.

add_test(NAME unit COMMAND unit_tests)
