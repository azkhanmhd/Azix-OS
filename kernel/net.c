/*
 * net.c — Minimal network stack for Azix OS
 *
 * Layers implemented (polling, no IRQ):
 *   Ethernet II framing
 *   ARP (IPv4 over Ethernet) — send request, receive reply, 1-entry cache
 *   IPv4 — TX only (build header + checksum)
 *   ICMP — echo request / echo reply (ping)
 *
 * Byte-order note
 * ---------------
 * All IP/Ethernet fields on the wire are big-endian (network byte order).
 * x86 is little-endian.  We use be16()/be32() which are their own inverse:
 *   be16(x) swaps the two bytes of a uint16_t.
 * When writing a field:  field = be16(host_value)
 * When reading a field:  host_value = be16(field)
 * inet_cksum() reads bytes as big-endian pairs; store result via be16().
 */

#include "net.h"
#include "pcnet.h"
#include "keyboard.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Protocol constants                                                  */
/* ================================================================== */
#define ETH_ARP        0x0806
#define ETH_IP         0x0800
#define IP_PROTO_ICMP  0x01
#define IP_PROTO_UDP   0x11
#define ICMP_ECHO_REQ  8
#define ICMP_ECHO_REP  0
#define ARP_OP_REQ     1
#define ARP_OP_REPLY   2
#define DNS_PORT       53
#define DNS_SRC_PORT   54321

/* ================================================================== */
/* Packed packet layouts                                               */
/* ================================================================== */
typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;          /* big-endian */
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t hw_type;       /* big-endian, 0x0001 = Ethernet */
    uint16_t proto;         /* big-endian, 0x0800 = IPv4     */
    uint8_t  hw_len;        /* 6 */
    uint8_t  proto_len;     /* 4 */
    uint16_t op;            /* big-endian, 1=req 2=reply     */
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];  /* big-endian bytes              */
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];  /* big-endian bytes              */
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t  ver_ihl;       /* 0x45 = IPv4, 20-byte header   */
    uint8_t  tos;
    uint16_t total_len;     /* big-endian                    */
    uint16_t id;            /* big-endian                    */
    uint16_t flags_frag;    /* big-endian                    */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;      /* big-endian; 0 when computing  */
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint16_t src_port;  /* big-endian */
    uint16_t dst_port;  /* big-endian */
    uint16_t length;    /* big-endian, header + payload */
    uint16_t checksum;  /* 0 = disabled (valid for IPv4 UDP) */
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;      /* big-endian */
    uint16_t id;            /* big-endian */
    uint16_t seq;           /* big-endian */
    uint8_t  data[8];       /* "AZIXPING" */
} __attribute__((packed)) icmp_hdr_t;

/* ================================================================== */
/* State                                                               */
/* ================================================================== */
uint32_t my_ip_addr = 0;   /* host byte order */
uint32_t my_gateway = 0;
uint32_t my_netmask = 0;

static uint8_t  my_mac[6];

/* 1-entry ARP cache */
static uint32_t arp_cached_ip  = 0;
static uint8_t  arp_cached_mac[6];
static int      arp_valid = 0;

/* Shared receive buffer — large enough for any Ethernet frame */
static uint8_t rx_buf[1536];

/* Running counters for IP ID and ICMP sequence */
static uint16_t g_ip_id  = 1;
static uint16_t g_ping_seq = 1;

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */
static void m_cpy(void *d, const void *s, int n)
{
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}
static void m_zero(void *d, int n)
{
    uint8_t *dd = d; while (n--) *dd++ = 0;
}

/* Approx 1µs per call (I/O port read takes ~1µs on real HW) */
static void udelay_us(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++) inb(0x80);
}

/* Byte-swap 16-bit value (htons / ntohs — same operation) */
static inline uint16_t be16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

/* Write 32-bit host-order IP as 4 network-order bytes */
static void put_ip(uint8_t *out, uint32_t ip)
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >>  8);
    out[3] = (uint8_t)(ip);
}

/* Read 4 network-order bytes → 32-bit host-order IP */
static uint32_t get_ip(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
}

/*
 * Internet checksum (RFC 1071).
 * Treats data as a sequence of 16-bit big-endian words, sums them,
 * folds carry, returns one's complement.
 * Store result with be16(): field = be16(inet_cksum(hdr, len));
 */
static uint16_t inet_cksum(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void mac_broadcast(uint8_t *m) { m[0]=m[1]=m[2]=m[3]=m[4]=m[5]=0xFF; }

/* ================================================================== */
/* net_init                                                            */
/* ================================================================== */
void net_init(uint32_t ip, uint32_t gw, uint32_t nm)
{
    my_ip_addr = ip;
    my_gateway = gw;
    my_netmask = nm;
    pcnet_get_mac(my_mac);
}

/* ================================================================== */
/* parse_ip — "10.0.2.2" → 0x0A000202 (host byte order)              */
/* ================================================================== */
uint32_t parse_ip(const char *s)
{
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t n = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (uint32_t)(*s - '0'); s++; }
        if (n > 255) return 0;
        ip = (ip << 8) | n;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    return ip;
}

/* ================================================================== */
/* ARP                                                                 */
/* ================================================================== */
static void arp_send_request(uint32_t target_ip)
{
    static uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    m_zero(frame, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    mac_broadcast(eth->dst);
    m_cpy(eth->src, my_mac, 6);
    eth->type = be16(ETH_ARP);

    arp_pkt_t *a = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    a->hw_type   = be16(1);
    a->proto     = be16(ETH_IP);
    a->hw_len    = 6;
    a->proto_len = 4;
    a->op        = be16(ARP_OP_REQ);
    m_cpy(a->sender_mac, my_mac, 6);
    put_ip(a->sender_ip, my_ip_addr);
    m_zero(a->target_mac, 6);
    put_ip(a->target_ip, target_ip);

    pcnet_send(frame, (uint16_t)sizeof(frame));
}

/* Process one ARP packet.  Caches sender.  Replies if someone asks for us. */
static void arp_process(const uint8_t *frame, uint16_t len)
{
    if (len < (uint16_t)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) return;
    const arp_pkt_t *a = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));

    uint16_t op         = be16(a->op);
    uint32_t sender_ip  = get_ip(a->sender_ip);

    /* Cache whoever is talking to us */
    if (op == ARP_OP_REPLY || op == ARP_OP_REQ) {
        arp_cached_ip = sender_ip;
        m_cpy(arp_cached_mac, a->sender_mac, 6);
        arp_valid = 1;
    }

    /* If someone asks for our IP, send a gratuitous reply */
    if (op == ARP_OP_REQ && get_ip(a->target_ip) == my_ip_addr) {
        static uint8_t reply[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
        m_zero(reply, sizeof(reply));

        eth_hdr_t *reth = (eth_hdr_t *)reply;
        m_cpy(reth->dst, a->sender_mac, 6);
        m_cpy(reth->src, my_mac, 6);
        reth->type = be16(ETH_ARP);

        arp_pkt_t *r = (arp_pkt_t *)(reply + sizeof(eth_hdr_t));
        r->hw_type   = be16(1);
        r->proto     = be16(ETH_IP);
        r->hw_len    = 6;
        r->proto_len = 4;
        r->op        = be16(ARP_OP_REPLY);
        m_cpy(r->sender_mac, my_mac, 6);
        put_ip(r->sender_ip, my_ip_addr);
        m_cpy(r->target_mac, a->sender_mac, 6);
        m_cpy(r->target_ip, a->sender_ip, 4);

        pcnet_send(reply, (uint16_t)sizeof(reply));
    }
}

int arp_lookup(uint32_t ip, uint8_t mac_out[6])
{
    /* Cache hit? */
    if (arp_valid && arp_cached_ip == ip) {
        m_cpy(mac_out, arp_cached_mac, 6);
        return 1;
    }

    arp_send_request(ip);

    /* Poll for ARP reply — timeout ~1 second (100000 × 10µs) */
    for (uint32_t i = 0; i < 100000; i++) {
        uint16_t len = pcnet_poll(rx_buf);
        if (len >= 14) {
            eth_hdr_t *eth = (eth_hdr_t *)rx_buf;
            if (be16(eth->type) == ETH_ARP) {
                arp_process(rx_buf, len);
                if (arp_valid && arp_cached_ip == ip) {
                    m_cpy(mac_out, arp_cached_mac, 6);
                    return 1;
                }
            }
        }
        udelay_us(10);
    }
    return 0;   /* timeout */
}

/* ================================================================== */
/* net_ping                                                            */
/* ================================================================== */
int net_ping(uint32_t dst_ip)
{
    if (!pcnet_ready)  return -2;
    if (!my_ip_addr)   return -2;

    /* Next-hop: direct if on our subnet, else via gateway */
    uint32_t nexthop = ((dst_ip & my_netmask) == (my_ip_addr & my_netmask))
                       ? dst_ip : my_gateway;

    uint8_t dst_mac[6];
    if (!arp_lookup(nexthop, dst_mac)) return -1;   /* ARP timeout */

    /* ---- Build Ethernet + IP + ICMP frame ---- */
    static uint8_t frame[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(icmp_hdr_t)];
    m_zero(frame, sizeof(frame));

    eth_hdr_t  *eth  = (eth_hdr_t  *)frame;
    ip_hdr_t   *ip   = (ip_hdr_t   *)(frame + sizeof(eth_hdr_t));
    icmp_hdr_t *icmp = (icmp_hdr_t *)(frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t));

    /* Ethernet header */
    m_cpy(eth->dst, dst_mac, 6);
    m_cpy(eth->src, my_mac,  6);
    eth->type = be16(ETH_IP);

    /* IP header */
    uint16_t ip_total = (uint16_t)(sizeof(ip_hdr_t) + sizeof(icmp_hdr_t));
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = be16(ip_total);
    ip->id         = be16(g_ip_id++);
    ip->flags_frag = be16(0x4000);   /* Don't Fragment */
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_ICMP;
    ip->checksum   = 0;
    put_ip(ip->src_ip, my_ip_addr);
    put_ip(ip->dst_ip, dst_ip);
    ip->checksum   = be16(inet_cksum(ip, sizeof(ip_hdr_t)));

    /* ICMP echo request */
    uint16_t my_id  = 0x4158;   /* 'AX' — identifies our pings */
    uint16_t my_seq = g_ping_seq++;
    icmp->type     = ICMP_ECHO_REQ;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = be16(my_id);
    icmp->seq      = be16(my_seq);
    const char *pd = "AZIXPING";
    for (int i = 0; i < 8; i++) icmp->data[i] = (uint8_t)pd[i];
    icmp->checksum = be16(inet_cksum(icmp, sizeof(icmp_hdr_t)));

    pcnet_send(frame, (uint16_t)sizeof(frame));

    /* ---- Poll for ICMP echo reply (~1 second timeout, 10µs/iter) ---- */
    for (uint32_t i = 0; i < 100000; i++) {
        if (keyboard_ctrl_c_flag) return -3;   /* Ctrl+C */
        uint16_t len = pcnet_poll(rx_buf);
        if (len >= 14) {
            eth_hdr_t *reth = (eth_hdr_t *)rx_buf;
            uint16_t  etype = be16(reth->type);

            if (etype == ETH_IP &&
                len >= (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t))) {

                ip_hdr_t *rip = (ip_hdr_t *)(rx_buf + sizeof(eth_hdr_t));
                uint16_t  ihl = (uint16_t)((rip->ver_ihl & 0x0F) * 4);

                if (rip->proto == IP_PROTO_ICMP &&
                    get_ip(rip->dst_ip) == my_ip_addr &&
                    len >= (uint16_t)(sizeof(eth_hdr_t) + ihl + sizeof(icmp_hdr_t))) {

                    icmp_hdr_t *ri = (icmp_hdr_t *)(rx_buf + sizeof(eth_hdr_t) + ihl);

                    if (ri->type == ICMP_ECHO_REP &&
                        be16(ri->id)  == my_id &&
                        be16(ri->seq) == my_seq) {
                        /* RTT estimate: i iterations × 10µs ÷ 1000 = ms */
                        return (int)(i * 10 / 1000);
                    }
                }
            } else if (etype == ETH_ARP) {
                /* Stay friendly — answer ARP requests that arrive while waiting */
                arp_process(rx_buf, len);
            }
        }
        udelay_us(10);
    }

    return -1;   /* timeout */
}

/* ================================================================== */
/* UDP send helper                                                     */
/* ================================================================== */
#define UDP_MAX_PAYLOAD 512
static uint8_t udp_frame[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) +
                          sizeof(udp_hdr_t) + UDP_MAX_PAYLOAD];

static int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                    const uint8_t *payload, uint16_t plen)
{
    if (!pcnet_ready || plen > UDP_MAX_PAYLOAD) return 0;

    uint8_t dst_mac[6];
    uint32_t nexthop = ((dst_ip & my_netmask) == (my_ip_addr & my_netmask))
                       ? dst_ip : my_gateway;
    if (!arp_lookup(nexthop, dst_mac)) return 0;

    m_zero(udp_frame, sizeof(udp_frame));

    eth_hdr_t *eth = (eth_hdr_t *)udp_frame;
    ip_hdr_t  *ip  = (ip_hdr_t  *)(udp_frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)(udp_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t));
    uint8_t   *pay = (uint8_t   *)(udp_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t));

    m_cpy(eth->dst, dst_mac, 6);
    m_cpy(eth->src, my_mac, 6);
    eth->type = be16(ETH_IP);

    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + plen);
    uint16_t ip_total = (uint16_t)(sizeof(ip_hdr_t) + udp_len);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = be16(ip_total);
    ip->id         = be16(g_ip_id++);
    ip->flags_frag = be16(0x4000);
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_UDP;
    ip->checksum   = 0;
    put_ip(ip->src_ip, my_ip_addr);
    put_ip(ip->dst_ip, dst_ip);
    ip->checksum   = be16(inet_cksum(ip, sizeof(ip_hdr_t)));

    udp->src_port = be16(src_port);
    udp->dst_port = be16(dst_port);
    udp->length   = be16(udp_len);
    udp->checksum = 0;   /* optional for IPv4 */

    m_cpy(pay, payload, plen);

    return pcnet_send(udp_frame,
                      (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + udp_len));
}

/* ================================================================== */
/* DNS resolver                                                        */
/* ================================================================== */

/* Encode "www.google.com" → \x03www\x06google\x03com\x00
   Returns encoded length. */
static uint16_t dns_encode_name(const char *name, uint8_t *out)
{
    uint8_t *p = out;
    while (*name) {
        /* find next label */
        const char *dot = name;
        while (*dot && *dot != '.') dot++;
        uint8_t len = (uint8_t)(dot - name);
        *p++ = len;
        while (name < dot) *p++ = (uint8_t)(*name++);
        if (*name == '.') name++;
    }
    *p++ = 0;   /* root label */
    return (uint16_t)(p - out);
}

/* Skip a DNS name (handles compression pointers). Returns new offset. */
static uint16_t dns_skip_name(const uint8_t *pkt, uint16_t off, uint16_t plen)
{
    while (off < plen) {
        uint8_t c = pkt[off];
        if (c == 0) { off++; break; }
        if ((c & 0xC0) == 0xC0) { off += 2; break; }   /* pointer */
        off += 1 + c;
    }
    return off;
}

static uint8_t  dns_buf[512];
static uint8_t  dns_rx[1536];
static uint16_t g_dns_txid = 1;

int net_dns_resolve(const char *hostname, uint32_t *ip_out)
{
    if (!pcnet_ready || !my_ip_addr) return 0;

    /* ---- Build DNS query ---- */
    m_zero(dns_buf, sizeof(dns_buf));
    uint16_t txid = g_dns_txid++;
    dns_buf[0] = (uint8_t)(txid >> 8);
    dns_buf[1] = (uint8_t)(txid);
    dns_buf[2] = 0x01; dns_buf[3] = 0x00;  /* flags: RD=1 */
    dns_buf[4] = 0x00; dns_buf[5] = 0x01;  /* QDCOUNT=1 */
    /* ANCOUNT/NSCOUNT/ARCOUNT = 0 */

    uint16_t off = 12;
    off += dns_encode_name(hostname, dns_buf + off);
    dns_buf[off++] = 0x00; dns_buf[off++] = 0x01;  /* QTYPE  A */
    dns_buf[off++] = 0x00; dns_buf[off++] = 0x01;  /* QCLASS IN */

    udp_send(NET_DNS, DNS_SRC_PORT, DNS_PORT, dns_buf, off);

    /* ---- Poll for reply (~2 second timeout) ---- */
    for (uint32_t i = 0; i < 200000; i++) {
        if (keyboard_ctrl_c_flag) return 0;   /* Ctrl+C — abort DNS */
        uint16_t len = pcnet_poll(dns_rx);
        if (len >= 14) {
            eth_hdr_t *eth = (eth_hdr_t *)dns_rx;
            uint16_t etype = be16(eth->type);
            if (etype == ETH_ARP) { arp_process(dns_rx, len); goto next; }
            if (etype != ETH_IP)  goto next;

            ip_hdr_t *ip = (ip_hdr_t *)(dns_rx + 14);
            if (ip->proto != IP_PROTO_UDP) goto next;
            uint16_t ihl = (uint16_t)((ip->ver_ihl & 0x0F) * 4);

            udp_hdr_t *udp = (udp_hdr_t *)(dns_rx + 14 + ihl);
            if (be16(udp->dst_port) != DNS_SRC_PORT) goto next;

            uint16_t ulen = be16(udp->length);
            if (ulen < 8 + 12) goto next;
            uint16_t plen = (uint16_t)(ulen - 8);  /* UDP payload length */
            const uint8_t *p = (const uint8_t *)udp + 8;

            /* Verify transaction ID */
            uint16_t rtxid = ((uint16_t)p[0] << 8) | p[1];
            if (rtxid != txid) goto next;

            /* Check QR bit (bit15 of flags) = 1 means response */
            if (!(p[2] & 0x80)) goto next;

            uint16_t ancount = ((uint16_t)p[6] << 8) | p[7];
            if (ancount == 0) return 0;   /* no answers */

            /* Skip header + question section */
            uint16_t pos = 12;
            uint16_t qdcount = ((uint16_t)p[4] << 8) | p[5];
            for (uint16_t q = 0; q < qdcount && pos < plen; q++) {
                pos = dns_skip_name(p, pos, plen);
                pos += 4;   /* QTYPE + QCLASS */
            }

            /* Parse answer RRs — find first A record */
            for (uint16_t a = 0; a < ancount && pos < plen; a++) {
                pos = dns_skip_name(p, pos, plen);
                if (pos + 10 > plen) break;
                uint16_t rtype  = ((uint16_t)p[pos] << 8) | p[pos+1]; pos += 2;
                pos += 2;   /* class */
                pos += 4;   /* TTL */
                uint16_t rdlen  = ((uint16_t)p[pos] << 8) | p[pos+1]; pos += 2;
                if (rtype == 1 && rdlen == 4 && pos + 4 <= plen) {
                    *ip_out = ((uint32_t)p[pos]   << 24) |
                              ((uint32_t)p[pos+1] << 16) |
                              ((uint32_t)p[pos+2] <<  8) |
                               (uint32_t)p[pos+3];
                    return 1;
                }
                pos += rdlen;
            }
        }
        next:
        udelay_us(10);
    }
    return 0;   /* timeout */
}
