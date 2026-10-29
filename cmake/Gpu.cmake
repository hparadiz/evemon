set(NVTOP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvtop")
if(NOT EXISTS "${NVTOP_DIR}/src/extract_gpuinfo.c")
    message(FATAL_ERROR "GPU support requires the existing nvtop checkout. Use -DEVEMON_GPU=OFF for a smaller build.")
endif()
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
# Keep vendor registration constructors: these objects must reach the linker
# directly, rather than disappearing as apparently unused archive members.
add_library(evemon_gpu OBJECT
    src/plugins/gpu_nvtop_adapter.c src/plugins/gpu_dcmi_shim.c
    ${NVTOP_DIR}/src/extract_gpuinfo.c ${NVTOP_DIR}/src/time.c
    ${NVTOP_DIR}/src/get_process_info_linux.c
    ${NVTOP_DIR}/src/extract_processinfo_fdinfo.c
    ${NVTOP_DIR}/src/device_discovery_linux.c)
set_target_properties(evemon_gpu PROPERTIES AUTOMOC OFF)
foreach(backend nvidia ascend amdgpu amdgpu_utils intel intel_i915 intel_xe
    msm msm_utils panfrost panfrost_utils panthor panthor_utils mali_common
    v3d v3d_utils tpu metax)
    target_sources(evemon_gpu PRIVATE ${NVTOP_DIR}/src/extract_gpuinfo_${backend}.c)
endforeach()
target_sources(evemon_gpu PRIVATE ${NVTOP_DIR}/src/extract_npuinfo_rockchip.c)
target_sources(evemon_gpu PRIVATE ${NVTOP_DIR}/src/extract_gcuinfo_enflame.c)
target_include_directories(evemon_gpu PRIVATE ${NVTOP_DIR}/include ${NVTOP_DIR}/src)
target_compile_definitions(evemon_gpu PRIVATE EVEMON_NO_GTK _GNU_SOURCE USING_LIBUDEV)
target_compile_options(evemon_gpu PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(evemon_gpu PRIVATE PkgConfig::DRM PkgConfig::UDEV)
install(FILES ${NVTOP_DIR}/COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/evemon/licenses RENAME nvtop-COPYING)
