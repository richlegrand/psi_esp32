// ESP32 compatibility: Missing constants and struct field mappings
#pragma once

#include <lwip/arch.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/ip6.h>

// IP version constant - standard value
#ifndef IPVERSION
#define IPVERSION 4
#endif

// _ALIGN function for CMSG_ALIGN macro
#ifndef _ALIGN
#define _ALIGN(n) LWIP_MEM_ALIGN_SIZE(n)
#endif

// IPv6 address field compatibility - LWIP vs BSD naming
// LWIP defines: un.u32_addr[4], un.u8_addr[16], s6_addr (=un.u8_addr)
// BSD expects: s6_addr32[4], s6_addr16[8], s6_addr[16]
#ifndef s6_addr32
#define s6_addr32 un.u32_addr
#endif
#ifndef s6_addr16
#define s6_addr16 ((uint16_t*)un.u32_addr)
#endif

// IPv6 packet information structure - missing from ESP-IDF's LWIP
#ifndef _STRUCT_IN6_PKTINFO_DEFINED
#define _STRUCT_IN6_PKTINFO_DEFINED
struct in6_pktinfo {
    struct in6_addr ipi6_addr;    /* src/dst IPv6 address */
    unsigned int    ipi6_ifindex; /* send/recv interface index */
};
#endif

// Socket option constants - missing from ESP-IDF's LWIP
#ifndef IPV6_PKTINFO
#define IPV6_PKTINFO 50  /* Standard Linux value, safe for ESP32 */
#endif

#ifndef IP_HDRINCL
#define IP_HDRINCL 3     /* Standard Linux value - LWIP may not support this socket option */
#endif

// getnameinfo() flags - standard POSIX values, used by ESP-IDF examples
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST 1   /* Return numeric hostname instead of resolving */
#endif

#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV 2   /* Return numeric service instead of resolving */
#endif

// IPv6 address to string conversion function
// BSD uses ip6_sprintf(), we'll use standard inet_ntop()
#include <arpa/inet.h>
static inline const char* ip6_sprintf(const struct in6_addr *addr) {
    static char buf[INET6_ADDRSTRLEN];
    return inet_ntop(AF_INET6, addr, buf, sizeof(buf));
}

// IPv6 header field compatibility: LWIP vs BSD naming
// LWIP uses different field names and types than BSD
#ifdef ESP32_PORT

// Simple field name mappings for compatible types
#define ip6_flow _v_tc_fl
#define ip6_plen _plen  
#define ip6_nxt  _nexth
#define ip6_hlim _hoplim
#define ip6_src  src
#define ip6_dst  dest

// IPv6 address field compatibility - create wrapper macros for assignments
// LWIP uses ip6_addr_p_t while BSD uses struct in6_addr - need memcpy for conversion

#define SCTP_SET_IPV6_DST(ip6_hdr, in6_addr_val) \
    memcpy(&((ip6_hdr)->dest), &(in6_addr_val), sizeof(struct in6_addr))

#define SCTP_SET_IPV6_SRC(ip6_hdr, in6_addr_val) \
    memcpy(&((ip6_hdr)->src), &(in6_addr_val), sizeof(struct in6_addr))

#define SCTP_GET_IPV6_DST(in6_addr_var, ip6_hdr) \
    memcpy(&(in6_addr_var), &((ip6_hdr)->dest), sizeof(struct in6_addr))

#define SCTP_GET_IPV6_SRC(in6_addr_var, ip6_hdr) \
    memcpy(&(in6_addr_var), &((ip6_hdr)->src), sizeof(struct in6_addr))

#endif // ESP32_PORT