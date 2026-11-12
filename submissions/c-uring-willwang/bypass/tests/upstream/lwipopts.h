#ifndef TEST_PORT_OPTS_H
#define TEST_PORT_OPTS_H
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_TCPIP_CORE_LOCKING 0
#define LWIP_IPV6 0
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_IGMP 0
#define LWIP_DHCP 0
#define LWIP_DNS 0
#define LWIP_AUTOIP 0
#define LWIP_ARP 1
#define LWIP_ICMP 1
#define MEM_ALIGNMENT __SIZEOF_POINTER__
#define LWIP_TESTMODE 1
#define LWIP_CHECKSUM_ON_COPY 1
#define TCP_CHECKSUM_ON_COPY_SANITY_CHECK 1
#define TCP_CHECKSUM_ON_COPY_SANITY_CHECK_FAIL(msg) LWIP_ASSERT("checksum",0)
#define MEM_SIZE 17000
#define TCP_SND_QUEUELEN 40
#define MEMP_NUM_TCP_SEG TCP_SND_QUEUELEN
#define TCP_SND_BUF (12*TCP_MSS)
#define TCP_WND (10*TCP_MSS)
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 0
#define PBUF_POOL_SIZE 400
#define LWIP_STATS 1
unsigned int lwip_port_rand(void);
#define LWIP_RAND() lwip_port_rand()
#endif
