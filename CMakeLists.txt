cmake_minimum_required(VERSION 3.10)
project(vmtouch LANGUAGES C)

add_executable(vmtouch vmtouch.c)

target_compile_options(vmtouch PRIVATE -Wall -Wextra)
target_compile_features(vmtouch PRIVATE c_std_99)

option(VMFTOUCH_SANITIZE "Build with ASan+UBSan" OFF)
if(VMFTOUCH_SANITIZE)
    target_compile_options(vmtouch PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(vmtouch PRIVATE -fsanitize=address,undefined)
endif()

include(GNUInstallDirs)

install(TARGETS vmtouch
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
