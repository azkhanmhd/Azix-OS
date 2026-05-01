/*
 * tcp.c — Minimal single-connection TCP client for Azix OS
 *
 * Supports: connect (3-way handshake), write, read-until-FIN, close.
 * Polling mode, no IRQ, no retransmission beyond the SYN phase.
 * Byte-order: same convention as net.c — IP addresses in host order,
 * packet fields written big-endian via be16()/put_ip().
 */

#include "tcp.h"
#include "net.h"
#include "pcnet.h"
#include "keyboard.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Packet layouts (matching net.c conventions)                         */
/* ================================================================== */
typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed)) tcp_eth_t;

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed)) tcp_iph_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_num;
    uint8_t  data_off;   /* upper 4 bits = header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

#define ETH_IP      0x0800
#define IP_PROTO_TCP 6

#define TCP_F_FIN  0x01
#define TCP_F_SYN  0x02
#define TCP_F_RST  0x04
#define TCP_F_PSH  0x08
#define TCP_F_ACK  0x10

#define TCP_SRC_PORT  49153
#define TCP_WINDOW    8192
#define TCP_MSS       1460

/* Large receive accumulation buffer (64 KB) */
#define TCP_RXBUF_SZ  65536

/* ================================================================== */
/* Connection state                                                    */
/* ================================================================== */
static uint32_t s_dst_ip;
static uint16_t s_dst_port;
static uint8_t  s_dst_mac[6];
static uint8_t  s_our_mac[6];
static uint32_t s_seq;          /* next seq we send */
static uint32_t s_ack;          /* next byte we expect from remote */
static int      s_established = 0;

static uint16_t s_ip_id = 0xAB00;

/* Shared RX frame buffer */
static uint8_t s_rx[1600];
/* Accumulation buffer for tcp_read() */

/* ================================================================== */
/* Helpers (mirror net.c, static so no conflicts)                      */
/* ================================================================== */
static inline uint16_t tcp_be16(uint16_t x) { return (uint16_t)((x>>8)|(x<<8)); }

static inline uint32_t tcp_be32(uint32_t x) {
    return ((x>>24)&0xFF)|((x>>8)&0xFF00)|((x<<8)&0xFF0000)|((x<<24)&0xFF000000u);
}

static void tcp_cpy(void *d, const void *s, uint32_t n)
{
    uint8_t *dd = (uint8_t*)d; const uint8_t *ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}

static void tcp_zero(void *d, uint32_t n)
{
    uint8_t *dd = (uint8_t*)d; while (n--) *dd++ = 0;
}

/* Write host-order IP as 4 network-order bytes */
static void tcp_put_ip(uint8_t *out, uint32_t ip)
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >>  8);
    out[3] = (uint8_t)(ip);
}

/* Read 4 network-order bytes → host-order IP */
static uint32_t tcp_get_ip(const uint8_t *b)
{
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|(uint32_t)b[3];
}

/* Internet checksum */
static uint16_t tcp_cksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += ((uint32_t)p[0]<<8)|p[1]; p += 2; len -= 2; }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* TCP checksum over pseudo-header + TCP segment */
static uint16_t tcp_seg_cksum(uint32_t src_ip, uint32_t dst_ip,
                               const uint8_t *seg, uint32_t seg_len)
{
    /* pseudo-header: src(4) dst(4) zero(1) proto(1) tcp_len(2) */
    uint8_t pseudo[12];
    tcp_put_ip(pseudo,   src_ip);
    tcp_put_ip(pseudo+4, dst_ip);
    pseudo[8]  = 0;
    pseudo[9]  = IP_PROTO_TCP;
    pseudo[10] = (uint8_t)(seg_len >> 8);
    pseudo[11] = (uint8_t)(seg_len);

    /* Sum pseudo-header */
    uint32_t sum = 0;
    for (int i = 0; i < 6; i++)
        sum += ((uint32_t)pseudo[i*2]<<8) | pseudo[i*2+1];

    /* Sum TCP segment */
    const uint8_t *p = seg;
    uint32_t len = seg_len;
    while (len > 1) { sum += ((uint32_t)p[0]<<8)|p[1]; p+=2; len-=2; }
    if (len) sum += (uint32_t)p[0]<<8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ================================================================== */
/* Send one TCP segment                                                */
/* ================================================================== */
static uint8_t s_tx[14 + 20 + 24 + TCP_MSS];  /* eth+ip+tcp(w/opts)+data */

static void tcp_send_seg(uint8_t flags, const uint8_t *data, uint32_t dlen)
{
    /* SYN gets an MSS option (4 bytes), padding to 4-byte boundary */
    uint32_t tcp_hdr_bytes = (flags & TCP_F_SYN) ? 24 : 20;
    uint32_t ip_total      = 20 + tcp_hdr_bytes + dlen;
    uint32_t frame_len     = 14 + ip_total;

    if (frame_len > sizeof(s_tx)) return;  /* safety */
    tcp_zero(s_tx, frame_len);

    /* Ethernet */
    tcp_eth_t *eth = (tcp_eth_t*)s_tx;
    tcp_cpy(eth->dst, s_dst_mac, 6);
    tcp_cpy(eth->src, s_our_mac, 6);
    eth->type = tcp_be16(ETH_IP);

    /* IP */
    tcp_iph_t *ip = (tcp_iph_t*)(s_tx + 14);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = tcp_be16((uint16_t)ip_total);
    ip->id         = tcp_be16(s_ip_id++);
    ip->flags_frag = tcp_be16(0x4000);  /* DF */
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_TCP;
    ip->checksum   = 0;
    tcp_put_ip(ip->src_ip, my_ip_addr);
    tcp_put_ip(ip->dst_ip, s_dst_ip);
    ip->checksum   = tcp_be16(tcp_cksum(ip, 20));

    /* TCP */
    tcp_hdr_t *tcp = (tcp_hdr_t*)(s_tx + 34);
    tcp->src_port  = tcp_be16(TCP_SRC_PORT);
    tcp->dst_port  = tcp_be16(s_dst_port);
    tcp->seq       = tcp_be32(s_seq);
    tcp->ack_num   = (flags & TCP_F_ACK) ? tcp_be32(s_ack) : 0;
    tcp->data_off  = (uint8_t)((tcp_hdr_bytes / 4) << 4);
    tcp->flags     = flags;
    tcp->window    = tcp_be16(TCP_WINDOW);
    tcp->checksum  = 0;
    tcp->urgent    = 0;

    /* MSS option on SYN: kind=2, len=4, mss=1460 (big-endian) */
    if (flags & TCP_F_SYN) {
        uint8_t *opt = s_tx + 34 + 20;
        opt[0] = 2;             /* kind: MSS */
        opt[1] = 4;             /* length */
        opt[2] = (TCP_MSS >> 8) & 0xFF;
        opt[3] = (TCP_MSS)      & 0xFF;
    }

    if (data && dlen > 0)
        tcp_cpy(s_tx + 34 + tcp_hdr_bytes, data, dlen);

    /* TCP checksum */
    tcp->checksum = tcp_be16(
        tcp_seg_cksum(my_ip_addr, s_dst_ip,
                      (uint8_t*)tcp, tcp_hdr_bytes + dlen));

    pcnet_send(s_tx, (uint16_t)frame_len);
}

/* ================================================================== */
/* Receive one TCP packet from our connection                          */
/* Returns 1 if a valid TCP segment was received, 0 otherwise.        */
/* ================================================================== */
static int tcp_poll(tcp_hdr_t **out_hdr, uint8_t **out_data, uint32_t *out_dlen)
{
    int len = pcnet_poll(s_rx);
    if (len < (int)(14 + 20 + 20)) return 0;

    tcp_eth_t *eth = (tcp_eth_t*)s_rx;
    if (tcp_be16(eth->type) != ETH_IP) return 0;

    tcp_iph_t *ip = (tcp_iph_t*)(s_rx + 14);
    if (ip->proto != IP_PROTO_TCP) return 0;
    /* Do NOT filter by src_ip — VirtualBox NAT may relay SYN-ACK
       from its gateway IP (10.0.2.2) rather than the destination IP. */
    if (tcp_get_ip(ip->dst_ip) != my_ip_addr) return 0;

    uint32_t ip_hlen = (uint32_t)((ip->ver_ihl & 0xF) * 4);
    tcp_hdr_t *tcp = (tcp_hdr_t*)(s_rx + 14 + ip_hlen);

    if (tcp_be16(tcp->dst_port) != TCP_SRC_PORT) return 0;
    if (tcp_be16(tcp->src_port) != s_dst_port)   return 0;

    uint32_t tcp_hlen = (uint32_t)(((tcp->data_off >> 4) & 0xF) * 4);
    uint32_t ip_total = (uint32_t)tcp_be16(ip->total_len);
    uint32_t tcp_total = (ip_total > ip_hlen) ? (ip_total - ip_hlen) : 0;
    uint32_t dlen = (tcp_total > tcp_hlen) ? (tcp_total - tcp_hlen) : 0;

    *out_hdr  = tcp;
    *out_data = (uint8_t*)tcp + tcp_hlen;
    *out_dlen = dlen;
    return 1;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

int tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    if (!pcnet_ready || !my_ip_addr) return TCP_ERROR;

    s_established = 0;
    s_dst_ip   = dst_ip;
    s_dst_port = dst_port;

    /* Resolve nexthop MAC */
    uint32_t nexthop = ((dst_ip & my_netmask) == (my_ip_addr & my_netmask))
                       ? dst_ip : my_gateway;
    if (!arp_lookup(nexthop, s_dst_mac)) return TCP_ERROR;
    pcnet_get_mac(s_our_mac);

    /* Starting sequence number */
    s_seq = 0xDEAD0000;
    s_ack = 0;

    /* SYN */
    tcp_send_seg(TCP_F_SYN, NULL, 0);
    s_seq++;    /* SYN consumes 1 sequence number */

    /* Wait for SYN-ACK — ~5 second timeout (5,000,000 × 1µs) */
    for (int i = 0; i < 5000000; i++) {
        if (keyboard_ctrl_c_flag) return TCP_CANCEL;

        tcp_hdr_t *hdr; uint8_t *dat; uint32_t dlen;
        if (tcp_poll(&hdr, &dat, &dlen)) {
            if (hdr->flags & TCP_F_RST) return TCP_RESET;
            if ((hdr->flags & (TCP_F_SYN|TCP_F_ACK)) == (TCP_F_SYN|TCP_F_ACK)) {
                s_ack = tcp_be32(hdr->seq) + 1;  /* SYN-ACK seq + 1 */
                tcp_send_seg(TCP_F_ACK, NULL, 0);
                s_established = 1;
                return TCP_OK;
            }
        }
        inb(0x80);  /* ~1µs delay */
    }
    return TCP_TIMEOUT;
}

int tcp_write(const uint8_t *data, uint32_t len)
{
    if (!s_established) return TCP_ERROR;

    uint32_t sent = 0;
    while (sent < len) {
        if (keyboard_ctrl_c_flag) return TCP_CANCEL;

        uint32_t chunk = len - sent;
        if (chunk > (uint32_t)TCP_MSS) chunk = (uint32_t)TCP_MSS;

        tcp_send_seg(TCP_F_ACK | TCP_F_PSH, data + sent, chunk);
        s_seq += chunk;
        sent  += chunk;

        /* Brief pause to avoid flooding the NIC */
        for (volatile int d = 0; d < 5000; d++);
    }
    return TCP_OK;
}

int tcp_read(uint8_t *buf, uint32_t buflen)
{
    uint32_t total = 0;
    int      idle  = 0;

    /* Poll until FIN, RST, timeout, or Ctrl+C */
    for (int i = 0; i < 3000000; i++) {
        if (keyboard_ctrl_c_flag) return TCP_CANCEL;

        tcp_hdr_t *hdr; uint8_t *dat; uint32_t dlen;
        if (tcp_poll(&hdr, &dat, &dlen)) {
            idle = 0;

            if (hdr->flags & TCP_F_RST) break;

            if (dlen > 0) {
                /* Update ACK and send ACK */
                s_ack = tcp_be32(hdr->seq) + dlen;
                tcp_send_seg(TCP_F_ACK, NULL, 0);

                /* Accumulate into caller's buffer */
                uint32_t space = (buflen > total) ? (buflen - total) : 0;
                uint32_t copy  = (dlen < space) ? dlen : space;
                if (copy > 0) { tcp_cpy(buf + total, dat, copy); total += copy; }
            }

            if (hdr->flags & TCP_F_FIN) {
                /* ACK the FIN */
                s_ack = tcp_be32(hdr->seq) + dlen + 1;
                tcp_send_seg(TCP_F_ACK, NULL, 0);
                break;
            }
        } else {
            idle++;
            inb(0x80);
            /* If we have data and no new packets for ~300 ms, assume done */
            if (total > 0 && idle > 300000) break;
        }
    }

    return (int)total;
}

void tcp_close(void)
{
    if (!s_established) return;
    tcp_send_seg(TCP_F_FIN | TCP_F_ACK, NULL, 0);
    s_seq++;
    s_established = 0;

    /* Wait briefly for ACK of our FIN */
    for (int i = 0; i < 50000; i++) {
        tcp_hdr_t *hdr; uint8_t *dat; uint32_t dlen;
        if (tcp_poll(&hdr, &dat, &dlen)) {
            if (hdr->flags & TCP_F_ACK) break;
        }
        inb(0x80);
    }
}
