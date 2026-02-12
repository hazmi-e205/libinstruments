# CLAUDE.md - libinstruments

## What is this?

`libinstruments` is a pure C++20 static library that implements Apple's Instruments (DTX) protocol for communicating with iOS devices. It lives at `Externals/libinstruments/` within the iDebugTool project and replaces the older `libnskeyedarchiver` + `libidevice` externals with a single, self-contained library.

## Code Style

- **PascalCase** for methods and classes (e.g., `GetProcessList`, `SendMessageSync`)
- **m_** prefix for member variables (e.g., `m_connection`, `m_running`)
- **C++20** standard (std::atomic, std::shared_ptr, std::function, structured bindings)
- No Qt dependency - pure STL
- No exceptions across public API boundaries - return `Error` enum
- RAII patterns for resource management
- Use include headers instead of forward declaration
- Make sure no compilation error on windows and unix/unix-like systems after your jobs done
- Make sure this library still inline with `README.md` and `CLAUDE.md`

## Directory Layout

```
include/instruments/    Public API headers (what consumers #include)
src/nskeyedarchiver/    NSKeyedArchiver encode/decode (internal, uses libplist)
src/dtx/                DTX binary protocol implementation (internal)
src/connection/         Device connection, tunneling, RSD (internal)
src/services/           High-level services (Process, FPS, Perf, XCTest, WDA)
src/util/               Logging, LZ4 decompression
tool/                   CLI tool (instruments-cli)
Prj/                    Premake5 build script for iDebugTool integration
```

## Build Systems

- **CMake**: `CMakeLists.txt` at root - standalone builds
- **Premake5**: `Prj/libinstruments.lua` - iDebugTool workspace integration

## Key Dependencies

All located in sibling directories under `Externals/`:
- `libimobiledevice` - `idevice_t`, lockdown service connections
- `libplist` - Binary plist encode/decode (used by NSKeyedArchiver)
- `libusbmuxd` - USB multiplexing
- `libimobiledevice-glue` - Utility helpers

## DTX Protocol Essentials

- **Magic**: `0x795B3D1F` (little-endian in header: `0x1F 0x3D 0x5B 0x79`)
- **Header**: 32 bytes (LE), payload header: 16 bytes
- **Message types**: Ack=0x0, MethodInvocation=0x2, ResponseWithPayload=0x3, Error=0x4, LZ4Compressed=0x0707
- **PrimitiveDictionary types**: null=0x0A, string=0x01, bytearray=0x02, uint32=0x03, int64=0x06
- **Fragments**: Fragment 0 is header-only when count > 1, subsequent fragments carry data
- **SSL**: instruments.remoteserver and testmanagerd.lockdown use handshake-only SSL (handshake then plaintext)

## Service Name Selection

```
iOS < 14:  com.apple.instruments.remoteserver
iOS 14-16: com.apple.instruments.remoteserver.DVTSecureSocketProxy
iOS 17+:   com.apple.instruments.dtservicehub
```

## Channel Identifiers

```
deviceinfo:    com.apple.instruments.server.services.deviceinfo
processcontrol: com.apple.instruments.server.services.processcontrol
sysmontap:     com.apple.instruments.server.services.sysmontap
graphics.opengl: com.apple.instruments.server.services.graphics.opengl
testmanagerd:  com.apple.instruments.server.services.testmanagerd
```

## Threading Model

- One background thread per `DTXConnection` for receiving messages
- Sync calls block with `std::condition_variable` (response matched by ConversationIndex)
- Monitoring callbacks (FPS, Perf) fire on the receive thread
- Port forwarder runs acceptor + relay threads
- Stop via `std::atomic<bool>` flags

## iOS 17+ QUIC Tunnel + RSD

### Architecture

```
Services (Process, FPS, Perf...)
    ↕
DTX Connection (existing)
    ↕
DeviceConnection
    ↕
RSD Provider (HTTP/2 + XPC handshake, service discovery)
    ↕
UserspaceNetwork (lwIP TCP/IP stack, no root needed)
    ↕
QUICTunnel (picoquic QUIC + datagram forwarding)
    ↕
picoquic → picotls → OpenSSL 1.1.x
```

### Stack: picoquic + picotls + lwIP

- **picoquic**: QUIC protocol (RFC 9000) with datagram extension (RFC 9221)
- **picotls**: TLS 1.3 using OpenSSL 1.1.x backend (no OpenSSL 3.x required)
- **lwIP**: Userspace TCP/IP stack in NO_SYS mode (bare-metal, manually polled)
- No root/admin privileges required - all networking in userspace

### Build flag: `INSTRUMENTS_HAS_QUIC`

- Premake5: defined in `Prj/libinstruments.lua`
- CMake: `-DINSTRUMENTS_HAS_QUIC=ON`
- Without this flag, tunnel returns `Error::NotSupported` (graceful fallback)

### Tunnel flow

1. `QUICTunnel::Connect()` creates UDP socket, picoquic context, QUIC handshake
2. Opens stream 0, sends `{"type":"clientHandshakeRequest","mtu":1280}`
3. Receives `serverHandshakeResponse` with IPv6 addresses + RSD port
4. Initializes `UserspaceNetwork` (lwIP) with tunnel IPv6 addresses
5. Starts forwarding thread: QUIC datagrams ↔ lwIP IPv6 packets
6. `RSDProvider::Connect()` creates lwIP TCP connection to RSD port (58783)
7. HTTP/2 preface + SETTINGS exchange
8. XPC InitHandshake → receives service port map

### Key files

- `src/connection/tunnel_quic.h/cpp` - QUIC tunnel (picoquic)
- `src/connection/userspace_network.h/cpp` - lwIP bridge
- `src/connection/http2_framer.h/cpp` - Minimal HTTP/2 framing
- `src/connection/rsd_provider.h/cpp` - RSD protocol (HTTP/2 + XPC)
- `src/connection/lwipopts.h` - lwIP configuration
- `src/connection/arch/cc.h` - lwIP platform config

### Backward compatibility

- `idevice_new_remote()` still works for remote usbmux proxy (sonic-gidevice / go-ios)
- `DeviceConnection::FromTunnel()` / `Instruments::CreateWithTunnel()` use remote proxy
- `TunnelManager::RegisterExternalTunnel()` works for external tunnel providers
- External tools: `pymobiledevice3 remote start-tunnel` or `ios tunnel start`

## Common Patterns

### Adding a new service

1. Create header in `include/instruments/` with the public API
2. Create implementation in `src/services/`
3. Add accessor method to `Instruments` facade class
4. Add source files to `CMakeLists.txt` `LIB_SOURCES` and `LIB_HEADERS`
5. Include the new header in `include/instruments/instruments.h`

### NSKeyedArchiver usage

```cpp
// Encode
NSObject dict(NSObject::DictMap{{"key", NSObject("value")}});
auto data = NSKeyedArchiver::Archive(dict);

// Decode
NSObject result = NSKeyedUnarchiver::Unarchive(data.data(), data.size());
```

### DTX method invocation

```cpp
auto msg = DTXMessage::CreateWithSelector("runningProcesses");
auto reply = channel->SendMessageSync(msg);
auto payload = reply->PayloadObject();  // NSKeyedArchiver-decoded result
```

## Reference Implementation

Ported from [go-ios](https://github.com/danielpaulus/go-ios) (Go), adapted to C++20 idioms:
- Go channels → `std::condition_variable` waiters
- Goroutines → `std::thread`
- `io.Copy` → bidirectional relay threads with select/poll
- Go interfaces → `std::function` callbacks

Don't use old dependencies `libidevice` and `libnskeyedarchiver` as code references and old implementation on `devicebridge_instrument.cpp`.

## Things to Watch Out For

- DTX message identifiers must be unique per connection (monotonic counter)
- Channel code 0 is the global channel (auto-created, never explicitly requested)
- Fragment reassembly is keyed by message identifier
- `_requestChannelWithCode:identifier:` must be called on the global channel
- XCTest proxy requires handling 26+ callback methods to avoid stalling the test runner
- Port forwarder connects via `idevice_connect()` for each accepted TCP connection
- Performance service requires setting both system AND process attributes before starting
