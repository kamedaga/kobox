cmake_minimum_required(VERSION 3.20)

project(kobox LANGUAGES C)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "kobox currently requires clang. Configure with -DCMAKE_C_COMPILER=clang.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(kobox_runtime STATIC
    src/backend/backend.c
    src/backend/linux_mock/backend_linux_mock.c
    src/loader/elf_reader.c
    src/loader/module_loader.c
    src/shim/linux_context.c
    src/shim/linux_dma.c
    src/shim/linux_irq.c
    src/shim/linux_memory.c
    src/shim/linux_printk.c
)

target_include_directories(kobox_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(kobox_runtime PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(kobox-inspect
    tools/kobox_inspect/main.c
)

target_link_libraries(kobox-inspect PRIVATE kobox_runtime)
target_compile_options(kobox-inspect PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(kobox-run-init
    tools/kobox_run_init/main.c
)

target_link_libraries(kobox-run-init PRIVATE kobox_runtime)
target_compile_options(kobox-run-init PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(kobox-run
    tools/kobox_run_init/main.c
)

target_link_libraries(kobox-run PRIVATE kobox_runtime)
target_compile_options(kobox-run PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

enable_testing()

add_executable(kobox-smoke
    tests/smoke.c
)

target_link_libraries(kobox-smoke PRIVATE kobox_runtime)
target_compile_options(kobox-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(NAME kobox-smoke COMMAND kobox-smoke)
