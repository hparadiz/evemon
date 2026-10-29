cmake_minimum_required(VERSION 3.21)
project(evemon VERSION 0.2.0 LANGUAGES C)
include(GNUInstallDirs)
include(CheckIPOSupported)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
option(EVEMON_GTK "Build the GTK 3 frontend" ON)
option(EVEMON_QT "Build the Qt 6 frontend" OFF)
option(EVEMON_PIPEWIRE "Enable PipeWire audio inspection" ON)
option(EVEMON_MILKDROP "Enable MilkDrop visualization" ON)
option(EVEMON_GPU "Enable bundled nvtop GPU backends" ON)
option(EVEMON_EBPF "Enable eBPF tracing and build its kernel program" ON)
option(EVEMON_LTO "Enable link-time optimization" OFF)
set(EVEMON_ANDROID_ARCHIVE
    "${CMAKE_CURRENT_SOURCE_DIR}/data/android-arm64/evemon-arm64.tar.gz"
    CACHE FILEPATH "Prebuilt Android ARM64 Evemon runtime archive")
# Visualization assets/code never enter a headless-only build.
if(NOT EVEMON_GTK AND NOT EVEMON_QT)
    set(EVEMON_MILKDROP OFF)
endif()
set(EVEMON_DEFAULT_UI "" CACHE STRING "Default frontend: gtk, qt, or headless")
if(NOT EVEMON_DEFAULT_UI)
    if(EVEMON_GTK)
        set(EVEMON_DEFAULT_UI gtk)
    elseif(EVEMON_QT)
        set(EVEMON_DEFAULT_UI qt)
    else()
        set(EVEMON_DEFAULT_UI headless)
    endif()
endif()
if((EVEMON_DEFAULT_UI STREQUAL "gtk" AND NOT EVEMON_GTK) OR
   (EVEMON_DEFAULT_UI STREQUAL "qt" AND NOT EVEMON_QT) OR
   NOT EVEMON_DEFAULT_UI MATCHES "^(gtk|qt|headless)$")
    message(FATAL_ERROR "Default UI must be an enabled frontend or headless")
endif()
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(BPF REQUIRED IMPORTED_TARGET libbpf libelf)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
add_compile_definitions(_GNU_SOURCE EVEMON_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/evemon"
    EVEMON_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/evemon")
add_compile_options(-Wall -Wextra -Wformat-security -fstack-protector-strong)
add_link_options(-Wl,-z,relro,-z,now)
if(EVEMON_LTO)
    check_ipo_supported(RESULT IPO_SUPPORTED)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()
add_library(evemon_core STATIC src/core/log.c src/core/monitor.c src/core/steam.c src/core/profile.c
    src/platform/fdmon.c src/platform/fdmon_ebpf.c src/plugins/event_bus.c src/plugins/plugin_broker.c
    src/plugins/plugin_loader.c src/core/settings.c src/media/mpris.c src/core/format.c src/app/runtime.c
    src/ui/devices.c src/platform/desktop_session.c src/platform/compatibility.c
    src/ipc/ipc_wire.c src/ipc/query_client.c src/ipc/query_server.c src/ipc/query_snapshot.c src/ipc/query_inspect.c
    src/ipc/query_events.c src/ipc/query_fdmon.c src/ipc/query_gpu.c src/ipc/query_host.c src/ipc/query_about.c
    src/app/client_session.c src/remote/ssh_tunnel.c src/remote/adb_tunnel.c)
target_compile_definitions(evemon_core PRIVATE EVEMON_NO_GTK
    EVEMON_HAVE_GTK=$<BOOL:${EVEMON_GTK}> EVEMON_HAVE_QT=$<BOOL:${EVEMON_QT}>
    EVEMON_WITH_GPU=$<BOOL:${EVEMON_GPU}> EVEMON_WITH_PIPEWIRE=$<BOOL:${EVEMON_PIPEWIRE}>
    EVEMON_WITH_EBPF=$<BOOL:${EVEMON_EBPF}>
    EVEMON_WITH_MILKDROP=$<BOOL:${EVEMON_MILKDROP}>
    EVEMON_DEFAULT_UI="${EVEMON_DEFAULT_UI}")
target_compile_definitions(evemon_core PUBLIC EVEMON_VERSION="${PROJECT_VERSION}")
target_include_directories(evemon_core PUBLIC src)
target_link_libraries(evemon_core PUBLIC Threads::Threads PkgConfig::GLIB
    PkgConfig::BPF PkgConfig::JANSSON ${CMAKE_DL_LIBS} m)
if(EVEMON_PIPEWIRE)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_sources(evemon_core PRIVATE src/ui/pipewire_scan.c)
    target_compile_definitions(evemon_core PUBLIC HAVE_PIPEWIRE)
    target_link_libraries(evemon_core PUBLIC PkgConfig::PIPEWIRE)
endif()
# Mirror the installed data layout beside the development executable.
if(EVEMON_GTK OR EVEMON_QT)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/share/evemon")
    file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/data/software-directory.sqlite"
        "${CMAKE_CURRENT_BINARY_DIR}/share/evemon/software-directory.sqlite" SYMBOLIC)
    if(EVEMON_MILKDROP)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/share/evemon/milkdrop")
        file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/presets"
            "${CMAKE_CURRENT_BINARY_DIR}/share/evemon/milkdrop/presets" SYMBOLIC)
    endif()
    if(NOT EXISTS "${EVEMON_ANDROID_ARCHIVE}")
        message(FATAL_ERROR
            "Android ADB support requires ${EVEMON_ANDROID_ARCHIVE}")
    endif()
    file(SHA256 "${EVEMON_ANDROID_ARCHIVE}" EVEMON_ANDROID_SHA256)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${EVEMON_ANDROID_ARCHIVE}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/share/evemon/android")
    configure_file("${EVEMON_ANDROID_ARCHIVE}"
        "${CMAKE_CURRENT_BINARY_DIR}/share/evemon/android/evemon-arm64.tar.gz"
        COPYONLY)
    target_compile_definitions(evemon_core PRIVATE
        EVEMON_ANDROID_SHA256="${EVEMON_ANDROID_SHA256}")
endif()
add_executable(evemon src/app/main.c)
target_compile_definitions(evemon PRIVATE EVEMON_NO_GTK)
target_link_libraries(evemon PRIVATE -Wl,--whole-archive evemon_core -Wl,--no-whole-archive)
# UI modules resolve the core API from the executable, sharing its state.
set_target_properties(evemon PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(evemon INTERFACE src)
target_compile_definitions(evemon INTERFACE
    $<TARGET_PROPERTY:evemon_core,INTERFACE_COMPILE_DEFINITIONS>)
target_link_libraries(evemon INTERFACE PkgConfig::GLIB)
if(EVEMON_PIPEWIRE)
    target_link_libraries(evemon INTERFACE PkgConfig::PIPEWIRE)
endif()
if(EVEMON_GTK OR EVEMON_QT)
    pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pangocairo gdk-pixbuf-2.0)
    pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
    if(EVEMON_PIPEWIRE)
        pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
    endif()
    if(EVEMON_MILKDROP)
        if(NOT EVEMON_PIPEWIRE)
            message(FATAL_ERROR "MilkDrop requires PipeWire; disable it with --without-milkdrop")
        endif()
        pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
    endif()
endif()
if(EVEMON_GPU)
    include(cmake/Gpu.cmake)
    target_link_libraries(evemon_core PRIVATE evemon_gpu PkgConfig::DRM PkgConfig::UDEV)
    target_compile_definitions(evemon_core PUBLIC HAVE_NVTOP)
endif()
set(EVEMON_MILKDROP_SOURCES src/plugins/milkdrop_expression.c
    src/plugins/milkdrop_presets.c src/plugins/milkdrop_render.c
    src/plugins/milkdrop_controls.c src/plugins/milkdrop_overlay.c)
if(EVEMON_GTK)
    include(cmake/Gtk.cmake)
endif()
if(EVEMON_QT)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(cmake/Qt.cmake)
endif()
if(EVEMON_EBPF)
    include(cmake/Bpf.cmake)
endif()
install(TARGETS evemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(CODE "
    file(GLOB obsolete_core \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_LIBDIR}/evemon/libevemon_core.so*\")
    if(obsolete_core)
        file(REMOVE \${obsolete_core})
    endif()
")
# Install service definitions as examples for either init system. Selecting or
# enabling a system/user service remains an administrator/user operation.
configure_file(packaging/evemon.service packaging/systemd/system/evemon.service @ONLY)
configure_file(packaging/evemon-user.service packaging/systemd/user/evemon.service @ONLY)
configure_file(packaging/evemon.initd packaging/openrc/evemon @ONLY)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/packaging/systemd/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/evemon/services/systemd)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/packaging/openrc/evemon
    DESTINATION ${CMAKE_INSTALL_DATADIR}/evemon/services/openrc)
if(EVEMON_GTK OR EVEMON_QT)
    install(FILES evemon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME evemon.png)
    install(FILES data/software-directory.sqlite DESTINATION ${CMAKE_INSTALL_DATADIR}/evemon)
    install(FILES "${EVEMON_ANDROID_ARCHIVE}"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/evemon/android
        RENAME evemon-arm64.tar.gz)
endif()
if(EVEMON_MILKDROP AND (EVEMON_GTK OR EVEMON_QT))
    install(DIRECTORY presets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/evemon/milkdrop/presets FILES_MATCHING PATTERN "*.milk")
endif()
set(CPACK_PACKAGE_NAME evemon)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Evemon contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux process monitor with GTK, Qt and headless interfaces")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_GENERATOR TGZ)
include(CPack)
message(STATUS "Evemon: GTK=${EVEMON_GTK}, Qt=${EVEMON_QT}, default=${EVEMON_DEFAULT_UI}; headless always available")
