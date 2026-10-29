find_program(CLANG_EXECUTABLE NAMES clang REQUIRED)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|i.86)$")
    set(BPF_ARCH x86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(BPF_ARCH arm64)
else()
    message(FATAL_ERROR "Set a supported BPF target architecture for ${CMAKE_SYSTEM_PROCESSOR}")
endif()
# Debian-family toolchains place asm/ headers under the multiarch directory.
set(BPF_SYSTEM_INCLUDES)
if(CMAKE_SYSROOT)
    # BPF uses a host clang, but its kernel/register headers belong to the
    # target architecture. Mixing x86 headers with an ARM target cannot work.
    list(APPEND BPF_SYSTEM_INCLUDES "-I${CMAKE_SYSROOT}/usr/include")
endif()
if(CMAKE_LIBRARY_ARCHITECTURE AND EXISTS "${CMAKE_SYSROOT}/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}/asm/types.h")
    list(APPEND BPF_SYSTEM_INCLUDES "-I${CMAKE_SYSROOT}/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}")
endif()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fdmon_ebpf_kern.o
    COMMAND ${CLANG_EXECUTABLE} -O2 -g -target bpf -D__TARGET_ARCH_${BPF_ARCH} ${BPF_SYSTEM_INCLUDES}
        -c ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/fdmon_ebpf_kern.c
        -o ${CMAKE_CURRENT_BINARY_DIR}/fdmon_ebpf_kern.o
    DEPENDS src/platform/fdmon_ebpf_kern.c VERBATIM)
add_custom_target(evemon_bpf ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fdmon_ebpf_kern.o)
add_dependencies(evemon evemon_bpf)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fdmon_ebpf_kern.o DESTINATION ${CMAKE_INSTALL_LIBDIR}/evemon)
