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
    src/device/device_backend.c
    src/host_interface/interface.c
    src/host_interface/linux_interface_common.c
    src/host_interface/linux_ipc/interface_linux_ipc.c
    src/device/linux_mock/device_linux_mock.c
    src/device/pachaos_capsule/device_pachaos_capsule.c
    src/device/linux_sysfs/device_linux_sysfs.c
    src/device/linux_vfio/device_linux_vfio.c
    src/device/linux_vfio/nvme.c
    src/device/linux_vfio/virtio_blk.c
    src/loader/elf_reader.c
    src/loader/module_loader.c
    src/loader/symbol_registry.c
    src/platform/platform.c
    src/linux_subsystem/block/block.c
    src/linux_subsystem/block/block_symbols.c
    src/linux_subsystem/dma/dma.c
    src/linux_subsystem/dma/dma_symbols.c
    src/linux_subsystem/fs/fs.c
    src/linux_subsystem/fs/fs_symbols.c
    src/linux_subsystem/fs/fops_smoke.c
    src/linux_subsystem/fs/kernel_object_registry.c
    src/linux_subsystem/input/input.c
    src/linux_subsystem/input/input_symbols.c
    src/linux_subsystem/kvm/kvm_symbols.c
    src/linux_subsystem/net/net_symbols.c
    src/linux_subsystem/nvme/nvme.c
    src/linux_subsystem/pci/pci.c
    src/linux_subsystem/pci/pci_symbols.c
    src/linux_subsystem/scsi/scsi.c
    src/linux_subsystem/security/security_symbols.c
    src/linux_subsystem/sound/sound_symbols.c
    src/linux_subsystem/usb/config.c
    src/linux_subsystem/usb/port_state.c
    src/linux_subsystem/usb/storage.c
    src/linux_subsystem/usb/synthetic.c
    src/linux_subsystem/usb/usb.c
    src/linux_subsystem/usb/usb_symbols.c
    src/linux_personality/linux_context.c
    src/linux_personality/linux_call.c
    src/linux_personality/linux_block.c
    src/linux_personality/linux_core_symbols.c
    src/linux_personality/linux_device.c
    src/linux_personality/linux_dma.c
    src/linux_personality/linux_dma_pool.c
    src/linux_personality/linux_hwmon.c
    src/linux_personality/linux_irq.c
    src/linux_personality/linux_memory.c
    src/linux_personality/linux_mmio.c
    src/linux_personality/linux_netdev.c
    src/linux_personality/linux_nvme.c
    src/linux_personality/linux_pci.c
    src/linux_personality/linux_platform.c
    src/linux_personality/linux_printk.c
    src/linux_personality/linux_scsi.c
    src/linux_personality/linux_stubs.c
    src/linux_personality/linux_stub_symbols.c
    src/linux_personality/linux_sync.c
    src/linux_personality/linux_usb.c
    src/linux_personality/linux_virtio_input.c
    src/linux_personality/linux_work.c
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

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(kobox_runtime PRIVATE
        -mstackrealign
    )
endif()

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

add_executable(kobox-ext4-real-ops
    tools/kobox_ext4_real_ops_probe/main.c
)

target_link_libraries(kobox-ext4-real-ops PRIVATE kobox_runtime)
target_include_directories(kobox-ext4-real-ops PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-ext4-real-ops PRIVATE
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
    src/linux_personality/linux_memory.c
    src/linux_subsystem/block/block.c
    src/linux_subsystem/usb/storage.c
    src/linux_subsystem/usb/usb.c
)

target_include_directories(kobox-usb-subsystem-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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
)

target_link_libraries(kobox-input-subsystem-smoke PRIVATE kobox_runtime)
target_include_directories(kobox-input-subsystem-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-input-subsystem-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(
    NAME kobox-input-subsystem-smoke
    COMMAND kobox-input-subsystem-smoke
)
set_tests_properties(kobox-input-subsystem-smoke PROPERTIES
    ENVIRONMENT KOBOX_INPUT_TRUST_DEVICE_STRINGS=1
)

add_executable(kobox-block-subsystem-smoke
    tests/block_subsystem_smoke.c
    src/linux_subsystem/block/block.c
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

add_executable(kobox-fs-subsystem-smoke
    tests/fs_subsystem_smoke.c
)

target_link_libraries(kobox-fs-subsystem-smoke PRIVATE kobox_runtime)
target_include_directories(kobox-fs-subsystem-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-fs-subsystem-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(NAME kobox-fs-subsystem-smoke COMMAND kobox-fs-subsystem-smoke)

add_executable(kobox-fs-block-disk-smoke
    tests/fs_block_disk_smoke.c
)

target_link_libraries(kobox-fs-block-disk-smoke PRIVATE kobox_runtime)
target_include_directories(kobox-fs-block-disk-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(kobox-fs-block-disk-smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_test(NAME kobox-fs-block-disk-smoke COMMAND kobox-fs-block-disk-smoke)

add_test(
    NAME kobox-ext4-real-ops
    COMMAND ${CMAKE_COMMAND} -E env
        KOBOX_EXT4_REAL_OPS_RUNNER=$<TARGET_FILE:kobox-ext4-real-ops>
        sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_ext4_real_ops_probe.sh
)
set_tests_properties(kobox-ext4-real-ops PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    SKIP_RETURN_CODE 77
)

add_test(
    NAME kobox-ext4-feature-matrix
    COMMAND ${CMAKE_COMMAND} -E env
        KOBOX_EXT4_REAL_OPS_RUNNER=$<TARGET_FILE:kobox-ext4-real-ops>
        sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_ext4_feature_matrix_probe.sh
)
set_tests_properties(kobox-ext4-feature-matrix PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    SKIP_RETURN_CODE 77
)

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
