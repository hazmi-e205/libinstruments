# libinstruments

A standalone, pure C++20 library for communicating with iOS Instruments services. Supports iOS 12-16 via USB/network and iOS 17+ via USB RSD, USB-NCM (iOS 18+/26+), or external CoreDevice tunnel.

**Status**: ✅ DTX protocol working - process listing, FPS monitoring, and performance monitoring tested on **iOS 12 and iOS 15** via USB (Feb 2026). iOS 18+/26+ (iOS 26.2) connection investigated (Mar 2026) — see iOS 18+/26+ section below.

## Features

### ✅ Tested and Working (iOS 12 & iOS 15)
- **Process Listing** - Get running processes (tested on iOS 12 & iOS 15 via USB)
- **FPS Monitoring** - Real-time frames-per-second and GPU utilization via `graphics.opengl` (tested on iOS 12 & iOS 15 via USB)
- **Performance Monitoring** - System and per-process CPU, memory, disk, network metrics via `sysmontap` (tested on iOS 12 & iOS 15 via USB)
  - Supports multiple iOS data formats: dict-based (Processes key), nested dict (System.processes/ProcessByPid), and array-packed layouts
  - Handles messages on both dedicated channel and global channel (-1)
- **DTX Protocol** - Handshake, message exchange, channel management, global message routing
- **iOS Version Detection** - Automatic protocol selection based on iOS version (12-13: Legacy, 14-16: Modern, 17+: RSD)
- **SSL Mode Handling** - Version-specific SSL behavior (pre-14: handshake-only, 14-16: full SSL, 17+: no SSL)
- **Cross-Platform** - Windows, Linux, macOS

### 🔄 Implemented But Not Yet Tested
- **Process Launch/Kill** - Start and terminate processes
- **Port Forwarding** - TCP relay between host and device
- **XCTest Runner** - Execute XCTest bundles with test result callbacks
- **WebDriverAgent** - Launch WDA with automatic port forwarding (HTTP + MJPEG)
- **iOS 17+ USB RSD** - Direct USB connection to RSD port 58783 via usbmuxd — auto-detected; confirmed **CONNREFUSED on iOS 18+/26+** (port 58783 not accessible without CoreDevice tunnel)
- **iOS 18+/26+ USB-NCM auto-connect** - 3-phase fallback: (1) Apple usbmuxd PREFER_NETWORK, (2) Direct NCM IPv6 TCP via host NIC adapter enumeration + NDP. Both require **Apple Devices app** (Microsoft Store) to install the Apple Mobile Device NCM driver. Confirmed not available without it on iOS 26.2 (Mar 2026).
- **iOS 17+ QUIC Tunnel** - Wi-Fi tunnel via picoquic + picotls + lwIP for wireless devices (no root needed)

## Dependencies

| Library | Purpose | Required |
|---|---|---|
| libimobiledevice | Device communication (`idevice_t`, lockdown) | Yes |
| libplist | Plist encode/decode (NSKeyedArchiver) | Yes |
| libusbmuxd | USB multiplexing, port forwarding | Yes |
| libimobiledevice-glue | Thread/socket helpers | Yes |
| picoquic | QUIC protocol (RFC 9000) with datagram extension | Optional (iOS 17+) |
| picotls | TLS 1.3 backend for picoquic (OpenSSL 1.1.x) | Optional (iOS 17+) |
| lwIP | Userspace TCP/IP stack (NO_SYS mode) | Optional (iOS 17+) |

## Building

### Prerequisites

Required dependencies (all available as git submodules in iDebugTool's `Externals/` directory):
- **libimobiledevice** - Device communication
- **libplist** - Plist encode/decode
- **libusbmuxd** - USB multiplexing
- **libimobiledevice-glue** - Utility helpers

Optional dependencies for iOS 17+ QUIC tunnel (if building with `INSTRUMENTS_HAS_QUIC`):
- **picoquic** - QUIC protocol implementation
- **picotls** - TLS 1.3 backend (works with OpenSSL 1.1.x)
- **lwIP** - Userspace TCP/IP stack
- **OpenSSL 1.1.x** - Cryptography backend

### CMake (Standalone Build)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

CMake Options:
- `-DINSTRUMENTS_BUILD_TOOL=ON` (default) - Build the CLI tool (`instruments-cli`)
- `-DINSTRUMENTS_HAS_QUIC=ON` - Enable QUIC tunnel support for iOS 17+ (requires picoquic + picotls + lwIP)

### Premake5 (iDebugTool Integration)

The library is built as a static library via `Prj/libinstruments.lua` when included in the iDebugTool workspace.

```bash
# From iDebugTool root directory
cd Prj
premake5 vs2022  # or vs2019, gmake2, etc.
```

The Premake build system automatically:
- Links all required dependencies from `Externals/`
- Defines `INSTRUMENTS_HAS_QUIC` when picoquic/picotls/lwIP are available
- Configures include paths and library links

## Quick Start

### Library API Examples

#### Basic Device Connection

```cpp
#include <instruments/instruments.h>
using namespace instruments;

// Connect to a device by UDID (auto-detects iOS version and protocol)
auto inst = Instruments::Create("00008030-001234567890ABCD");
if (!inst) {
    fprintf(stderr, "Failed to connect to device\n");
    return 1;
}

// Get device info
DeviceInfo info = inst->GetDeviceInfo();
printf("Device: %s\n", info.name.c_str());
printf("iOS: %s\n", info.version.c_str());
```

#### Process Management

```cpp
// List all running processes (✅ Tested on iOS 15 via USB)
std::vector<ProcessInfo> procs;
Error err = inst->Process().GetProcessList(procs);
if (err == Error::Success) {
    for (const auto& p : procs) {
        printf("PID: %lld  %s  %s\n",
               (long long)p.pid, p.name.c_str(), p.bundleId.c_str());
    }
}

// Launch an app by bundle ID (🔄 Not yet tested)
int64_t pid = 0;
err = inst->Process().LaunchApp("com.example.MyApp", pid);
if (err == Error::Success) {
    printf("Launched app with PID: %lld\n", (long long)pid);
}

// Kill a process (🔄 Not yet tested)
err = inst->Process().KillProcess(pid);
```

#### FPS Monitoring (✅ Tested on iOS 15 via USB)

```cpp
// Start FPS monitoring (1000ms interval)
inst->FPS().Start(1000,
    [](const FPSData& data) {
        printf("FPS: %.1f  GPU: %.1f%%\n", data.fps, data.gpuUtilization);
    },
    [](Error e, const std::string& msg) {
        fprintf(stderr, "FPS Error: %s\n", msg.c_str());
    }
);

// ... monitor runs in background ...

// Stop when done
inst->FPS().Stop();
```

#### Performance Monitoring (✅ Tested on iOS 15 via USB)

```cpp
// Configure performance monitoring
PerfConfig config;
config.sampleIntervalMs = 1000;  // Sample every 1 second
// System and process attributes will be auto-populated if not specified
// Alternatively, you can specify custom attributes:
// config.systemAttributes = {"cpu_total_load", "memUsed", "diskBytesRead"};
// config.processAttributes = {"pid", "name", "cpuUsage", "physFootprint"};

// Start monitoring
inst->Performance().Start(config,
    // System metrics callback
    [](const SystemMetrics& sys) {
        printf("System CPU: %.1f%%  Memory: %llu MB\n",
               sys.cpuTotalLoad, sys.memUsed / 1024 / 1024);
    },
    // Process metrics callback
    [](const std::vector<ProcessMetrics>& procs) {
        for (const auto& p : procs) {
            if (p.cpuUsage > 5.0) {  // Show processes using > 5% CPU
                printf("  PID %lld (%s): CPU %.1f%%  Mem %llu MB\n",
                       (long long)p.pid, p.name.c_str(),
                       p.cpuUsage, p.memResident / 1024 / 1024);
            }
        }
    },
    // Error callback
    [](Error e, const std::string& msg) {
        fprintf(stderr, "Perf Error: %s\n", msg.c_str());
    }
);

// ... monitoring runs in background ...

// Stop when done
inst->Performance().Stop();
```

#### Port Forwarding (🔄 Not Yet Tested)

```cpp
// Forward local port 8080 to device port 80
err = inst->Ports().Forward(8080, 80);
if (err == Error::Success) {
    printf("Port forwarding active: localhost:8080 -> device:80\n");
}

// ... use the forwarded port ...

// Stop forwarding
inst->Ports().StopForward(8080);
```

#### WebDriverAgent (🔄 Not Yet Tested)

```cpp
WDAConfig wdaConfig;
wdaConfig.bundleId = "com.facebook.WebDriverAgentRunner.xctrunner";
wdaConfig.wdaPort = 8100;
wdaConfig.mjpegPort = 9100;

inst->WDA().Start(wdaConfig,
    [](const std::string& log) { printf("[WDA] %s\n", log.c_str()); },
    [](Error e, const std::string& msg) { fprintf(stderr, "WDA Error: %s\n", msg.c_str()); }
);

// WDA available at http://localhost:8100
// MJPEG stream at http://localhost:9100

// ... later
inst->WDA().Stop();
```

#### Error Handling

```cpp
Error err = inst->Process().LaunchApp("com.example.app", pid);
if (err != Error::Success) {
    fprintf(stderr, "Failed to launch app: %s\n", ErrorToString(err));
    // Handle error...
}
```

### iOS 17+, iOS 18+/26+ Support

iOS 17+ uses RSD (Remote Service Discovery) over HTTP/2 + XPC. Connection strategy depends on iOS version and installed software.

#### iOS 17.x — USB RSD (Automatic)

For USB-connected iOS 17.x devices, `idevice_connect()` to port 58783 works via usbmuxd. Auto-detected by all `FromUDID/FromDevice` factory methods.

```cpp
// Transparently handles iOS 17.x USB via RSD:
auto inst = Instruments::Create("DEVICE-UDID");
inst->Process().GetProcessList(procs);
```

#### iOS 18+/26+ — Requires Apple Devices App or External Tunnel

iOS 18+ (reported as iOS 26+ with Apple's 2025 versioning) moved CoreDevice out of lockdownd. Port 58783 returns `CONNREFUSED` via plain usbmuxd TCP forwarding. Three automatic paths are attempted in order:

**Phase 1: Apple usbmuxd PREFER_NETWORK** (`TryNetworkRSD`)
Works when "Apple Devices" app (Microsoft Store / Windows) has activated the USB-NCM tunnel. Apple's usbmuxd registers iOS 18+ devices with their USB-NCM IPv6 address, making `IDEVICE_LOOKUP_PREFER_NETWORK` return a direct TCP device.

**Phase 2: Direct USB-NCM IPv6 TCP** (`TryDirectNCMConnection`)
Enumerates host network adapters for Apple USB-NCM interface, gets device link-local IPv6 from NDP neighbor table, connects directly via TCP. No admin, no libusb — same approach as go-ios. **Requires Apple Devices app** to install the Apple Mobile Device NCM driver.

> **Windows prerequisite**: Install [Apple Devices](https://apps.microsoft.com/detail/9NP83LWLPZ9K) from the Microsoft Store. This installs the Apple Mobile Device NCM driver that creates the USB-NCM virtual Ethernet adapter. Without it, no USB-NCM adapter exists and both Phase 1 and Phase 2 fail.

**Phase 3 fallback**: If both automatic paths fail, the library logs instructions to use an external CoreDevice tunnel:

```bash
# go-ios:
ios tunnel start --udid=<UDID>
# outputs: Tunnel started. Address: fd61:a1b2::1 Port: 58783

# pymobiledevice3:
python3 -m pymobiledevice3 remote start-tunnel
```

Then connect via tunnel address:
```cpp
auto inst = Instruments::CreateFromTunnel("fd61:a1b2::1", 58783, "26.2");
inst->Process().GetProcessList(procs);
```

Or using the CLI tool:
```bash
instruments-cli process list --address fd61:a1b2::1 --rsd-port 58783
```

### CLI Tool

```bash
# List processes (USB, iOS 12-17.x)
instruments-cli process list --udid <UDID>

# List processes via external tunnel (iOS 17+, including iOS 18+/26+)
instruments-cli process list --address <IPv6> --rsd-port 58783

# Launch an app
instruments-cli process launch --udid <UDID> --bundle com.example.app
instruments-cli process launch --address <IPv6> --rsd-port 58783 --bundle com.example.app

# Kill a process
instruments-cli process kill --udid <UDID> --pid 1234

# Monitor FPS
instruments-cli fps --udid <UDID> --interval 1000
instruments-cli fps --address <IPv6> --rsd-port 58783 --interval 1000

# Monitor performance
instruments-cli perf --udid <UDID> --interval 1000
instruments-cli perf --address <IPv6> --rsd-port 58783 --interval 1000

# Run XCTest
instruments-cli xctest --udid <UDID> --bundle com.example.app --runner com.example.appUITests.xctrunner

# Run WebDriverAgent
instruments-cli wda --udid <UDID> --bundle com.facebook.WebDriverAgentRunner.xctrunner

# Port forwarding
instruments-cli forward --udid <UDID> --host-port 8080 --device-port 80

# Tunnel management
instruments-cli tunnel list
instruments-cli tunnel start --udid <UDID>

# Debug logging
instruments-cli process list --udid <UDID> --verbose
```

## Architecture

```
include/instruments/     Public API headers
src/
├── nskeyedarchiver/     Self-contained NSKeyedArchiver (encode/decode via libplist)
├── dtx/                 DTX binary protocol (message, channel, connection, transport)
├── connection/          Device connection abstraction (USB, tunnel, RSD)
├── services/            High-level instrument services
└── util/                Logging, LZ4 decompression
tool/                    CLI tool
```

### Protocol Stack

```
┌─────────────────────────────────────────────┐
│  Services (Process, FPS, Perf, XCTest, WDA) │
├─────────────────────────────────────────────┤
│  DTX Channel (sync/async messaging)         │
├─────────────────────────────────────────────┤
│  DTX Connection (channel mgmt, recv loop)   │
├─────────────────────────────────────────────┤
│  DTX Transport (raw send/recv)              │
├─────────────────────────────────────────────┤
│  DeviceConnection (USB / Tunnel / RSD)      │
├─────────────────────────────────────────────┤
│  libimobiledevice (idevice_t)               │
└─────────────────────────────────────────────┘
```

### Threading Model

- One background receive thread per DTXConnection
- Sync calls use `std::condition_variable` for response correlation
- Monitoring callbacks fire on the receive thread
- Port forwarder: one acceptor thread + relay thread pair per connection
- All stop mechanisms use `std::atomic<bool>` flags

## Protocol Reference

### DTX Message Format

```
Header (32 bytes, little-endian):
  [0-3]   Magic: 0x1F3D5B79 (LE) = 0x795B3D1F
  [4-7]   Header length: 32
  [8-11]  Fragment index
  [12-15] Fragment count
  [16-19] Total message length
  [20-23] Message identifier
  [24-27] Conversation index
  [28-29] Channel code
  [30]    Expects reply flag
  [31]    Reserved

Payload Header (16 bytes):
  [0-3]   Flags (0x02 = has payload, 0x01 = has auxiliary)
  [4-7]   Auxiliary length
  [8-15]  Total payload length

Auxiliary: PrimitiveDictionary encoding of method arguments
Payload: NSKeyedArchiver-encoded selector or return value
```

### Service Names

| iOS Version | Protocol | Service Identifier | SSL Mode |
|---|---|---|---|
| 12-13 | Legacy | `com.apple.instruments.remoteserver` | Handshake-only (auth then plaintext DTX) |
| 14-16 | Modern | `com.apple.instruments.remoteserver.DVTSecureSocketProxy` | Full SSL encryption |
| 17+ | RSD | `com.apple.instruments.dtservicehub` | No SSL (USB usbmuxd or QUIC tunnel provides transport security) |

**How it works:**
1. Library detects iOS version via lockdown (`ProductVersion` key)
2. Selects appropriate protocol: `IOSProtocol::Legacy` (iOS <14), `Modern` (14-16), or `RSD` (17+)
3. Chooses matching service name and SSL behavior
4. Creates DTX connection with correct SSL mode (handshake-only for Legacy, full for Modern, none for RSD)

## API Reference

All public headers are in `include/instruments/`:

| Header | Description |
|---|---|
| `instruments.h` | Main facade class (single include for everything) |
| `types.h` | Common types, enums, error codes |
| `device_connection.h` | Low-level device connection management |
| `tunnel_manager.h` | QUIC tunnel management (iOS 17+) |
| `dtx_connection.h` | DTX protocol connection layer |
| `dtx_channel.h` | DTX channel abstraction |
| `dtx_message.h` | DTX message construction |
| `process_service.h` | Process management API |
| `performance_service.h` | Performance monitoring API |
| `fps_service.h` | FPS monitoring API |
| `xctest_service.h` | XCTest execution API |
| `wda_service.h` | WebDriverAgent API |
| `port_forwarder.h` | Port forwarding API |

## Contributing

When modifying this library:
1. Read `CLAUDE.md` for developer guidelines and protocol details
2. Maintain C++20 compatibility (no Qt dependencies)
3. Test on Windows and Linux
4. Update both README.md and CLAUDE.md with any protocol or API changes
5. Follow the code style: PascalCase methods, m_ prefix for members

## References

- **[pymobiledevice3](https://github.com/doronz88/pymobiledevice3)** - Python reference for DTX protocol and iOS communication
- **[go-ios](https://github.com/danielpaulus/go-ios)** - Go reference implementation (primary DTX reference)
- **[sonic-gidevice](https://github.com/SonicCloudOrg/sonic-gidevice)** - Go implementation with remote proxy support
- **[iosif](https://github.com/dryark/iosif)** - C implementation reference for DTX protocol details (bytearr.c, dtxpayload__new)

## License

GNU Affero General Public License v3.0 (AGPL-3.0)

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See the [LICENSE](LICENSE) file for full details.
