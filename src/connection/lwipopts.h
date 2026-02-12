#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// lwIP configuration for iOS tunnel userspace networking
// This runs lwIP in NO_SYS mode (bare-metal, no OS thread support).
// We drive the stack manually via Poll() calls from the QUIC forwarding thread.

// System
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_TIMERS                 1

// IPv6 is required for Apple tunnel
#define LWIP_IPV4                   1
#define LWIP_IPV6                   1
#define LWIP_IPV6_NUM_ADDRESSES     3

// Protocol support
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_RAW                    0
#define LWIP_ICMP                   0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_ARP                    0

// We use raw API, no socket/netconn (requires NO_SYS=0)
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_NETIF_API              0

// Memory configuration
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_NETBUF             4
#define MEMP_NUM_NETCONN            0

// Pbuf configuration
#define PBUF_POOL_SIZE              24
#define PBUF_POOL_BUFSIZE           1536

// TCP configuration (tuned for QUIC tunnel, MTU 1280)
#define TCP_MSS                     1220
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_QUEUE_OOSEQ             1
#define LWIP_TCP_SACK_OUT           1

// Single network interface (our tunnel)
#define LWIP_SINGLE_NETIF           1
#define LWIP_NETIF_HOSTNAME         0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0

// Checksum - we compute our own for the tunnel interface
#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_ICMP           0
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_ICMP         0

// Disable features we don't need
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define PPP_SUPPORT                 0
#define LWIP_HAVE_LOOPIF            0

// Debug (disable in release)
#define LWIP_DEBUG                  0

// Platform-specific random
#ifdef _WIN32
#define LWIP_RAND()                 rand()
#else
#define LWIP_RAND()                 ((u32_t)random())
#endif

#endif // LWIPOPTS_H
