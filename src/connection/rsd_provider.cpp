#include "rsd_provider.h"
#include "http2_framer.h"
#include "../util/log.h"
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using rsd_socket_t = SOCKET;
#define RSD_SOCKET_INVALID INVALID_SOCKET
#define RSD_CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
using rsd_socket_t = int;
#define RSD_SOCKET_INVALID (-1)
#define RSD_CLOSE_SOCKET ::close
#endif

namespace instruments {

static const char* TAG = "RSDProvider";

RSDProvider::RSDProvider() = default;
RSDProvider::~RSDProvider() = default;

Error RSDProvider::Connect(const std::string& tunnelAddress, uint16_t rsdPort) {
    INST_LOG_WARN(TAG, "Direct RSD handshake requires UserspaceNetwork. "
                 "Use Connect(address, port, network) instead.");
    return Error::NotSupported;
}

Error RSDProvider::Connect(const std::string& tunnelAddress, uint16_t rsdPort,
                           UserspaceNetwork* network) {
    if (!network || !network->IsInitialized()) {
        INST_LOG_ERROR(TAG, "UserspaceNetwork not initialized");
        return Error::InvalidArgument;
    }

    INST_LOG_INFO(TAG, "Connecting to RSD at [%s]:%u", tunnelAddress.c_str(), rsdPort);

    // Step 1: Create TCP connection through the tunnel
    std::shared_ptr<UserspaceTcpConnection> tcpConn;
    Error err = network->TcpConnect(tunnelAddress, rsdPort, tcpConn);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to create TCP connection to RSD");
        return err;
    }

    // Wait for connection to establish (lwIP TCP handshake)
    std::mutex connMutex;
    std::condition_variable connCv;
    bool connected = false;
    bool connError = false;

    tcpConn->SetErrorCallback([&](Error) {
        std::lock_guard<std::mutex> lock(connMutex);
        connError = true;
        connCv.notify_one();
    });

    // Poll the network until connected or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!tcpConn->IsConnected() && !connError) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "TCP connection to RSD timed out");
            return Error::Timeout;
        }
        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (connError || !tcpConn->IsConnected()) {
        INST_LOG_ERROR(TAG, "TCP connection to RSD failed");
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "TCP connected to RSD");
    m_tcpConn = tcpConn;

    // Step 2: Accumulate received data
    std::mutex recvMutex;
    std::vector<uint8_t> recvBuffer;
    bool recvDone = false;

    tcpConn->SetRecvCallback([&](const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(recvMutex);
        recvBuffer.insert(recvBuffer.end(), data, data + length);
    });

    tcpConn->SetErrorCallback([&](Error) {
        std::lock_guard<std::mutex> lock(recvMutex);
        recvDone = true;
    });

    // Step 3: Send HTTP/2 connection preface
    auto preface = Http2Framer::MakeConnectionPreface();
    err = tcpConn->Send(preface.data(), preface.size());
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to send HTTP/2 preface");
        return err;
    }

    // Also send a window update for connection-level flow control
    auto windowUpdate = Http2Framer::MakeWindowUpdateFrame(0, 1048576);
    tcpConn->Send(windowUpdate.data(), windowUpdate.size());

    INST_LOG_DEBUG(TAG, "Sent HTTP/2 connection preface");

    // Step 4: Wait for server SETTINGS
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gotServerSettings = false;

    while (!gotServerSettings && !recvDone) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Timeout waiting for server SETTINGS");
            return Error::Timeout;
        }

        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Try to decode frames
        std::lock_guard<std::mutex> lock(recvMutex);
        size_t offset = 0;
        while (offset < recvBuffer.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(
                recvBuffer.data() + offset, recvBuffer.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                INST_LOG_DEBUG(TAG, "Received server SETTINGS");
                // Send SETTINGS ACK
                auto ack = Http2Framer::MakeSettingsFrame(true);
                tcpConn->Send(ack.data(), ack.size());
                gotServerSettings = true;
            }
        }
        if (offset > 0) {
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + offset);
        }
    }

    if (!gotServerSettings) {
        INST_LOG_ERROR(TAG, "Failed to receive server SETTINGS");
        return Error::ProtocolError;
    }

    // Step 5: Send XPC InitHandshake on stream 1 (client-initiated bidi)
    XPCMessage initMsg;
    initMsg.flags = XPCFlags::AlwaysSet | XPCFlags::InitHandshake;
    initMsg.messageId = 1;

    auto xpcData = initMsg.Encode();

    // Send as HTTP/2 DATA frame on stream 1
    // First send empty HEADERS to open the stream
    auto headers = Http2Framer::MakeHeadersFrame(1, {}, false);
    tcpConn->Send(headers.data(), headers.size());

    auto dataFrame = Http2Framer::MakeDataFrame(1, xpcData.data(), xpcData.size(), false);
    err = tcpConn->Send(dataFrame.data(), dataFrame.size());
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to send XPC InitHandshake");
        return err;
    }

    INST_LOG_DEBUG(TAG, "Sent XPC InitHandshake on stream 1");

    // Step 6: Receive the XPC response (service discovery)
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gotXPCResponse = false;

    while (!gotXPCResponse && !recvDone) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Timeout waiting for XPC response");
            return Error::Timeout;
        }

        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Try to decode frames
        std::lock_guard<std::mutex> lock(recvMutex);
        size_t offset = 0;
        while (offset < recvBuffer.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(
                recvBuffer.data() + offset, recvBuffer.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Data && !frame.payload.empty()) {
                // Try to decode as XPC message
                XPCMessage xpcResp;
                if (XPCMessage::Decode(frame.payload, xpcResp)) {
                    INST_LOG_INFO(TAG, "Received XPC response (flags=0x%x msgId=%llu)",
                                 xpcResp.flags, (unsigned long long)xpcResp.messageId);

                    if (xpcResp.body.IsDict()) {
                        const size_t oldCount = m_services.size();
                        ParseServiceResponse(xpcResp.body);
                        if (m_services.size() > oldCount || !m_services.empty()) {
                            gotXPCResponse = true;
                        } else {
                            INST_LOG_DEBUG(TAG, "RSD: dict response did not include services yet");
                        }
                    }
                }
            } else if (frame.type == H2FrameType::Settings) {
                if (!(frame.flags & H2Flags::Ack)) {
                    auto ack = Http2Framer::MakeSettingsFrame(true);
                    tcpConn->Send(ack.data(), ack.size());
                }
            } else if (frame.type == H2FrameType::WindowUpdate) {
                // Acknowledge window updates silently
            }
        }
        if (offset > 0) {
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + offset);
        }
    }

    if (!gotXPCResponse) {
        INST_LOG_ERROR(TAG, "Failed to receive XPC service discovery response");
        return Error::ProtocolError;
    }

    INST_LOG_INFO(TAG, "RSD handshake complete: UDID=%s, %zu services discovered",
                 m_udid.c_str(), m_services.size());

    return Error::Success;
}

Error RSDProvider::DoRSDHandshake(
    std::function<bool(const std::vector<uint8_t>&)> sendFn,
    std::function<bool(std::vector<uint8_t>&)> recvMoreFn)
{
    std::vector<uint8_t> recvBuf;
    std::map<uint32_t, std::vector<uint8_t>> xpcStreamBuffers;
    std::map<uint32_t, std::deque<XPCMessage>> pendingXpcByStream;

    auto readU32Le = [](const uint8_t* p) -> uint32_t {
        return static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    };
    auto readU64Le = [](const uint8_t* p) -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= (static_cast<uint64_t>(p[i]) << (8 * i));
        }
        return v;
    };

    auto appendDataFrameAndExtractXpc = [&](uint32_t streamId,
                                            const std::vector<uint8_t>& payload) {
        constexpr uint32_t kWrapperMagic = 0x29b00b92;
        constexpr uint64_t kMaxXpcFrameLen = 8ULL * 1024ULL * 1024ULL; // sanity cap

        auto& sb = xpcStreamBuffers[streamId];
        sb.insert(sb.end(), payload.begin(), payload.end());

        size_t offset = 0;
        while (sb.size() >= offset + 24) {
            const uint8_t* p = sb.data() + offset;
            uint32_t magic = readU32Le(p);

            if (magic != kWrapperMagic) {
                // Resync: find next potential wrapper header.
                size_t next = offset + 1;
                while (next + 4 <= sb.size()) {
                    if (readU32Le(sb.data() + next) == kWrapperMagic) break;
                    ++next;
                }
                INST_LOG_WARN(TAG, "RSD: XPC stream %u lost sync, dropped %zu byte(s)",
                              streamId, next - offset);
                offset = next;
                continue;
            }

            uint64_t bodyLen = readU64Le(p + 8);
            uint64_t totalLen64 = 24 + bodyLen;
            if (totalLen64 > kMaxXpcFrameLen) {
                INST_LOG_WARN(TAG, "RSD: XPC frame too large on stream %u (%llu), dropping 1 byte",
                              streamId, static_cast<unsigned long long>(totalLen64));
                offset += 1;
                continue;
            }

            size_t totalLen = static_cast<size_t>(totalLen64);
            if (sb.size() - offset < totalLen) {
                // Partial XPC message, wait for more DATA frames.
                break;
            }

            XPCMessage xpc;
            if (XPCMessage::Decode(sb.data() + offset, totalLen, xpc)) {
                pendingXpcByStream[streamId].push_back(std::move(xpc));
                offset += totalLen;
                continue;
            }

            // Decoder rejected this candidate. Advance one byte and try to resync.
            INST_LOG_WARN(TAG, "RSD: failed to decode XPC message on stream %u, attempting resync",
                          streamId);
            offset += 1;
        }

        if (offset > 0) {
            sb.erase(sb.begin(), sb.begin() + offset);
        }
    };

    auto popNextXpc = [&](uint32_t streamId, XPCMessage& outMsg) -> bool {
        auto it = pendingXpcByStream.find(streamId);
        if (it == pendingXpcByStream.end() || it->second.empty()) return false;
        outMsg = std::move(it->second.front());
        it->second.pop_front();
        return true;
    };

    // Step 1: Send HTTP/2 connection preface + window update
    if (!sendFn(Http2Framer::MakeConnectionPreface())) {
        INST_LOG_ERROR(TAG, "RSD: failed to send HTTP/2 preface");
        return Error::ConnectionFailed;
    }
    sendFn(Http2Framer::MakeWindowUpdateFrame(0, 1048576));
    INST_LOG_DEBUG(TAG, "RSD: sent HTTP/2 preface");

    // Step 2: Wait for server SETTINGS
    bool gotServerSettings = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!gotServerSettings) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "RSD: timeout waiting for server SETTINGS");
            return Error::Timeout;
        }
        if (!recvMoreFn(recvBuf)) {
            INST_LOG_ERROR(TAG, "RSD: connection closed waiting for SETTINGS");
            return Error::ConnectionFailed;
        }
        size_t offset = 0;
        while (offset < recvBuf.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(recvBuf.data() + offset,
                                                        recvBuf.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;
            if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                sendFn(Http2Framer::MakeSettingsFrame(true));
                gotServerSettings = true;
            }
        }
        if (offset > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + offset);
    }

    // Step 3: RemoteXPC stream initialization (matches go-ios initializeXpcConnection):
    //   stream 1: flags=AlwaysSet, body={}
    //   stream 3: flags=AlwaysSet|InitHandshake, body=nil
    //   stream 1: flags=0x201, body=nil
    // then read handshake/service response on stream 1.
    std::set<uint32_t> openedStreams;
    auto sendXpc = [&](uint32_t streamId, uint32_t flags, const NSObject* body, uint64_t msgId) -> bool {
        XPCMessage msg;
        msg.flags = flags;
        msg.messageId = msgId;
        msg.body = body ? *body : NSObject::Null();
        auto wire = msg.Encode();

        if (!openedStreams.count(streamId)) {
            if (!sendFn(Http2Framer::MakeHeadersFrame(streamId, {}, false))) {
                return false;
            }
            openedStreams.insert(streamId);
        }
        return sendFn(Http2Framer::MakeDataFrame(streamId, wire.data(), wire.size(), false));
    };

    auto waitForXpcOnStream = [&](uint32_t wantedStream, XPCMessage& outMsg,
                                  std::chrono::steady_clock::time_point dl) -> Error {
        while (true) {
            if (std::chrono::steady_clock::now() > dl) return Error::Timeout;

            {
                XPCMessage ready;
                if (popNextXpc(wantedStream, ready)) {
                    outMsg = std::move(ready);
                    return Error::Success;
                }
            }

            if (!recvMoreFn(recvBuf)) return Error::ProtocolError;

            size_t offset = 0;
            while (offset < recvBuf.size()) {
                H2Frame frame;
                size_t consumed = Http2Framer::DecodeFrame(recvBuf.data() + offset,
                                                           recvBuf.size() - offset, frame);
                if (consumed == 0) break;
                offset += consumed;

                if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                    sendFn(Http2Framer::MakeSettingsFrame(true));
                    continue;
                }
                if (frame.type == H2FrameType::Data && !frame.payload.empty()) {
                    appendDataFrameAndExtractXpc(frame.streamId, frame.payload);
                    if (frame.streamId == wantedStream) {
                        XPCMessage ready;
                        if (popNextXpc(wantedStream, ready)) {
                            outMsg = std::move(ready);
                            if (offset > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + offset);
                            return Error::Success;
                        }
                    }
                }
            }
            if (offset > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + offset);
        }
    };

    NSObject emptyDict = NSObject::MakeDict({});
    if (!sendXpc(1, XPCFlags::AlwaysSet, &emptyDict, 0)) {
        INST_LOG_ERROR(TAG, "RSD: failed to send XPC init step 1 (stream 1)");
        return Error::ConnectionFailed;
    }
    {
        XPCMessage ignored;
        Error e = waitForXpcOnStream(1, ignored, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        if (e != Error::Success) {
            INST_LOG_ERROR(TAG, "RSD: failed waiting for XPC init step 1 reply");
            return e;
        }
        INST_LOG_DEBUG(TAG, "RSD: init step 1 reply flags=0x%x msgId=%llu",
                       ignored.flags, (unsigned long long)ignored.messageId);
    }

    if (!sendXpc(3, XPCFlags::AlwaysSet | XPCFlags::InitHandshake, nullptr, 0)) {
        INST_LOG_ERROR(TAG, "RSD: failed to send XPC init step 2 (stream 3)");
        return Error::ConnectionFailed;
    }
    {
        XPCMessage ignored;
        Error e = waitForXpcOnStream(3, ignored, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        if (e != Error::Success) {
            INST_LOG_ERROR(TAG, "RSD: failed waiting for XPC init step 2 reply");
            return e;
        }
        INST_LOG_DEBUG(TAG, "RSD: init step 2 reply flags=0x%x msgId=%llu",
                       ignored.flags, (unsigned long long)ignored.messageId);
    }

    if (!sendXpc(1, 0x201, nullptr, 0)) {
        INST_LOG_ERROR(TAG, "RSD: failed to send XPC init step 3 (stream 1)");
        return Error::ConnectionFailed;
    }
    {
        XPCMessage ignored;
        Error e = waitForXpcOnStream(1, ignored, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        if (e != Error::Success) {
            INST_LOG_ERROR(TAG, "RSD: failed waiting for XPC init step 3 reply");
            return e;
        }
        INST_LOG_DEBUG(TAG, "RSD: init step 3 reply flags=0x%x msgId=%llu",
                       ignored.flags, (unsigned long long)ignored.messageId);
    }

    INST_LOG_DEBUG(TAG, "RSD: RemoteXPC stream initialization complete");

    // Step 4: Receive XPC service discovery response on stream 1
    bool gotXPCResponse = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!gotXPCResponse) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "RSD: timeout waiting for XPC response");
            return Error::Timeout;
        }

        {
            XPCMessage xpcResp;
            while (popNextXpc(1, xpcResp)) {
                INST_LOG_INFO(TAG, "RSD: received XPC response (flags=0x%x msgId=%llu)",
                             xpcResp.flags, (unsigned long long)xpcResp.messageId);
                if (xpcResp.body.IsDict()) {
                    const size_t oldCount = m_services.size();
                    ParseServiceResponse(xpcResp.body);
                    if (m_services.size() > oldCount || !m_services.empty()) {
                        gotXPCResponse = true;
                        break;
                    }
                    INST_LOG_DEBUG(TAG, "RSD: dict response did not include services yet");
                } else {
                    INST_LOG_DEBUG(TAG, "RSD: ignoring XPC response with non-dict body");
                }
            }
            if (gotXPCResponse) break;
        }

        if (!recvMoreFn(recvBuf)) {
            INST_LOG_ERROR(TAG, "RSD: connection closed waiting for XPC response");
            return Error::ProtocolError;
        }
        size_t offset = 0;
        while (offset < recvBuf.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(recvBuf.data() + offset,
                                                        recvBuf.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Data && !frame.payload.empty()) {
                appendDataFrameAndExtractXpc(frame.streamId, frame.payload);
                if (frame.streamId == 1) {
                    XPCMessage xpcResp;
                    while (popNextXpc(1, xpcResp)) {
                        INST_LOG_INFO(TAG, "RSD: received XPC response (flags=0x%x msgId=%llu)",
                                     xpcResp.flags, (unsigned long long)xpcResp.messageId);
                        if (xpcResp.body.IsDict()) {
                            const size_t oldCount = m_services.size();
                            ParseServiceResponse(xpcResp.body);
                            if (m_services.size() > oldCount || !m_services.empty()) {
                                gotXPCResponse = true;
                                break;
                            }
                            INST_LOG_DEBUG(TAG, "RSD: dict response did not include services yet");
                        } else {
                            INST_LOG_DEBUG(TAG, "RSD: ignoring XPC response with non-dict body");
                        }
                    }
                }
            } else if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                sendFn(Http2Framer::MakeSettingsFrame(true));
            }
            if (gotXPCResponse) break;
        }
        if (offset > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + offset);
    }

    if (!gotXPCResponse) {
        INST_LOG_ERROR(TAG, "RSD: no XPC service response received");
        return Error::ProtocolError;
    }

    INST_LOG_INFO(TAG, "RSD handshake complete: UDID=%s, %zu services",
                 m_udid.c_str(), m_services.size());
    return Error::Success;
}

Error RSDProvider::ConnectDirect(const std::string& address, uint16_t rsdPort) {
    INST_LOG_INFO(TAG, "ConnectDirect: connecting to RSD at [%s]:%u", address.c_str(), rsdPort);

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", rsdPort);

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(address.c_str(), portStr, &hints, &res) != 0 || !res) {
        INST_LOG_ERROR(TAG, "ConnectDirect: getaddrinfo failed for [%s]:%u", address.c_str(), rsdPort);
        return Error::ConnectionFailed;
    }

    rsd_socket_t sock = RSD_SOCKET_INVALID;
    for (auto* rp = res; rp != nullptr; rp = rp->ai_next) {
        sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == RSD_SOCKET_INVALID) continue;
        if (::connect(sock, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) break;
        RSD_CLOSE_SOCKET(sock);
        sock = RSD_SOCKET_INVALID;
    }
    freeaddrinfo(res);

    if (sock == RSD_SOCKET_INVALID) {
        INST_LOG_ERROR(TAG, "ConnectDirect: TCP connect failed to [%s]:%u", address.c_str(), rsdPort);
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "ConnectDirect: TCP connected to RSD");

    // Prevent indefinite blocking in recv so DoRSDHandshake deadlines can fire.
#ifdef _WIN32
    {
        DWORD tmoMs = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tmoMs), sizeof(tmoMs));
    }
#else
    {
        struct timeval tmo = {0, 500000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    }
#endif

    auto sendFn = [&](const std::vector<uint8_t>& data) -> bool {
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            int n = ::send(sock, reinterpret_cast<const char*>(data.data() + sent),
                           static_cast<int>(data.size() - sent), 0);
#else
            ssize_t n = ::send(sock, data.data() + sent, data.size() - sent, 0);
#endif
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    auto recvMoreFn = [&](std::vector<uint8_t>& buf) -> bool {
        uint8_t tmp[4096];
#ifdef _WIN32
        int n = ::recv(sock, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
        if (n < 0) {
            int werr = WSAGetLastError();
            if (werr == WSAETIMEDOUT || werr == WSAEWOULDBLOCK) {
                return true; // no data yet, keep waiting until handshake deadline
            }
            return false;
        }
#else
        ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                return true; // no data yet, keep waiting until handshake deadline
            }
            return false;
        }
#endif
        if (n <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + n);
        return true;
    };

    Error err = DoRSDHandshake(sendFn, recvMoreFn);
    RSD_CLOSE_SOCKET(sock);

    if (err == Error::Success) {
        INST_LOG_INFO(TAG, "ConnectDirect: complete — UDID=%s, %zu services",
                     m_udid.c_str(), m_services.size());
    }
    return err;
}

Error RSDProvider::ConnectViaIDevice(idevice_t device, uint16_t rsdPort) {
    INST_LOG_INFO(TAG, "ConnectViaIDevice: connecting to RSD port %u via USB (usbmuxd)", rsdPort);

    idevice_connection_t conn = nullptr;
    idevice_error_t ierr = idevice_connect(device, rsdPort, &conn);
    if (ierr != IDEVICE_E_SUCCESS || !conn) {
        INST_LOG_ERROR(TAG, "ConnectViaIDevice: idevice_connect to port %u failed: %d", rsdPort, ierr);
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "ConnectViaIDevice: USB TCP connected to RSD port %u", rsdPort);

    auto sendFn = [&](const std::vector<uint8_t>& data) -> bool {
        const char* ptr = reinterpret_cast<const char*>(data.data());
        size_t remaining = data.size();
        while (remaining > 0) {
            uint32_t sent = 0;
            idevice_error_t e = idevice_connection_send(
                conn, ptr, static_cast<uint32_t>(remaining), &sent);
            if (e != IDEVICE_E_SUCCESS || sent == 0) return false;
            ptr += sent;
            remaining -= sent;
        }
        return true;
    };

    auto recvMoreFn = [&](std::vector<uint8_t>& buf) -> bool {
        char tmp[4096];
        uint32_t received = 0;
        idevice_error_t e = idevice_connection_receive_timeout(
            conn, tmp, sizeof(tmp), &received, 5000);
        if (e != IDEVICE_E_SUCCESS || received == 0) return false;
        buf.insert(buf.end(), tmp, tmp + received);
        return true;
    };

    Error err = DoRSDHandshake(sendFn, recvMoreFn);
    idevice_disconnect(conn);

    if (err == Error::Success) {
        INST_LOG_INFO(TAG, "ConnectViaIDevice: complete — UDID=%s, %zu services",
                     m_udid.c_str(), m_services.size());
    }
    return err;
}

Error RSDProvider::ConnectViaFd(int socketFd) {
    INST_LOG_INFO(TAG, "ConnectViaFd: doing RSD handshake on pre-connected socket");

    // Ensure recv() does not block forever, otherwise DoRSDHandshake can hang.
#ifdef _WIN32
    {
        DWORD tmoMs = 500;
        setsockopt(static_cast<SOCKET>(socketFd), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tmoMs), sizeof(tmoMs));
    }
#else
    {
        struct timeval tmo = {0, 500000};
        setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    }
#endif

    auto sendFn = [socketFd](const std::vector<uint8_t>& data) -> bool {
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            int n = ::send(static_cast<SOCKET>(socketFd),
                           reinterpret_cast<const char*>(data.data() + sent),
                           static_cast<int>(data.size() - sent), 0);
#else
            ssize_t n = ::send(socketFd, data.data() + sent, data.size() - sent, 0);
#endif
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    auto recvMoreFn = [socketFd](std::vector<uint8_t>& buf) -> bool {
        uint8_t tmp[4096];
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(socketFd),
                       reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
        if (n < 0) {
            int werr = WSAGetLastError();
            if (werr == WSAETIMEDOUT || werr == WSAEWOULDBLOCK) {
                return true; // no data yet, keep waiting until handshake deadline
            }
            return false;
        }
#else
        ssize_t n = ::recv(socketFd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                return true; // no data yet, keep waiting until handshake deadline
            }
            return false;
        }
#endif
        if (n <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + static_cast<size_t>(n));
        return true;
    };

    return DoRSDHandshake(sendFn, recvMoreFn);
}

void RSDProvider::ParseServiceResponse(const NSObject& body) {
    auto toLower = [](const std::string& s) -> std::string {
        std::string out = s;
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };

    auto findKeyCI = [&](const NSObject& dict, const char* key) -> const NSObject* {
        if (!dict.IsDict()) return nullptr;
        const auto& m = dict.AsDict();
        auto it = m.find(key);
        if (it != m.end()) return &it->second;
        const std::string want = toLower(key);
        for (const auto& kv : m) {
            if (toLower(kv.first) == want) return &kv.second;
        }
        return nullptr;
    };

    auto parsePort = [](const NSObject& portNode) -> uint16_t {
        if (portNode.IsUInt()) {
            return static_cast<uint16_t>(portNode.AsUInt64());
        }
        if (portNode.IsInt()) {
            int64_t v = portNode.AsInt64();
            return v > 0 ? static_cast<uint16_t>(v) : 0;
        }
        if (portNode.IsString()) {
            const std::string& s = portNode.AsString();
            char* end = nullptr;
            unsigned long v = std::strtoul(s.c_str(), &end, 10);
            if (end != s.c_str() && *end == '\0' && v <= 65535ul) {
                return static_cast<uint16_t>(v);
            }
        }
        return 0;
    };

    const NSObject* root = &body;
    if (const NSObject* value = findKeyCI(body, "value"); value && value->IsDict()) {
        root = value;
    }

    if (const NSObject* props = findKeyCI(*root, "Properties"); props && props->IsDict()) {
        if (const NSObject* udid = findKeyCI(*props, "UniqueDeviceID"); udid && udid->IsString()) {
            m_udid = udid->AsString();
            INST_LOG_INFO(TAG, "Device UDID: %s", m_udid.c_str());
        }
    }

    if (const NSObject* services = findKeyCI(*root, "Services"); services && services->IsDict()) {
        for (const auto& [name, value] : services->AsDict()) {
            if (!value.IsDict()) continue;
            RSDServiceEntry entry;
            entry.name = name;
            if (const NSObject* portNode = findKeyCI(value, "Port")) {
                entry.port = parsePort(*portNode);
            }
            if (entry.port > 0) {
                m_services[name] = entry;
                INST_LOG_DEBUG(TAG, "  Service: %s -> port %u", name.c_str(), entry.port);
            }
        }
    }
}

uint16_t RSDProvider::FindServicePort(const std::string& serviceName) const {
    auto it = m_services.find(serviceName);
    if (it != m_services.end()) {
        return it->second.port;
    }
    return 0;
}

uint16_t RSDProvider::FindServicePortWithShim(const std::string& serviceName) const {
    // First try exact match
    uint16_t port = FindServicePort(serviceName);
    if (port > 0) return port;

    // Try with .shim.remote suffix (iOS 17+ shim services)
    std::string shimName = serviceName + ".shim.remote";
    return FindServicePort(shimName);
}

} // namespace instruments


