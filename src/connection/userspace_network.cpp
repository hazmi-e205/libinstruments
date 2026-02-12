#include "userspace_network.h"
#include "../util/log.h"

#ifdef INSTRUMENTS_HAS_QUIC

extern "C" {
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/tcp.h>
#include <lwip/timeouts.h>
#include <lwip/pbuf.h>
#include <lwip/ip6_addr.h>
#include <lwip/ip_addr.h>
#include <lwip/ip6.h>
}

#include <cstring>

namespace instruments {

static const char* TAG = "UserspaceNetwork";

// ---------- UserspaceTcpConnection ----------

UserspaceTcpConnection::~UserspaceTcpConnection() {
    Close();
}

Error UserspaceTcpConnection::Send(const uint8_t* data, size_t length) {
    if (!m_connected.load() || !m_pcb) {
        return Error::ConnectionFailed;
    }

    err_t err = tcp_write(m_pcb, data, static_cast<uint16_t>(length), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        INST_LOG_ERROR(TAG, "tcp_write failed: %d", err);
        return Error::InternalError;
    }

    tcp_output(m_pcb);
    return Error::Success;
}

void UserspaceTcpConnection::Close() {
    if (m_pcb) {
        tcp_arg(m_pcb, nullptr);
        tcp_recv(m_pcb, nullptr);
        tcp_sent(m_pcb, nullptr);
        tcp_err(m_pcb, nullptr);
        tcp_close(m_pcb);
        m_pcb = nullptr;
    }
    m_connected.store(false);
}

err_t UserspaceTcpConnection::OnConnected(void* arg, tcp_pcb* tpcb, err_t err) {
    auto* conn = static_cast<UserspaceTcpConnection*>(arg);
    if (!conn) return ERR_VAL;

    if (err != ERR_OK) {
        INST_LOG_ERROR(TAG, "TCP connect failed: %d", err);
        conn->m_connected.store(false);
        if (conn->m_errorCb) conn->m_errorCb(Error::ConnectionFailed);
        return ERR_OK;
    }

    INST_LOG_INFO(TAG, "TCP connection established");
    conn->m_connected.store(true);
    return ERR_OK;
}

err_t UserspaceTcpConnection::OnRecv(void* arg, tcp_pcb* tpcb, struct pbuf* p, err_t err) {
    auto* conn = static_cast<UserspaceTcpConnection*>(arg);
    if (!conn) {
        if (p) pbuf_free(p);
        return ERR_VAL;
    }

    if (!p) {
        // Connection closed by remote
        INST_LOG_DEBUG(TAG, "TCP connection closed by remote");
        conn->m_connected.store(false);
        if (conn->m_errorCb) conn->m_errorCb(Error::ConnectionFailed);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    // Deliver data to application
    if (conn->m_recvCb) {
        // Walk the pbuf chain
        struct pbuf* q = p;
        while (q) {
            conn->m_recvCb(static_cast<const uint8_t*>(q->payload), q->len);
            q = q->next;
        }
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

err_t UserspaceTcpConnection::OnSent(void* arg, tcp_pcb* tpcb, u16_t len) {
    // Data acknowledged by remote - could notify application if needed
    (void)arg;
    (void)tpcb;
    (void)len;
    return ERR_OK;
}

void UserspaceTcpConnection::OnError(void* arg, err_t err) {
    auto* conn = static_cast<UserspaceTcpConnection*>(arg);
    if (!conn) return;

    INST_LOG_ERROR(TAG, "TCP error: %d", err);
    conn->m_pcb = nullptr; // pcb is already freed by lwIP on error
    conn->m_connected.store(false);
    if (conn->m_errorCb) conn->m_errorCb(Error::ConnectionFailed);
}

// ---------- UserspaceNetwork ----------

UserspaceNetwork::UserspaceNetwork() = default;

UserspaceNetwork::~UserspaceNetwork() {
    Shutdown();
}

Error UserspaceNetwork::Init(const std::string& localIPv6,
                             const std::string& gatewayIPv6,
                             uint32_t mtu) {
    if (m_initialized.load()) {
        INST_LOG_WARN(TAG, "Already initialized");
        return Error::Success;
    }

    INST_LOG_INFO(TAG, "Initializing userspace network: local=%s gateway=%s mtu=%u",
                 localIPv6.c_str(), gatewayIPv6.c_str(), mtu);

    // Initialize lwIP
    lwip_init();

    // Allocate and configure network interface
    m_netif = new struct netif();
    std::memset(m_netif, 0, sizeof(struct netif));

    // Parse IPv6 addresses
    ip6_addr_t local6, gateway6;
    if (!ip6addr_aton(localIPv6.c_str(), &local6)) {
        INST_LOG_ERROR(TAG, "Invalid local IPv6 address: %s", localIPv6.c_str());
        delete m_netif;
        m_netif = nullptr;
        return Error::InvalidArgument;
    }
    if (!ip6addr_aton(gatewayIPv6.c_str(), &gateway6)) {
        INST_LOG_ERROR(TAG, "Invalid gateway IPv6 address: %s", gatewayIPv6.c_str());
        delete m_netif;
        m_netif = nullptr;
        return Error::InvalidArgument;
    }

    // Add the network interface - for IPv6-only, we pass NULL for IPv4 params
    if (!netif_add_noaddr(m_netif, this, NetifInit, netif_input)) {
        INST_LOG_ERROR(TAG, "Failed to add network interface");
        delete m_netif;
        m_netif = nullptr;
        return Error::InternalError;
    }

    // Assign IPv6 address
    ip_addr_t localAddr;
    ip_addr_copy_from_ip6(localAddr, local6);
    netif_ip6_addr_set(m_netif, 0, ip_2_ip6(&localAddr));
    netif_ip6_addr_set_state(m_netif, 0, IP6_ADDR_VALID);

    // Set as default interface
    netif_set_default(m_netif);
    netif_set_up(m_netif);
    netif_set_link_up(m_netif);

    // Configure MTU
    m_netif->mtu = static_cast<uint16_t>(mtu);

    m_initialized.store(true);
    INST_LOG_INFO(TAG, "Userspace network initialized");
    return Error::Success;
}

void UserspaceNetwork::InjectPacket(const uint8_t* data, size_t len) {
    if (!m_initialized.load() || !m_netif) return;

    // Allocate a pbuf and copy the incoming packet data
    struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<uint16_t>(len), PBUF_POOL);
    if (!p) {
        INST_LOG_WARN(TAG, "Failed to allocate pbuf for incoming packet (%zu bytes)", len);
        return;
    }

    // Copy data into pbuf chain
    pbuf_take(p, data, static_cast<uint16_t>(len));

    // Feed into lwIP via the network interface input function
    if (m_netif->input(p, m_netif) != ERR_OK) {
        INST_LOG_WARN(TAG, "netif input rejected packet");
        pbuf_free(p);
    }
}

Error UserspaceNetwork::TcpConnect(const std::string& destIPv6, uint16_t port,
                                    std::shared_ptr<UserspaceTcpConnection>& out) {
    if (!m_initialized.load()) {
        return Error::InternalError;
    }

    // Parse destination address
    ip6_addr_t dest6;
    if (!ip6addr_aton(destIPv6.c_str(), &dest6)) {
        INST_LOG_ERROR(TAG, "Invalid destination IPv6: %s", destIPv6.c_str());
        return Error::InvalidArgument;
    }

    ip_addr_t destAddr;
    ip_addr_copy_from_ip6(destAddr, dest6);

    // Create TCP PCB for IPv6
    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V6);
    if (!pcb) {
        INST_LOG_ERROR(TAG, "Failed to create TCP PCB");
        return Error::InternalError;
    }

    // Create connection object
    auto conn = std::shared_ptr<UserspaceTcpConnection>(new UserspaceTcpConnection());
    conn->m_pcb = pcb;

    // Set up lwIP callbacks
    tcp_arg(pcb, conn.get());
    tcp_recv(pcb, UserspaceTcpConnection::OnRecv);
    tcp_sent(pcb, UserspaceTcpConnection::OnSent);
    tcp_err(pcb, UserspaceTcpConnection::OnError);

    INST_LOG_INFO(TAG, "TCP connecting to [%s]:%u", destIPv6.c_str(), port);

    // Initiate connection
    err_t err = tcp_connect(pcb, &destAddr, port, UserspaceTcpConnection::OnConnected);
    if (err != ERR_OK) {
        INST_LOG_ERROR(TAG, "tcp_connect failed: %d", err);
        tcp_close(pcb);
        return Error::ConnectionFailed;
    }

    // Store connection reference
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        m_connections.push_back(conn);
    }

    out = conn;
    return Error::Success;
}

void UserspaceNetwork::Poll() {
    if (!m_initialized.load()) return;

    // Process lwIP timers (TCP retransmits, keepalives, etc.)
    sys_check_timeouts();
}

void UserspaceNetwork::Shutdown() {
    if (!m_initialized.exchange(false)) return;

    INST_LOG_INFO(TAG, "Shutting down userspace network");

    // Close all connections
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        for (auto& conn : m_connections) {
            conn->Close();
        }
        m_connections.clear();
    }

    // Remove and free network interface
    if (m_netif) {
        netif_set_down(m_netif);
        netif_remove(m_netif);
        delete m_netif;
        m_netif = nullptr;
    }
}

// lwIP netif initialization callback
err_t UserspaceNetwork::NetifInit(struct netif* netif) {
    netif->name[0] = 't';  // tunnel
    netif->name[1] = 'n';
    netif->output_ip6 = UserspaceNetwork::NetifOutput;
    netif->mtu = 1280;
    netif->flags = NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

// lwIP netif output callback - called when lwIP has an IPv6 packet to send
err_t UserspaceNetwork::NetifOutput(struct netif* netif, struct pbuf* p,
                                     const ip6_addr_t* ipaddr) {
    (void)ipaddr;

    auto* self = static_cast<UserspaceNetwork*>(netif->state);
    if (!self || !self->m_outputCb) {
        return ERR_IF;
    }

    // Linearize the pbuf chain into a contiguous buffer
    if (p->next == nullptr) {
        // Single pbuf - send directly
        self->m_outputCb(static_cast<const uint8_t*>(p->payload), p->tot_len);
    } else {
        // Chain - need to copy into contiguous buffer
        std::vector<uint8_t> buf(p->tot_len);
        pbuf_copy_partial(p, buf.data(), p->tot_len, 0);
        self->m_outputCb(buf.data(), buf.size());
    }

    return ERR_OK;
}

} // namespace instruments

#else // !INSTRUMENTS_HAS_QUIC

namespace instruments {

UserspaceTcpConnection::~UserspaceTcpConnection() {}
Error UserspaceTcpConnection::Send(const uint8_t*, size_t) { return Error::NotSupported; }
void UserspaceTcpConnection::Close() {}

UserspaceNetwork::UserspaceNetwork() = default;
UserspaceNetwork::~UserspaceNetwork() = default;

Error UserspaceNetwork::Init(const std::string&, const std::string&, uint32_t) {
    return Error::NotSupported;
}
void UserspaceNetwork::InjectPacket(const uint8_t*, size_t) {}
Error UserspaceNetwork::TcpConnect(const std::string&, uint16_t,
                                    std::shared_ptr<UserspaceTcpConnection>&) {
    return Error::NotSupported;
}
void UserspaceNetwork::Poll() {}
void UserspaceNetwork::Shutdown() {}

} // namespace instruments

#endif // INSTRUMENTS_HAS_QUIC
