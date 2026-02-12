# libinstruments

A standalone, pure C++20 library for communicating with iOS Instruments services. Supports iOS < 17 via USB/network, iOS 17+ via QUIC tunnel (picoquic + picotls + lwIP), and remote usbmux proxy connections (sonic-gidevice / go-ios).

## Features

- **Process Management** - List running processes, launch/kill apps
- **FPS Monitoring** - Real-time frames-per-second and GPU utilization via `graphics.opengl`
- **Performance Monitoring** - System and per-process CPU, memory, disk, network metrics via `sysmontap`
- **XCTest Runner** - Execute XCTest bundles with test result callbacks
- **WebDriverAgent** - Launch WDA with automatic port forwarding (HTTP + MJPEG)
- **Port Forwarding** - TCP relay between host and device
- **Remote Usbmux Proxy** - Connect via sonic-gidevice / go-ios shared port (`idevice_new_remote`)
- **iOS 17+ QUIC Tunnel** - Full tunnel support via picoquic + picotls + lwIP (no root needed)
- **Cross-Platform** - Windows, Linux, macOS

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

### CMake

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Options:
- `-DINSTRUMENTS_BUILD_TOOL=ON` (default) - Build the CLI tool
- `-DINSTRUMENTS_HAS_QUIC=ON` - Enable QUIC tunnel support (picoquic + picotls + lwIP)

### Premake5 (iDebugTool integration)

The library is built as a static library via `Prj/libinstruments.lua` when included in the iDebugTool workspace.

## Quick Start

### Library API

```cpp
#include <instruments/instruments.h>
using namespace instruments;

// Connect to a device by UDID
auto inst = Instruments::Create("00008030-001234567890ABCD");

// List processes
std::vector<ProcessInfo> procs;
inst->Process().GetProcessList(procs);
for (const auto& p : procs) {
    printf("PID: %lld  %s\n", (long long)p.pid, p.name.c_str());
}

// Monitor FPS
inst->FPS().Start(1000,
    [](const FPSData& data) {
        printf("FPS: %.0f  GPU: %.1f%%\n", data.fps, data.gpuUtilization);
    },
    [](Error e, const std::string& msg) {
        fprintf(stderr, "Error: %s\n", msg.c_str());
    }
);
// ... later
inst->FPS().Stop();

// Monitor performance
PerfConfig config;
config.sampleIntervalMs = 1000;
inst->Performance().Start(config,
    [](const SystemMetrics& m) {
        printf("CPU: %.1f%%\n", m.cpuTotalLoad);
    },
    [](const std::vector<ProcessMetrics>& procs) {
        for (const auto& p : procs) {
            printf("  PID %lld CPU: %.1f%%\n", (long long)p.pid, p.cpuUsage);
        }
    },
    nullptr
);
// ... later
inst->Performance().Stop();

// Launch an app
int64_t pid = 0;
inst->Process().LaunchApp("com.example.app", pid);

// Run WebDriverAgent
WDAConfig wdaConfig;
wdaConfig.bundleId = "com.facebook.WebDriverAgentRunner.xctrunner";
wdaConfig.wdaPort = 8100;
wdaConfig.mjpegPort = 9100;
inst->WDA().Start(wdaConfig,
    [](const std::string& log) { printf("[WDA] %s\n", log.c_str()); },
    nullptr
);
// WDA available at http://localhost:8100
// ... later
inst->WDA().Stop();
```

### Remote Usbmux Proxy (sonic-gidevice / go-ios shared port)

```cpp
// Connect via remote usbmux proxy (e.g., sonic-gidevice shared port)
auto inst = Instruments::CreateWithTunnel("192.168.1.100", 5555);
```

### iOS 17+ QUIC Tunnel

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

**Alternative**: Remote usbmux proxy tools (sonic-gidevice, go-ios) also work via `Instruments::CreateWithTunnel()`.

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

| iOS Version | Service Identifier |
|---|---|
| < 14 | `com.apple.instruments.remoteserver` |
| 14-16 | `com.apple.instruments.remoteserver.DVTSecureSocketProxy` |
| 17+ | `com.apple.instruments.dtservicehub` |

## License

Part of the iDebugTool project.
