# CLAUDE.md - libinstruments

## What is this?

`libinstruments` is a **production-ready**, pure C++20 static library that implements Apple's Instruments (DTX) protocol for communicating with iOS devices. It lives at `Externals/libinstruments/` within the iDebugTool project and replaces the older `libnskeyedarchiver` + `libidevice` externals with a single, self-contained library.

**Status**: ✅ DTX protocol working and tested with process listing, FPS monitoring, and performance monitoring on iOS 15 via USB (as of Feb 2026). Designed to support iOS 14-17+.

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

⚠️ **CRITICAL PROTOCOL DETAILS** - These are lessons learned from debugging iOS 15 connections. Violating any of these will cause immediate connection closure by the device!

### Message Format

- **Magic**: `0x795B3D1F` (little-endian in header: `0x1F 0x3D 0x5B 0x79`)
- **Header**: 32 bytes (LE), payload header: 16 bytes
- **Message types**: Ack=0x0, MethodInvocation=0x2, ResponseWithPayload=0x3, Error=0x4, LZ4Compressed=0x0707
  - **CRITICAL**: Payload header's messageType field MUST include 0x1000 bit when ExpectsReply=true
  - Example: MethodInvocation with ExpectsReply=true is `0x1002`, with false is `0x0002`
  - Per iosif: `type = base_type | (expectsReply ? 0x1000 : 0)`
  - Both main header `expectsReply` field AND payload header type 0x1000 bit must be consistent

### PrimitiveDictionary Encoding (CRITICAL!)

- **Entry format**: MUST include 4-byte marker (0x0A) before each entry
  - Per pymobiledevice3 message_aux_t_struct: `marker + type + value`
  - Without marker: decoder fails with "Truncated entry" errors and device closes connection
- **Auxiliary header**: `[8 bytes: magic 0x1F0] [8 bytes: data length]`
  - Per iosif bytearr.c: 8-byte magic 0x1F0, then 8-byte length of data EXCLUDING the 16-byte header
  - Old format used 4x uint32 fields, causing device to misparse and close connection
- **Types**: null=0x0A, string=0x01, bytearray=0x02, uint32=0x03, int64=0x06
  - **Type 0x06 is UNSIGNED uint64_t**, not signed int64_t (per pymobiledevice3 Int64ul)
  - NSObject supports both Int64 and UInt64 types, encoder handles both correctly

### Handshake Protocol (CRITICAL!)

Client MUST send `_notifyOfPublishedCapabilities:` immediately after connection or device will close connection.

**Critical requirements**:
1. **ExpectsReply must be FALSE**: Bidirectional exchange, not request-response
   - If sent with ExpectsReply=true, device expects ACK protocol that breaks the handshake flow
   - Use SendMessageAsync() to send handshake, then wait for device response with condition variable
2. **Must wait for device response**: Wait for device's `_notifyOfPublishedCapabilities:` message (5 sec timeout)
   - Device sends its own `_notifyOfPublishedCapabilities:` message back
   - Matches pymobiledevice3: send_message() then recv_plist() blocks until device responds
   - Implementation uses std::condition_variable with predicate checking m_handshakeReceived flag
3. **Capability values**: Must use uint64_t type in NSObject
   - DTXBlockCompression: NSObject(static_cast<uint64_t>(2)) to match go-ios/sonic-gidevice
   - DTXConnection: NSObject(static_cast<uint64_t>(1))
   - These get encoded as PrimitiveDictionary type 0x06 (uint64) but represent valid uint values
4. **Message identifier synchronization (MOST IMPORTANT!)**: Must update identifier counter when device sends message
   - Per pymobiledevice3 lines 512-513: if device sends id=N, next client message must use id>=N+1
   - Without sync: client reuses device's identifier → device closes connection immediately
   - DTXChannel::SyncIdentifier() updates counter when receiving messages with conv=0

### ACK Behavior

ONLY send ACK if message.ExpectsReply==true (NO special case for handshake)
- Device's `_notifyOfPublishedCapabilities:` response has ExpectsReply=false, must NOT ACK it
- Incorrect ACK causes device to close connection immediately after handshake

### SSL Behavior (Version-Dependent)

- **Pre-iOS 14**: `com.apple.instruments.remoteserver` uses **handshake-only SSL** (SSL handshake for auth, then plaintext DTX)
- **iOS 14-16**: `com.apple.instruments.remoteserver.DVTSecureSocketProxy` uses **full SSL** (all DTX traffic encrypted)
- **iOS 17+**: `com.apple.instruments.dtservicehub` via RSD tunnel uses **no service-level SSL** (tunnel handles encryption)

### Fragments

- Fragment 0 is header-only when count > 1, subsequent fragments carry data

## Service Name Selection

```
iOS < 14:  com.apple.instruments.remoteserver                      (handshake-only SSL)
iOS 14-16: com.apple.instruments.remoteserver.DVTSecureSocketProxy (full SSL)
iOS 17+:   com.apple.instruments.dtservicehub                      (RSD tunnel, no service SSL)
```

TestManagerD follows the same pattern:
```
iOS < 14:  com.apple.testmanagerd.lockdown        (handshake-only SSL)
iOS 14+:   com.apple.testmanagerd.lockdown.secure (full SSL)
iOS 17+:   com.apple.dt.testmanagerd.remote       (RSD tunnel)
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

### Global Message Handler Chain

`DTXConnection::AddGlobalMessageHandler()` allows multiple handlers to be registered. Each call wraps the previous handler in a lambda chain:

```cpp
void AddGlobalMessageHandler(MessageHandler handler) {
    auto existing = m_globalHandler;
    m_globalHandler = [existing, handler](auto msg) {
        if (existing) existing(msg);  // Call previous handler
        if (handler) handler(msg);    // Call new handler
    };
}
```

This enables multiple services to coexist on the same connection, each filtering for their specific channel codes.

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

- `idevice_new_remote()` works for remote usbmux proxy (sonic-gidevice / go-ios)
- Call patched functions, then pass to `Instruments::Create(device, lockdown)`
- `TunnelManager::RegisterExternalTunnel()` works for external tunnel providers
- External tools: `pymobiledevice3 remote start-tunnel` or `ios tunnel start`

## Remote Usbmux Proxy Support

### Patched libimobiledevice Functions

The remote usbmux proxy feature (✅ tested on iOS 15) requires **custom functions NOT present in official libimobiledevice**:

1. **`idevice_error_t idevice_new_remote(idevice_t* device, const char* ip_address, uint16_t port)`**
   - Creates a device connection to a remote usbmux proxy (e.g., sonic-gidevice, go-ios)
   - Connects to `ip_address:port` instead of local usbmuxd socket
   - Returns the same `idevice_t` handle type as local connections
   - Example: `idevice_new_remote(&device, "192.168.1.100", 5555)`

2. **`lockdownd_error_t lockdownd_client_new_with_handshake_remote(idevice_t device, lockdownd_client_t* client, const char* label)`**
   - Performs lockdown handshake via the already-connected remote device
   - Uses the remote connection established by `idevice_new_remote()`
   - Parallel to `lockdownd_client_new_with_handshake()` but for remote connections
   - Does NOT take host/port parameters (uses existing device connection)

### Build Script Patching

**In iDebugTool**: These functions are patched by the build script automatically during the build process:
- Patch location: `Externals/_Patches/libimobiledevice.patch` applied to libimobiledevice source
- Implementation: Functions handle TCP connection to remote host instead of local unix/TCP socket
- Transparent: libimobiledevice API remains the same, only connection establishment differs
- Applied during build: Premake5/CMake copies and patches libimobiledevice sources

### How It Works

```cpp
// Application code (e.g., iDebugTool, instruments-cli):
idevice_t device = nullptr;
idevice_new_remote(&device, "192.168.1.100", 5555)
  → (libimobiledevice patched) TCP connect to 192.168.1.100:5555
  → (libimobiledevice patched) Handshake with remote usbmux proxy
  → (proxy) Forwards usbmux protocol to local iOS device over USB
  → Returns idevice_t handle

lockdownd_client_t lockdown = nullptr;
lockdownd_client_new_with_handshake_remote(device, &lockdown, "my-app")
  → (libimobiledevice patched) Uses existing remote device connection
  → (libimobiledevice) Opens lockdown service via remote proxy
  → (libimobiledevice) Performs SSL/TLS handshake if required
  → Returns lockdownd_client_t handle

// Create Instruments instance (libinstruments):
auto inst = Instruments::Create(device, lockdown)
  → Creates DeviceConnection from existing device/lockdown handles
  → Detects iOS version and protocol (pre-14, 14-16, 17+)
  → All DTX protocol operations work identically to USB connections
  → Library does NOT take ownership of device/lockdown (caller manages lifetime)
```

### Testing Status

**✅ Verified Working on iOS 15 via sonic-gidevice**:
- Process listing (`GetProcessList()`) successfully retrieves remote device processes
- FPS monitoring (`FPSService::Start()`) successfully monitors remote device FPS/GPU
- DTX protocol handshake and channel management work over network
- Compatible with sonic-gidevice shared port (tested with default port 5555)

### Usage Example

```cpp
#include <instruments/instruments.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

// Connect to remote usbmux proxy
idevice_t device = nullptr;
idevice_error_t err = idevice_new_remote(&device, "192.168.1.100", 5555);
if (err != IDEVICE_E_SUCCESS) {
    fprintf(stderr, "Failed to connect to remote usbmux\n");
    return 1;
}

// Perform lockdown handshake
lockdownd_client_t lockdown = nullptr;
lockdownd_error_t lerr = lockdownd_client_new_with_handshake_remote(device, &lockdown, "my-app");
if (lerr != LOCKDOWN_E_SUCCESS) {
    fprintf(stderr, "Failed lockdown handshake\n");
    idevice_free(device);
    return 1;
}

// Create Instruments instance
auto inst = instruments::Instruments::Create(device, lockdown);
if (!inst) {
    fprintf(stderr, "Failed to create Instruments\n");
    lockdownd_client_free(lockdown);
    idevice_free(device);
    return 1;
}

// Use services (same API as local USB)
std::vector<instruments::ProcessInfo> procs;
inst->Process().GetProcessList(procs);
for (const auto& p : procs) {
    printf("PID: %lld %s\n", p.pid, p.name.c_str());
}

// Cleanup when done
inst.reset();  // Destroy Instruments instance first
lockdownd_client_free(lockdown);
idevice_free(device);
```

**External proxy setup examples**:

```bash
# sonic-gidevice (Go-based)
$ gidevice share --port 5555

# go-ios (Go-based)
$ ios forward --udid <UDID> --port 5555
```

### Notes

- This is **NOT an iOS 17+ RSD tunnel** - it's a remote usbmux proxy that works on iOS 14-16 (and possibly other versions)
- Network latency impacts real-time monitoring performance (FPS, performance metrics)
- The patched functions are required only for remote connections; local USB connections use standard libimobiledevice
- Compatible with multiple proxy implementations: sonic-gidevice, go-ios, or custom implementations
- Can be used alongside local USB connections in the same application

## Service Implementation Patterns (iOS 15 Tested)

### Global Message Handler Pattern

Many DTX services send messages on BOTH their dedicated channel AND the global/default channel (channel code -1 or 0xFFFFFFFF). This was discovered during iOS 15 testing with FPS and Performance services.

**Critical requirement**: Services MUST register handlers for both:
1. Their dedicated channel via `channel->SetMessageHandler()`
2. The global channel via `dtxConnection->AddGlobalMessageHandler()`

**Example pattern**:
```cpp
// Create shared lambda for parsing messages
auto parseMessage = [this, callback](std::shared_ptr<DTXMessage> msg) {
    if (!m_running.load()) return;
    if (!msg) return;

    auto payload = msg->PayloadObject();
    if (payload) {
        ParsePayload(*payload, callback);
    }
};

// Register on dedicated channel
m_channel->SetMessageHandler(parseMessage);

// Register on global channel for channel-code=-1 messages
if (m_dtxConnection) {
    m_dtxConnection->AddGlobalMessageHandler([parseMessage](std::shared_ptr<DTXMessage> msg) {
        if (!msg) return;
        if (static_cast<int32_t>(msg->ChannelCode()) == -1) {
            parseMessage(msg);
        }
    });
}
```

### Early Message Arrival

Some services (like sysmontap) may send data messages BEFORE the "start" method returns. This requires:
1. Set `m_running = true` BEFORE sending the start command
2. Register message handlers BEFORE sending start
3. Only then send the start command

**Example**:
```cpp
// 1. Configure and send setConfig first
m_channel->SendMessageSync(setConfigMsg);

// 2. Set running flag and register handlers
m_running.store(true);
m_channel->SetMessageHandler(parseMessage);
m_dtxConnection->AddGlobalMessageHandler(globalHandler);

// 3. NOW send start (messages may arrive immediately)
m_channel->SendMessageSync(startMsg);
```

### Performance Service (sysmontap) Payload Formats

iOS 15 sysmontap service returns data in multiple possible formats. The parser must handle ALL of these:

**Format 1: Top-level Processes dict** (dict-based, keyed by PID string)
```json
{
  "SystemCPUUsage": {...},
  "Processes": {
    "123": ["launchd", 0.1, 12345678, ...],  // array of values
    "456": {"pid": 456, "name": "foo", "cpuUsage": 1.2, ...}  // or dict
  }
}
```

**Format 2: Nested System dict** (dict-based, various key names)
```json
{
  "System": {
    "Processes": {...},          // or
    "processes": {...},          // or
    "ProcessByPid": {...},       // or
    "processByPid": {...}
  }
}
```

**Format 3: Array-packed layout** (most efficient, used on some iOS versions)
```json
{
  "SystemAttributes": ["cpu_total_load", "memUsed", ...],
  "ProcessesAttributes": ["pid", "name", "cpuUsage", "physFootprint", ...],
  "System": [
    // System values (systemAttributes.length items)
    45.2, 1234567890, ...,
    // Process 1 values (processAttributes.length items)
    123, "launchd", 0.1, 12345678, ...,
    // Process 2 values
    456, "foo", 1.2, 23456789, ...
  ]
}
```

**Implementation**: `ParseSysmontapMessage()` in performance_service.cpp handles all three formats with fallback logic.

### FPS Service Rate Limiting

The FPS service receives messages at a high frequency (often faster than requested). To honor the `sampleIntervalMs` parameter, the service implements rate limiting:

```cpp
std::atomic<uint32_t> m_sampleIntervalMs{0};
std::atomic<int64_t> m_lastCallbackMs{0};

// In message handler:
const uint32_t intervalMs = m_sampleIntervalMs.load();
if (intervalMs > 0) {
    const auto now = std::chrono::steady_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const auto lastMs = m_lastCallbackMs.load();
    if (lastMs != 0 && (nowMs - lastMs) < static_cast<int64_t>(intervalMs)) {
        return;  // Skip this callback, too soon
    }
    m_lastCallbackMs.store(nowMs);
}
callback(fpsData);
```

### Attribute Auto-Population

Performance service auto-populates system and process attributes if not specified in config:
```cpp
if (config.systemAttributes.empty()) {
    GetSystemAttributes(config.systemAttributes);  // Query device
    if (config.systemAttributes.empty()) {
        // Fallback to defaults if query fails
        config.systemAttributes = {"cpu_total_load", "memUsed", ...};
    }
}
```

This ensures the service works out-of-the-box while allowing customization.

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

## Reference Implementations

Ported from and validated against multiple iOS tools:

- **[go-ios](https://github.com/danielpaulus/go-ios)** (Go) - Primary reference for DTX protocol, adapted to C++20:
  - Go channels → `std::condition_variable` waiters
  - Goroutines → `std::thread`
  - `io.Copy` → bidirectional relay threads with select/poll
  - Go interfaces → `std::function` callbacks

- **[pymobiledevice3](https://github.com/doronz88/pymobiledevice3)** (Python) - SSL behavior reference:
  - Validated SSL handling: pre-14 handshake-only vs iOS 14+ full SSL
  - DTX message format and NSKeyedArchiver encoding
  - Service name selection and RSD tunnel implementation
  - Location: `pymobiledevice3/services/dvt/` and `pymobiledevice3/services/remote_server.py`

- **[sonic-gidevice](https://github.com/SonicCloudOrg/sonic-gidevice)** (Go) - Remote usbmux proxy and DTX protocol reference:
  - Remote device communication via usbmux proxy
  - DTX protocol implementation and message handling
  - Alternative Go-based approach to iOS device communication

- **[iosif](https://github.com/dryark/iosif)** (C) - Low-level DTX protocol implementation reference:
  - Critical format details: `bytearr.c` (PrimitiveDictionary auxiliary header format)
  - Payload header encoding: `dtxpayload__new` (messageType 0x1000 bit handling)
  - Binary protocol validation and edge case handling

Don't use old dependencies `libidevice` and `libnskeyedarchiver` as code references and old implementation on `devicebridge_instrument.cpp`.

## Testing Status

### ✅ Verified Working on iOS 15
- **Process listing** - GetProcessList() successfully retrieves running processes with PID, name, bundle ID (tested via USB and remote proxy)
- **FPS monitoring** - FPSService successfully monitors real-time FPS and GPU utilization via graphics.opengl channel with rate limiting (tested via USB and remote proxy)
- **Performance monitoring** - PerformanceService successfully monitors system CPU/memory and per-process metrics via sysmontap (tested via USB on iOS 15)
  - All three payload formats supported: dict-based (Processes key), nested dict (System.processes/ProcessByPid), and array-packed
  - Handles messages on both dedicated channel and global channel (-1)
  - Auto-populates attributes with fallback to defaults
- **DTX protocol core** - Handshake, message exchange, channel management, callbacks, global message routing all working
- **USB connection** - Direct device connection via libimobiledevice
- **Remote usbmux proxy** - Connection via sonic-gidevice shared port (tested with process listing, FPS monitoring, and performance monitoring)

### 🔄 Implemented But Not Yet Tested
- Process launch/kill operations
- Port forwarding
- iOS 17+ QUIC tunnel support (requires INSTRUMENTS_HAS_QUIC build flag)
- XCTest service
- WDA service
- iOS < 14 and iOS 16+ versions (protocol design supports them)

## Debugging Journey & Lessons Learned (Feb 2026)

This library was successfully implemented after fixing several critical protocol issues that caused immediate connection closure by iOS 15 devices. These issues are documented here to prevent regressions.

### Problem 1: Message Identifier Collision (MOST CRITICAL)
**Symptom**: Device closes connection immediately after handshake, no obvious error message.

**Root Cause**: Client was reusing message identifier 0 after device sent a message with id=0. When both client and device send messages with the same identifier simultaneously, the device closes the connection.

**Fix**: Implement identifier synchronization - when device sends a message with id=N on conversation=0 (unsolicited message), client must update its counter to id>=N+1 before sending next message. Implemented in `DTXChannel::SyncIdentifier()`.

**Reference**: pymobiledevice3 lines 512-513, go-ios identifier management.

### Problem 2: PrimitiveDictionary Missing Markers
**Symptom**: Device logs "Truncated entry" errors and closes connection when receiving client messages.

**Root Cause**: PrimitiveDictionary entries were encoded as `[type][value]` without the required 4-byte marker (0x0A) prefix. Device's decoder expects `[marker=0x0A][type][value]` for each entry.

**Fix**: Modified `DTXPrimitiveDict::Encode()` to write 0x0A marker before each entry.

**Reference**: pymobiledevice3 message_aux_t_struct format.

### Problem 3: Incorrect Auxiliary Header Format
**Symptom**: Device misparses auxiliary data, sends back malformed responses, closes connection.

**Root Cause**: Auxiliary header used 4x uint32 fields (old format), but device expects 8-byte magic (0x1F0) followed by 8-byte length (excluding the 16-byte header itself).

**Fix**: Changed header format to `[uint64 magic=0x1F0][uint64 length]`.

**Reference**: [iosif](https://github.com/dryark/iosif) `bytearr.c` auxiliary encoding implementation.

### Problem 4: Missing 0x1000 Bit in Payload MessageType
**Symptom**: Device doesn't respond to messages that should get replies.

**Root Cause**: Payload header messageType field was set to base type (e.g., 0x0002 for MethodInvocation) without including the 0x1000 bit when ExpectsReply=true.

**Fix**: Set messageType = baseType | (expectsReply ? 0x1000 : 0). E.g., MethodInvocation with reply is 0x1002, without is 0x0002.

**Reference**: [iosif](https://github.com/dryark/iosif) `dtxpayload__new` implementation.

### Problem 5: Handshake Message with ExpectsReply=true
**Symptom**: Device closes connection immediately after handshake exchange.

**Root Cause**: Client sent handshake message with ExpectsReply=true, causing device to wait for ACK. But handshake is a bidirectional exchange, not request-response, so ACK protocol breaks the flow.

**Fix**: Send handshake with ExpectsReply=false, but still wait for device's response using SendMessageSync() with timeout.

**Reference**: pymobiledevice3 DvtSecureSocketProxyService handshake implementation.

### Problem 6: Capability Value Types
**Symptom**: Capability values in handshake misinterpreted by device, causing protocol errors.

**Root Cause**: Capability values must be created as NSObject(uint64_t) to match go-ios/sonic-gidevice encoding. PrimitiveDictionary type 0x06 is UNSIGNED uint64_t (per pymobiledevice3 Int64ul).

**Fix**: Create capability NSObjects with explicit uint64_t: `NSObject(static_cast<uint64_t>(2))` for DTXBlockCompression, `NSObject(static_cast<uint64_t>(1))` for DTXConnection. The PrimitiveDictionary encoder uses WriteLE64() with uint64_t.

**Reference**: go-ios capability encoding, pymobiledevice3 message_aux_t_struct line 87 (Int64ul = unsigned).

### Problem 7: Global Channel Message Routing (Feb 2026)
**Symptom**: FPS and Performance services miss some messages, appear to work intermittently.

**Root Cause**: Some iOS services (graphics.opengl, sysmontap) send messages on BOTH their dedicated channel AND the global/default channel (channel code -1 or 0xFFFFFFFF). Services only registered handlers on their dedicated channel.

**Fix**: Implement `AddGlobalMessageHandler()` in DTXConnection to allow multiple handlers. Services now register parsers on both channels:
```cpp
m_channel->SetMessageHandler(parseMessage);
m_dtxConnection->AddGlobalMessageHandler([parseMessage](auto msg) {
    if (msg && static_cast<int32_t>(msg->ChannelCode()) == -1) {
        parseMessage(msg);
    }
});
```

**Reference**: Tested on iOS 15, both FPS and Performance services require this pattern.

### Problem 8: Early Message Arrival (Feb 2026)
**Symptom**: Some performance data lost at service start, first few samples missing.

**Root Cause**: sysmontap sends data immediately after receiving "start" command, before SendMessageSync() returns. If m_running flag and handlers aren't set up first, early messages are dropped.

**Fix**: Set m_running=true and register handlers BEFORE sending start command:
```cpp
m_channel->SendMessageSync(setConfigMsg);  // 1. Config first
m_running.store(true);                      // 2. Enable processing
m_channel->SetMessageHandler(...);          // 3. Register handlers
m_channel->SendMessageSync(startMsg);       // 4. Now start (messages arrive immediately)
```

### Problem 9: Multiple sysmontap Payload Formats (Feb 2026)
**Symptom**: Process metrics work on some devices/sessions but not others, or return empty.

**Root Cause**: iOS 15 sysmontap returns data in at least three different formats:
1. Dict with top-level "Processes" key
2. Dict with nested "System" dict containing "processes"/"ProcessByPid" keys (case variations)
3. Array-packed format with "ProcessesAttributes" and flat "System" array

**Fix**: Implement comprehensive parser in `ParseSysmontapMessage()` that tries all format variations with fallback logic. See performance_service.cpp lines 210-461 for full implementation.

### Key Takeaways
1. **Always sync message identifiers** when device sends unsolicited messages
2. **PrimitiveDictionary format is strict** - must include 0x0A marker before each entry
3. **Handshake is bidirectional** - not request-response, so ExpectsReply=false
4. **Payload messageType includes 0x1000 bit** when expecting replies
5. **Capability values** - use NSObject(uint64_t) for capability values in handshake
6. **Global message handler required** - services send on both dedicated channel and channel -1
7. **Set handlers before start** - messages may arrive immediately, must be ready
8. **Handle multiple payload formats** - iOS devices use different encoding schemes

## Things to Watch Out For

- **DTX handshake is mandatory and complex**: See "Handshake Protocol" section above for all critical requirements
- **Message identifier synchronization**: MUST sync counter when device sends messages (most common failure mode)
- **PrimitiveDictionary format**: MUST include 0x0A marker before each entry
- DTX message identifiers must be unique per connection (monotonic counter)
- Channel code 0 is the global channel (auto-created, never explicitly requested)
- Fragment reassembly is keyed by message identifier
- `_requestChannelWithCode:identifier:` must be called on the global channel
- XCTest proxy requires handling 26+ callback methods to avoid stalling the test runner
- Port forwarder connects via `idevice_connect()` for each accepted TCP connection
- Performance service requires setting both system AND process attributes before starting
