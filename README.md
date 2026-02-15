# libinstruments

A standalone, pure C++20 library for communicating with iOS Instruments services. Supports iOS < 17 via USB/network, iOS 17+ via QUIC tunnel (picoquic + picotls + lwIP), and remote usbmux proxy connections (sonic-gidevice / go-ios).

**Status**: ✅ DTX protocol working - process listing, FPS monitoring, and performance monitoring tested on iOS 15 via USB. Designed for iOS 14-17+.

## Features

### ✅ Tested and Working (iOS 15)
- **Process Listing** - Get running processes (tested via USB and remote usbmux)
- **FPS Monitoring** - Real-time frames-per-second and GPU utilization via `graphics.opengl` (tested via USB and remote usbmux)
- **Performance Monitoring** - System and per-process CPU, memory, disk, network metrics via `sysmontap` (tested via USB on iOS 15)
  - Supports multiple iOS data formats: dict-based (Processes key), nested dict (System.processes/ProcessByPid), and array-packed layouts
  - Handles messages on both dedicated channel and global channel (-1)
- **DTX Protocol** - Handshake, message exchange, channel management, global message routing
- **Remote Usbmux Proxy** - Connect via sonic-gidevice / go-ios shared port (tested on iOS 15)
- **Cross-Platform** - Windows, Linux, macOS

### 🔄 Implemented But Not Yet Tested
- **Process Launch/Kill** - Start and terminate processes
- **Port Forwarding** - TCP relay between host and device
- **XCTest Runner** - Execute XCTest bundles with test result callbacks
- **WebDriverAgent** - Launch WDA with automatic port forwarding (HTTP + MJPEG)
- **iOS 17+ QUIC Tunnel** - Full tunnel support via picoquic + picotls + lwIP (no root needed)

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

### Remote Usbmux Proxy (✅ Tested on iOS 15)

Connect to iOS devices over the network via remote usbmux proxy servers like [sonic-gidevice](https://github.com/SonicCloudOrg/sonic-gidevice) or [go-ios](https://github.com/danielpaulus/go-ios).

**⚠️ Important**: This feature requires **patched libimobiledevice functions** that are NOT in the official repo:

```c
// Connect to remote usbmux proxy (e.g., sonic-gidevice at 192.168.1.100:5555)
idevice_error_t idevice_new_remote(idevice_t *device, const char *ip_address, uint16_t port);

// Perform lockdown handshake via the remote connection
lockdownd_error_t lockdownd_client_new_with_handshake_remote(idevice_t device,
                                                               lockdownd_client_t *client,
                                                               const char *label);
```

**In iDebugTool**: These functions are patched by the build script automatically during the build process. The patches are located at `Externals/_Patches/libimobiledevice.patch` and applied to the libimobiledevice source.

#### Example: Connect via sonic-gidevice

```cpp
// 1. Start sonic-gidevice on the iOS device host (e.g., macOS machine with USB-connected iPhone)
//    $ gidevice share --port 5555

// 2. Connect to remote usbmux proxy using patched libimobiledevice functions
idevice_t device = nullptr;
idevice_error_t err = idevice_new_remote(&device, "192.168.1.100", 5555);
if (err != IDEVICE_E_SUCCESS || !device) {
    fprintf(stderr, "Failed to connect to remote usbmux at 192.168.1.100:5555\n");
    return 1;
}

// 3. Create lockdown client with remote handshake
lockdownd_client_t lockdown = nullptr;
lockdownd_error_t lerr = lockdownd_client_new_with_handshake_remote(device, &lockdown, "my-app");
if (lerr != LOCKDOWN_E_SUCCESS || !lockdown) {
    fprintf(stderr, "Failed to create lockdown client\n");
    idevice_free(device);
    return 1;
}

// 4. Create Instruments instance (library does NOT take ownership)
auto inst = Instruments::Create(device, lockdown);
if (!inst) {
    fprintf(stderr, "Failed to create Instruments connection\n");
    lockdownd_client_free(lockdown);
    idevice_free(device);
    return 1;
}

// 5. Use normally - all features work transparently
std::vector<ProcessInfo> procs;
Error err2 = inst->Process().GetProcessList(procs);
if (err2 == Error::Success) {
    printf("Connected to remote device via %s\n", inst->GetDeviceInfo().name.c_str());
    for (const auto& p : procs) {
        printf("  PID: %lld  %s\n", (long long)p.pid, p.name.c_str());
    }
}

// FPS monitoring also works via remote proxy
inst->FPS().Start(1000, [](const FPSData& data) {
    printf("FPS: %.1f  GPU: %.1f%%\n", data.fps, data.gpuUtilization);
}, nullptr);

// 6. Cleanup (when done with Instruments instance)
lockdownd_client_free(lockdown);
idevice_free(device);
```

#### How It Works

1. **sonic-gidevice** runs on the machine with USB-connected iOS device
2. It exposes a TCP port (e.g., 5555) that speaks the usbmux protocol
3. `idevice_new_remote()` connects to this remote port instead of local usbmuxd socket
4. `lockdownd_client_new_with_handshake_remote()` performs lockdown handshake over the remote connection
5. Pass the device and lockdown handles to `Instruments::Create(device, lockdown)`
6. All DTX protocol communication happens transparently over the network
7. **This is NOT an RSD tunnel** - it's a remote usbmux proxy (works on iOS 14-16, possibly iOS 17+ too)

#### Notes

- Tested successfully on **iOS 15** with process listing and FPS monitoring
- Works with both **sonic-gidevice** (Go) and **go-ios** (Go) proxy servers
- Network latency may affect real-time monitoring performance
- Requires the patched libimobiledevice functions (automatically applied in iDebugTool builds)
- Can be used alongside local USB connections (library auto-detects connection type)

### iOS 17+ QUIC Tunnel (🔄 Not Yet Tested)

iOS 17+ requires a QUIC tunnel for instruments communication. Enable with `INSTRUMENTS_HAS_QUIC` build flag.

**Stack**: picoquic (QUIC + datagrams) → picotls (TLS 1.3) → OpenSSL 1.1.x, with lwIP as a userspace TCP/IP stack (no root/admin privileges required).

**Tunnel flow**:
1. QUIC handshake over UDP → stream 0 clientHandshakeRequest
2. Receives serverHandshakeResponse with IPv6 addresses + RSD port
3. lwIP initializes userspace TCP/IP with tunnel IPv6
4. QUIC datagrams forwarded bidirectionally as lwIP IPv6 packets
5. RSD provider connects via lwIP TCP → HTTP/2 + XPC handshake → service discovery

```cpp
// Start a QUIC tunnel to an iOS 17+ device
TunnelManager mgr;
mgr.StartTunnel("UDID");

// Or register an external tunnel (pymobiledevice3, ios tunnel start)
mgr.RegisterExternalTunnel("UDID", "fd75:a1b2::1", 60789);

auto tunnels = mgr.GetActiveTunnels();
for (const auto& t : tunnels) {
    printf("%s -> %s:%u\n", t.udid.c_str(), t.address.c_str(), t.rsdPort);
}
```

**Alternative**: Remote usbmux proxy tools (sonic-gidevice, go-ios) work for iOS 17+ devices by using `idevice_new_remote()` + `lockdownd_client_new_with_handshake_remote()` + `Instruments::Create(device, lockdown)`.

### CLI Tool

```bash
# List processes
instruments-cli process list --udid <UDID>

# Launch an app
instruments-cli process launch --udid <UDID> --bundle com.example.app

# Kill a process
instruments-cli process kill --udid <UDID> --pid 1234

# Monitor FPS
instruments-cli fps --udid <UDID> --interval 1000

# Monitor performance
instruments-cli perf --udid <UDID> --interval 1000

# Run XCTest
instruments-cli xctest --udid <UDID> --bundle com.example.app --runner com.example.appUITests.xctrunner

# Run WebDriverAgent
instruments-cli wda --udid <UDID> --bundle com.facebook.WebDriverAgentRunner.xctrunner

# Port forwarding
instruments-cli forward --udid <UDID> --host-port 8080 --device-port 80

# Tunnel management (iOS 17+)
instruments-cli tunnel list
instruments-cli tunnel start --udid <UDID>

# Using with external tunnel
instruments-cli process list --tunnel [fd75:a1b2::1]:60789

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

| iOS Version | Service Identifier | SSL Mode |
|---|---|---|
| < 14 | `com.apple.instruments.remoteserver` | Handshake-only (plaintext DTX) |
| 14-16 | `com.apple.instruments.remoteserver.DVTSecureSocketProxy` | Full SSL |
| 17+ | `com.apple.instruments.dtservicehub` | No service SSL (tunnel encrypts) |

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

## License

GNU Affero General Public License v3.0 (AGPL-3.0)

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See the [LICENSE](LICENSE) file for full details.
