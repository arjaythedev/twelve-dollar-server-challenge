#ifndef CHALLENGE_LWIP_OPTIONS_H
#define CHALLENGE_LWIP_OPTIONS_H

#include <stddef.h>
#include <stdint.h>

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_TCPIP_CORE_LOCKING 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ETHERNET 1
#define LWIP_ARP 1
#define LWIP_ICMP 1
#define LWIP_TCP 1
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_DNS 0
#define LWIP_STATS 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_SINGLE_NETIF 1
#define LWIP_NOASSERT_ON_ERROR 1

/* The one issuer owns all protocol allocations. The port applies an aggregate
 * byte limit, including half-open connections, retransmissions and TIME_WAIT. */
void *stack_protocol_malloc(size_t size);
void *stack_protocol_calloc(size_t count, size_t size);
void stack_protocol_free(void *ptr);
uint32_t stack_random(void);
#define MEM_ALIGNMENT __SIZEOF_POINTER__
#define MEM_CUSTOM_ALLOCATOR 1
#define MEM_CUSTOM_MALLOC stack_protocol_malloc
#define MEM_CUSTOM_CALLOC stack_protocol_calloc
#define MEM_CUSTOM_FREE stack_protocol_free
#define MEMP_MEM_MALLOC 1
#define LWIP_RAND() stack_random()
#define LWIP_HOOK_TCP_ISN(local_ip, local_port, remote_ip, remote_port) stack_random()

#define TCP_MSS 1460
#define TCP_WND 32768
#define TCP_SND_BUF 16384
#define TCP_OVERSIZE 0
#define TCP_QUEUE_OOSEQ 1
#define TCP_OOSEQ_MAX_BYTES 16384
#define TCP_OOSEQ_MAX_PBUFS 16
#define LWIP_TCP_SACK_OUT 1
#define TCP_LISTEN_BACKLOG 0
#define ARP_TABLE_SIZE 128
#define ARP_QUEUEING 1
#define ARP_QUEUE_LEN 8
#define IP_REASSEMBLY 1
#define IP_REASS_MAX_PBUFS 128
#define IP_FRAG 1

#endif
