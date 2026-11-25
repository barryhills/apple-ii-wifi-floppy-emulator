// lwIP options for a2drive: NO_SYS (cyw43_arch threadsafe_background),
// IPv4 + link-local IPv6, DHCP, mDNS responder, small raw-API HTTP server.
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0

#define MEM_LIBC_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (24 * 1024)   // tcp_write(COPY) data lives here
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_ARP_QUEUE              10
#define PBUF_POOL_SIZE                  24

#define LWIP_IPV4                       1
// IPv6 is on only so mDNS can answer AAAA queries. lwIP's responder cannot
// send the NSEC "no AAAA" reply, and macOS resolvers then wait 5 s per lookup.
#define LWIP_IPV6                       1
#define LWIP_IPV6_MLD                   1     // required by mDNS over IPv6
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_IPV6_DHCP6                 0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define DHCP_DOES_ARP_CHECK             0
#define LWIP_DHCP_DOES_ACD_CHECK        0

#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE              1

#define LWIP_NETIF_HOSTNAME             1     // DHCP option 12
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  1     // mDNS restarts on link/IP change
#define LWIP_NETIF_TX_SINGLE_PBUF       1     // required by the cyw43 driver
#define LWIP_CHKSUM_ALGORITHM           3

// mDNS (apps/mdns): needs IGMP, a netif client-data slot, extra timeouts.
// LWIP_RAND is supplied by the SDK's arch/cc.h (get_rand_32).
#define LWIP_IGMP                       1
#define LWIP_MDNS_RESPONDER             1
#define LWIP_MDNS_SEARCH                0
#define MDNS_MAX_SERVICES               1
#define LWIP_NUM_NETIF_CLIENT_DATA      1
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

// SNTP (upload timestamps). DHCP, DNS, mDNS and SNTP each use a UDP PCB.
#include <stdint.h>
void clock_set_unix(uint32_t sec);
#define SNTP_SERVER_DNS                 1
#define SNTP_SET_SYSTEM_TIME(sec)       clock_set_unix((uint32_t)(sec))
#define MEMP_NUM_UDP_PCB                6

#define MEM_STATS                       0
#define SYS_STATS                       0
#define MEMP_STATS                      0
#define LINK_STATS                      0

#endif
