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
    src/backend/linux_sysfs/backend_linux_sysfs.c
    src/backend/linux_vfio/backend_linux_vfio.c
    src/loader/elf_reader.c
    src/loader/module_loader.c
    src/subsystem/block/block.c
    src/subsystem/dma/dma.c
    src/subsystem/input/input.c
    src/subsystem/nvme/nvme.c
    src/subsystem/pci/pci.c
    src/subsystem/usb/usb.c
    src/shim/linux_context.c
    src/shim/linux_block.c
    src/shim/linux_device.c
    src/shim/linux_dma.c
    src/shim/linux_dma_pool.c
    src/shim/linux_hwmon.c
    src/shim/linux_irq.c
    src/shim/linux_memory.c
    src/shim/linux_mmio.c
    src/shim/linux_netdev.c
    src/shim/linux_nvme.c
    src/shim/linux_pci.c
    src/shim/linux_platform.c
    src/shim/linux_printk.c
    src/shim/linux_stubs.c
    src/shim/linux_sync.c
    src/shim/linux_usb.c
    src/shim/linux_virtio_input.c
    src/shim/linux_work.c
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
target_include_directories(kobox-run-init PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
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
target_include_directories(kobox-run PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-run PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(kobox-ls-devices
    tools/kobox_ls_devices/main.c
)

target_link_libraries(kobox-ls-devices PRIVATE kobox_runtime)
target_compile_options(kobox-ls-devices PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(kobox-vfio-edu-smoke
    tools/kobox_vfio_edu_smoke/main.c
)

target_link_libraries(kobox-vfio-edu-smoke PRIVATE kobox_runtime)
target_compile_options(kobox-vfio-edu-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(kobox-vfio-nvme-smoke
    tools/kobox_vfio_nvme_smoke/main.c
)

target_link_libraries(kobox-vfio-nvme-smoke PRIVATE kobox_runtime)
target_compile_options(kobox-vfio-nvme-smoke PRIVATE
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

add_executable(kobox-usb-subsystem-smoke
    tests/usb_subsystem_smoke.c
    src/subsystem/usb/usb.c
)

target_include_directories(kobox-usb-subsystem-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-usb-subsystem-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(NAME kobox-usb-subsystem-smoke COMMAND kobox-usb-subsystem-smoke)

add_executable(kobox-input-subsystem-smoke
    tests/input_subsystem_smoke.c
    src/subsystem/input/input.c
)

target_include_directories(kobox-input-subsystem-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-input-subsystem-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(NAME kobox-input-subsystem-smoke COMMAND kobox-input-subsystem-smoke)

add_executable(kobox-block-subsystem-smoke
    tests/block_subsystem_smoke.c
    src/subsystem/block/block.c
)

target_include_directories(kobox-block-subsystem-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-block-subsystem-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(NAME kobox-block-subsystem-smoke COMMAND kobox-block-subsystem-smoke)

add_test(
    NAME kobox-live-script-args
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_script_args.sh
)

add_test(
    NAME kobox-nvidia-mock-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_nvidia_mock_smoke.sh
)

add_test(
    NAME kobox-nvidia-modeset-mock-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_nvidia_modeset_mock_smoke.sh
)

add_test(
    NAME kobox-nvidia-uvm-mock-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_nvidia_uvm_mock_smoke.sh
)

add_test(
    NAME kobox-nvidia-drm-mock-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_nvidia_drm_mock_smoke.sh
)

add_test(
    NAME kobox-nvidia-stack-mock-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_nvidia_stack_mock_smoke.sh
)

add_test(
    NAME kobox-xhci-pci-mock-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_xhci_pci_mock_smoke.sh
)

add_test(
    NAME kobox-xhci-pci-vfio-smoke
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_xhci_pci_vfio_smoke.sh
)

add_test(
    NAME kobox-linux-sysfs
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_linux_sysfs_smoke.sh
        $<TARGET_FILE:kobox-ls-devices>
)

add_test(
    NAME kobox-fixture-modules
    COMMAND sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_fixture_modules.sh
        $<TARGET_FILE:kobox-run>
        tests/fixtures/linux_module/kobox_empty.ko
        tests/fixtures/linux_module/kobox_probe.ko
        tests/fixtures/linux_module/kobox_driverish.ko
        tests/fixtures/linux_module/kobox_pci_minimal.ko
)
set_tests_properties(kobox-fixture-modules PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
