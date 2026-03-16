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

    find_package(OpenSSL REQUIRED)

    # ── picotls ──────────────────────────────────────────────────────────
    # Pass -DPICOTLS_SOURCE_DIR=/path/to/picotls to build from source,
    # otherwise falls back to find_library.
    if(DEFINED PICOTLS_SOURCE_DIR)
        add_library(instruments_picotls STATIC
            ${PICOTLS_SOURCE_DIR}/lib/picotls.c
            ${PICOTLS_SOURCE_DIR}/lib/hpke.c
            ${PICOTLS_SOURCE_DIR}/lib/pembase64.c
            ${PICOTLS_SOURCE_DIR}/lib/asn1.c
            ${PICOTLS_SOURCE_DIR}/lib/openssl.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra/x25519.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra/chacha20.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra/aes128.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra/aes256.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra/random.c
            ${PICOTLS_SOURCE_DIR}/lib/minicrypto-pem.c
            ${PICOTLS_SOURCE_DIR}/lib/uecc.c
            ${PICOTLS_SOURCE_DIR}/lib/ffx.c
            ${PICOTLS_SOURCE_DIR}/lib/micro-ecc.c
            ${PICOTLS_SOURCE_DIR}/lib/cifra-core-chacha20.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/aes.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/blockwise.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/chash.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/curve25519.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/drbg.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/hmac.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/gcm.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/gf128.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/modes.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/poly1305.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/sha256.c
            ${PICOTLS_SOURCE_DIR}/deps/cifra/src/sha512.c
        )
        target_include_directories(instruments_picotls
            PUBLIC
                ${PICOTLS_SOURCE_DIR}/include
            PRIVATE
                ${PICOTLS_SOURCE_DIR}/picotlsvs/picotls
                ${PICOTLS_SOURCE_DIR}/deps/cifra/src/ext
                ${PICOTLS_SOURCE_DIR}/deps/cifra/src
                ${PICOTLS_SOURCE_DIR}/deps/micro-ecc
                ${OPENSSL_INCLUDE_DIR}
        )
        target_link_libraries(instruments_picotls PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        if(WIN32)
            target_compile_definitions(instruments_picotls PRIVATE _WINDOWS)
        else()
            target_compile_definitions(instruments_picotls PRIVATE _GNU_SOURCE)
            target_compile_options(instruments_picotls PRIVATE -pthread)
        endif()
        if(NOT MSVC)
            target_compile_options(instruments_picotls PRIVATE
                -Wno-unused-parameter -Wno-missing-field-initializers
                -Wno-sign-compare -Wno-cast-function-type -Wno-incompatible-pointer-types
            )
        endif()
        set(PICOTLS_LIBRARIES instruments_picotls)
        set(PICOTLS_INCLUDE_DIRS ${PICOTLS_SOURCE_DIR}/include)
    else()
        find_path(PICOTLS_INCLUDE_DIRS picotls.h
            PATHS
                ${CMAKE_SOURCE_DIR}/../picotls/include
                ${CMAKE_SOURCE_DIR}/../../Externals/picotls/include
        )
        find_library(PICOTLS_LIBRARIES NAMES picotls)
    endif()

    # ── picoquic ─────────────────────────────────────────────────────────
    # Pass -DPICOQUIC_SOURCE_DIR=/path/to/picoquic to build from source.
    if(DEFINED PICOQUIC_SOURCE_DIR)
        add_library(instruments_picoquic STATIC
            ${PICOQUIC_SOURCE_DIR}/picoquic/bbr.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/bbr1.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/bytestream.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/c4.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/cc_common.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/config.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/cubic.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/ech.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/error_names.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/fastcc.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/frames.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/intformat.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/logger.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/logwriter.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/loss_recovery.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/newreno.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/pacing.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/packet.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/paths.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/performance_log.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/picohash.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/picoquic_lb.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/picoquic_ptls_openssl.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/picoquic_ptls_minicrypto.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/picosplay.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/port_blocking.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/prague.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/quicctx.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/register_all_cc_algorithms.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/sacks.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/sender.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/sim_link.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/siphash.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/spinbit.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/ticket_store.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/timing.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/token_store.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/tls_api.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/transport.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/unified_log.c
            ${PICOQUIC_SOURCE_DIR}/picoquic/util.c
        )
        target_include_directories(instruments_picoquic
            PUBLIC  ${PICOQUIC_SOURCE_DIR}/picoquic
            PRIVATE
                ${PICOTLS_INCLUDE_DIRS}
                ${PICOTLS_SOURCE_DIR}/deps/cifra/src
                ${PICOTLS_SOURCE_DIR}/deps/cifra/src/ext
                ${PICOTLS_SOURCE_DIR}/deps/micro-ecc
                ${OPENSSL_INCLUDE_DIR}
        )
        target_compile_definitions(instruments_picoquic PRIVATE
            PTLS_WITHOUT_FUSION
            DISABLE_DEBUG_PRINTF
        )
        if(WIN32)
            target_compile_definitions(instruments_picoquic PRIVATE _WINDOWS _WIN32_WINNT=0x0A00)
        else()
            target_compile_definitions(instruments_picoquic PRIVATE _GNU_SOURCE)
        endif()
        if(NOT MSVC)
            target_compile_options(instruments_picoquic PRIVATE
                -Wno-unused-parameter -Wno-missing-field-initializers
                -Wno-sign-compare -Wno-extra -Wno-type-limits -Wno-unknown-pragmas
            )
        endif()
        target_link_libraries(instruments_picoquic PUBLIC ${PICOTLS_LIBRARIES})
        set(PICOQUIC_LIBRARIES instruments_picoquic)
        set(PICOQUIC_INCLUDE_DIRS ${PICOQUIC_SOURCE_DIR}/picoquic)
    else()
        find_path(PICOQUIC_INCLUDE_DIRS picoquic.h
            PATHS
                ${CMAKE_SOURCE_DIR}/../picoquic/picoquic
                ${CMAKE_SOURCE_DIR}/../../Externals/picoquic/picoquic
        )
        find_library(PICOQUIC_LIBRARIES NAMES picoquic)
    endif()

    # ── lwIP ─────────────────────────────────────────────────────────────
    # Pass -DLWIP_SOURCE_DIR=/path/to/lwip to build from source.
    if(DEFINED LWIP_SOURCE_DIR)
        add_library(instruments_lwip STATIC
            ${LWIP_SOURCE_DIR}/src/core/init.c
            ${LWIP_SOURCE_DIR}/src/core/def.c
            ${LWIP_SOURCE_DIR}/src/core/dns.c
            ${LWIP_SOURCE_DIR}/src/core/inet_chksum.c
            ${LWIP_SOURCE_DIR}/src/core/ip.c
            ${LWIP_SOURCE_DIR}/src/core/mem.c
            ${LWIP_SOURCE_DIR}/src/core/memp.c
            ${LWIP_SOURCE_DIR}/src/core/netif.c
            ${LWIP_SOURCE_DIR}/src/core/pbuf.c
            ${LWIP_SOURCE_DIR}/src/core/raw.c
            ${LWIP_SOURCE_DIR}/src/core/stats.c
            ${LWIP_SOURCE_DIR}/src/core/sys.c
            ${LWIP_SOURCE_DIR}/src/core/altcp.c
            ${LWIP_SOURCE_DIR}/src/core/altcp_alloc.c
            ${LWIP_SOURCE_DIR}/src/core/altcp_tcp.c
            ${LWIP_SOURCE_DIR}/src/core/tcp.c
            ${LWIP_SOURCE_DIR}/src/core/tcp_in.c
            ${LWIP_SOURCE_DIR}/src/core/tcp_out.c
            ${LWIP_SOURCE_DIR}/src/core/timeouts.c
            ${LWIP_SOURCE_DIR}/src/core/udp.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/acd.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/autoip.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/dhcp.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/etharp.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/icmp.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/igmp.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/ip4.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/ip4_addr.c
            ${LWIP_SOURCE_DIR}/src/core/ipv4/ip4_frag.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/dhcp6.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/ethip6.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/icmp6.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/inet6.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/ip6.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/ip6_addr.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/ip6_frag.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/mld6.c
            ${LWIP_SOURCE_DIR}/src/core/ipv6/nd6.c
            ${LWIP_SOURCE_DIR}/src/api/err.c
            ${LWIP_SOURCE_DIR}/src/netif/ethernet.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/connection/sys_arch.c
        )
        target_include_directories(instruments_lwip
            PUBLIC  ${LWIP_SOURCE_DIR}/src/include
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/connection
        )
        if(NOT MSVC)
            target_compile_options(instruments_lwip PRIVATE
                -Wno-unused-parameter -Wno-missing-field-initializers
                -Wno-sign-compare -Wno-address -Wno-extra
            )
        endif()
        set(LWIP_LIBRARIES instruments_lwip)
        set(LWIP_INCLUDE_DIRS ${LWIP_SOURCE_DIR}/src/include)
    else()
        find_path(LWIP_INCLUDE_DIRS lwip/init.h
            PATHS
                ${CMAKE_SOURCE_DIR}/../lwip/src/include
                ${CMAKE_SOURCE_DIR}/../../Externals/lwip/src/include
        )
        find_library(LWIP_LIBRARIES NAMES lwip)
    endif()

    target_include_directories(instruments PRIVATE
        ${PICOQUIC_INCLUDE_DIRS}
        ${PICOTLS_INCLUDE_DIRS}
        ${LWIP_INCLUDE_DIRS}
    )
    target_link_libraries(instruments PRIVATE
        ${PICOQUIC_LIBRARIES}
        ${PICOTLS_LIBRARIES}
        ${LWIP_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
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
