cmake_minimum_required(VERSION 3.16)
project(libinstruments VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(INSTRUMENTS_BUILD_TOOL "Build the CLI tool" ON)
option(INSTRUMENTS_HAS_QUIC "Enable QUIC tunnel support (requires picoquic + picotls + lwIP)" OFF)

# Find dependencies
find_package(PkgConfig QUIET)

# libplist
if(PkgConfig_FOUND)
    pkg_check_modules(PLIST IMPORTED_TARGET libplist-2.0)
endif()
if(NOT PLIST_FOUND)
    # Fallback: look for headers/libs directly
    find_path(PLIST_INCLUDE_DIRS plist/plist.h
        PATHS
            ${CMAKE_SOURCE_DIR}/../libplist/include
            ${CMAKE_SOURCE_DIR}/../../Externals/libplist/include
            /usr/include /usr/local/include
    )
    find_library(PLIST_LIBRARIES NAMES plist plist-2.0
        PATHS
            ${CMAKE_SOURCE_DIR}/../libplist/src/.libs
            /usr/lib /usr/local/lib
    )
endif()

# libimobiledevice
if(PkgConfig_FOUND)
    pkg_check_modules(IMOBILEDEVICE IMPORTED_TARGET libimobiledevice-1.0)
endif()
if(NOT IMOBILEDEVICE_FOUND)
    find_path(IMOBILEDEVICE_INCLUDE_DIRS libimobiledevice/libimobiledevice.h
        PATHS
            ${CMAKE_SOURCE_DIR}/../libimobiledevice/include
            ${CMAKE_SOURCE_DIR}/../../Externals/libimobiledevice/include
            /usr/include /usr/local/include
    )
    find_library(IMOBILEDEVICE_LIBRARIES NAMES imobiledevice imobiledevice-1.0
        PATHS
            ${CMAKE_SOURCE_DIR}/../libimobiledevice/src/.libs
            /usr/lib /usr/local/lib
    )
endif()

# libusbmuxd
if(PkgConfig_FOUND)
    pkg_check_modules(USBMUXD IMPORTED_TARGET libusbmuxd-2.0)
endif()
if(NOT USBMUXD_FOUND)
    find_path(USBMUXD_INCLUDE_DIRS usbmuxd.h
        PATHS
            ${CMAKE_SOURCE_DIR}/../libusbmuxd/include
            ${CMAKE_SOURCE_DIR}/../../Externals/libusbmuxd/include
            /usr/include /usr/local/include
    )
endif()

# libimobiledevice-glue
if(PkgConfig_FOUND)
    pkg_check_modules(IMOBILEDEVICE_GLUE IMPORTED_TARGET libimobiledevice-glue-1.0)
endif()

# Library sources
set(LIB_SOURCES
    # Utilities
    src/util/log.cpp
    src/util/lz4.cpp

    # NSKeyedArchiver
    src/nskeyedarchiver/nsobject.cpp
    src/nskeyedarchiver/nskeyedarchiver.cpp
    src/nskeyedarchiver/nskeyedunarchiver.cpp

    # DTX protocol
    src/dtx/dtx_message.cpp
    src/dtx/dtx_primitive_dict.cpp
    src/dtx/dtx_fragment.cpp
    src/dtx/dtx_transport.cpp
    src/dtx/dtx_channel.cpp
    src/dtx/dtx_connection.cpp

    # Connection
    src/connection/device_connection.cpp
    src/connection/service_connector.cpp
    src/connection/xpc_message.cpp
    src/connection/http2_framer.cpp
    src/connection/rsd_provider.cpp
    src/connection/tunnel_quic.cpp
    src/connection/tunnel_userspace.cpp
    src/connection/userspace_network.cpp
    src/connection/tunnel_manager.cpp

    # Services
    src/services/process_service.cpp
    src/services/performance_service.cpp
    src/services/fps_service.cpp
    src/services/xctest_proxy.cpp
    src/services/xctest_service.cpp
    src/services/wda_service.cpp
    src/services/port_forwarder.cpp

    # Facade
    src/instruments.cpp
)

set(LIB_HEADERS
    include/instruments/instruments.h
    include/instruments/types.h
    include/instruments/device_connection.h
    include/instruments/tunnel_manager.h
    include/instruments/dtx_connection.h
    include/instruments/dtx_channel.h
    include/instruments/dtx_message.h
    include/instruments/process_service.h
    include/instruments/performance_service.h
    include/instruments/fps_service.h
    include/instruments/xctest_service.h
    include/instruments/wda_service.h
    include/instruments/port_forwarder.h
)

# Create library
add_library(instruments STATIC ${LIB_SOURCES} ${LIB_HEADERS})

target_include_directories(instruments
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PLIST_INCLUDE_DIRS}
        ${IMOBILEDEVICE_INCLUDE_DIRS}
        ${USBMUXD_INCLUDE_DIRS}
        ${IMOBILEDEVICE_GLUE_INCLUDE_DIRS}
)

target_compile_definitions(instruments PRIVATE
    INSTRUMENTS_STATIC
)

if(INSTRUMENTS_HAS_QUIC)
    target_compile_definitions(instruments PRIVATE INSTRUMENTS_HAS_QUIC)

    # picoquic
    find_path(PICOQUIC_INCLUDE_DIRS picoquic.h
        PATHS
            ${CMAKE_SOURCE_DIR}/../picoquic/picoquic
            ${CMAKE_SOURCE_DIR}/../../Externals/picoquic/picoquic
    )
    find_library(PICOQUIC_LIBRARIES NAMES picoquic)

    # picotls
    find_path(PICOTLS_INCLUDE_DIRS picotls.h
        PATHS
            ${CMAKE_SOURCE_DIR}/../picotls/include
            ${CMAKE_SOURCE_DIR}/../../Externals/picotls/include
    )
    find_library(PICOTLS_LIBRARIES NAMES picotls)

    # lwIP
    find_path(LWIP_INCLUDE_DIRS lwip/init.h
        PATHS
            ${CMAKE_SOURCE_DIR}/../lwip/src/include
            ${CMAKE_SOURCE_DIR}/../../Externals/lwip/src/include
    )
    find_library(LWIP_LIBRARIES NAMES lwip)

    target_include_directories(instruments PRIVATE
        ${PICOQUIC_INCLUDE_DIRS}
        ${PICOTLS_INCLUDE_DIRS}
        ${LWIP_INCLUDE_DIRS}
    )
    target_link_libraries(instruments PRIVATE
        ${PICOQUIC_LIBRARIES}
        ${PICOTLS_LIBRARIES}
        ${LWIP_LIBRARIES}
    )
endif()

# Link dependencies
target_link_libraries(instruments
    PUBLIC
        ${PLIST_LIBRARIES}
        ${IMOBILEDEVICE_LIBRARIES}
)

if(WIN32)
    target_link_libraries(instruments PUBLIC ws2_32 Iphlpapi)
endif()

# Platform-specific settings
if(MSVC)
    target_compile_options(instruments PRIVATE /W3)
else()
    target_compile_options(instruments PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    )
endif()

# Install
install(TARGETS instruments
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/instruments
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# CLI tool
if(INSTRUMENTS_BUILD_TOOL)
    add_executable(instruments-cli
        tool/main.cpp
    )

    target_link_libraries(instruments-cli PRIVATE instruments)

    target_include_directories(instruments-cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(WIN32)
        target_link_libraries(instruments-cli PRIVATE ws2_32)
    else()
        target_link_libraries(instruments-cli PRIVATE pthread)
    endif()

    install(TARGETS instruments-cli DESTINATION bin)
endif()
