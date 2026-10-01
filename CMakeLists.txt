cmake_minimum_required(VERSION 3.16)
project(vmtouch LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vmtouch_core STATIC src/core/core.cpp)  # module sources added incrementally
add_executable(vmtouch src/cli/main.cpp)
target_link_libraries(vmtouch PRIVATE vmtouch_core)

target_compile_options(vmtouch PRIVATE -Wall -Wextra)
target_compile_options(vmtouch_core PRIVATE -Wall -Wextra)

option(VMFTOUCH_SANITIZE "Build with ASan+UBSan" OFF)
if(VMFTOUCH_SANITIZE)
    set(SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_compile_options(vmtouch PRIVATE ${SANITIZE_FLAGS})
    target_link_options(vmtouch PRIVATE ${SANITIZE_FLAGS})
    target_compile_options(vmtouch_core PRIVATE ${SANITIZE_FLAGS})
    target_link_options(vmtouch_core PRIVATE ${SANITIZE_FLAGS})
endif()

include(GNUInstallDirs)

install(TARGETS vmtouch
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

enable_testing()
add_test(NAME characterization
    COMMAND ${CMAKE_SOURCE_DIR}/tests/run.sh
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(characterization PROPERTIES ENVIRONMENT "VMFILE=$<TARGET_FILE:vmtouch>")
