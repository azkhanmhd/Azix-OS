#ifndef NET_H
#define NET_H

/*
 * net.h — Minimal network stack: ARP + IPv4 + ICMP ping
 *
 * VirtualBox NAT defaults (hardcoded):
 *   My IP   : 10.0.2.15   (0x0A00020F)
 *   Gateway : 10.0.2.2    (0x0A000202)
 *   Netmask : 255.255.255.0 (0xFFFFFF00)
 */

#include <stdint.h>

/* ---- Configuration defaults (host byte order) ---- */
#define NET_MY_IP    0x0A00020FUL   /* 10.0.2.15  */
#define NET_GATEWAY  0x0A000202UL   /* 10.0.2.2   */
#define NET_NETMASK  0xFFFFFF00UL   /* /24         */
#define NET_DNS      0x08080808UL   /* 8.8.8.8 — works through VirtualBox NAT */

/* Initialize stack with static IP config.
   Must be called after pcnet_init() succeeds. */
void     net_init(uint32_t my_ip, uint32_t gateway, uint32_t netmask);

/* Resolve a hostname to an IPv4 address via DNS (A record).
   Returns 1 and sets *ip_out (host byte order) on success, 0 on failure. */
int      net_dns_resolve(const char *hostname, uint32_t *ip_out);

/* Resolve IP → MAC via ARP.  Returns 1 on success, 0 on timeout (~1s). */
int      arp_lookup(uint32_t ip, uint8_t mac_out[6]);

/* Send one ICMP echo request and wait for reply.
   Returns RTT in ms (0 = sub-ms), -1 = timeout, -2 = NIC not ready. */
int      net_ping(uint32_t dst_ip);

/* Parse dotted-decimal "a.b.c.d" → host-byte-order uint32_t.
   Returns 0 on parse error. */
uint32_t parse_ip(const char *s);

/* Current IP configuration (host byte order, set by net_init) */
extern uint32_t my_ip_addr;
extern uint32_t my_gateway;
extern uint32_t my_netmask;

#endif /* NET_H */
