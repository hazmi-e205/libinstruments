include "common.lua"

project "instruments"
    kind "StaticLib"
    QtConfigs {"force_debug_info", "c++20"}

    files
    {
        "../Externals/libinstruments/include/**.h",
        "../Externals/libinstruments/src/**.h",
        "../Externals/libinstruments/src/**.cpp",
    }

    excludes
    {
        "../Externals/libinstruments/tool/**",
    }

    includedirs
    {
        "../Externals/libinstruments/include",
        "../Externals/libinstruments/src",
        "../Externals/libplist/include",
        "../Externals/libimobiledevice/include",
        "../Externals/libusbmuxd/include",
        "../Externals/libimobiledevice-glue/include",
        "../Externals/picoquic/picoquic",
        "../Externals/picotls/include",
        "../Externals/lwip/src/include",
    }

    defines
    {
        "INSTRUMENTS_STATIC",
        "LIBIMOBILEDEVICE_STATIC",
        "LIBPLIST_STATIC",
        "LIBUSBMUXD_STATIC",
        "LIMD_GLUE_STATIC",
        "INSTRUMENTS_HAS_QUIC",
    }

    buildoptions
    {
        "-Wno-unused-parameter",
        "-Wno-missing-field-initializers",
    }

    filter "system:windows"
        links { "Ws2_32", "Iphlpapi" }

    filter "system:linux"
        links { "pthread" }
