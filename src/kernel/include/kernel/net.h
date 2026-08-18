/*
 * Qira OS - networking
 *
 * Qira implements a compact IPv4 stack: an Ethernet layer over the emulated
 * NICs it can drive, ARP, IPv4, ICMP echo and UDP. TCP is not implemented.
 * When no supported NIC is present the stack runs with only the loopback
 * interface, which still exercises the protocol code and lets `ping 127.0.0.1`
 * work.
 */
#ifndef QIRA_NET_H
#define QIRA_NET_H

#include <kernel/types.h>

struct shell;

#define ETH_ALEN       6
#define NET_MTU        1500
#define NET_MAX_IFACES 4

typedef uint32_t ipv4_addr_t;

#define IPV4(a, b, c, d) \
    ((ipv4_addr_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                   ((uint32_t)(c) << 8) | (uint32_t)(d)))

struct net_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_dropped;
    uint64_t tx_errors;
    uint64_t arp_requests;
    uint64_t arp_replies;
    uint64_t icmp_echo_requests;
    uint64_t icmp_echo_replies;
};

struct net_interface {
    char        name[16];
    uint8_t     mac[ETH_ALEN];
    ipv4_addr_t address;
    ipv4_addr_t netmask;
    ipv4_addr_t gateway;
    bool_t      up;
    bool_t      loopback;
    struct net_stats stats;

    /* Driver hook; NULL for loopback. */
    int (*transmit)(struct net_interface *iface, const void *frame, size_t len);
    void *driver_data;
};

void net_init(void);

struct net_interface *net_interface_at(int index);
int  net_interface_count(void);
struct net_interface *net_find_interface(const char *name);

/* Called by drivers when a frame arrives. */
void net_receive(struct net_interface *iface, const void *frame, size_t len);

int  net_send_ipv4(struct net_interface *iface, ipv4_addr_t destination,
                   uint8_t protocol, const void *payload, size_t len);

/* ICMP echo. Returns the round-trip time in microseconds, or negative. */
int  net_ping(struct shell *sh, const char *address);

/* Send echo requests without printing; returns how many were sent. */
int  net_ping_quiet(ipv4_addr_t target, int count);

void net_print_info(struct shell *sh);
void net_format_ip(ipv4_addr_t address, char *out, size_t size);
bool_t net_parse_ip(const char *text, ipv4_addr_t *out);

/* The interface packets go out of by default. */
struct net_interface *net_default_interface(void);

/* Shared helpers, used by the transport layer. */
uint16_t net_checksum16(const void *data, size_t len);
uint16_t net_htons(uint16_t value);
uint32_t net_htonl(uint32_t value);

/* --- transport: TCP, UDP and DNS -------------------------------------- */

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
} tcp_state_t;

struct tcp_socket;

void tcp_init(void);

/* Open a connection. Returns NULL if it could not be established. */
struct tcp_socket *tcp_connect(ipv4_addr_t address, uint16_t port,
                               uint32_t timeout_ms);
ssize_t tcp_send(struct tcp_socket *socket, const void *data, size_t len);
ssize_t tcp_receive(struct tcp_socket *socket, void *out, size_t len,
                    uint32_t timeout_ms);
void    tcp_close(struct tcp_socket *socket);

tcp_state_t tcp_socket_state(const struct tcp_socket *socket);
const char *tcp_state_name(tcp_state_t state);
int         tcp_active_sockets(void);
void        tcp_get_stats(struct net_stats *out);

/* Called from the IPv4 receive path. */
void tcp_input(struct net_interface *iface, ipv4_addr_t source,
               ipv4_addr_t destination, const uint8_t *data, size_t len);
void udp_input(struct net_interface *iface, ipv4_addr_t source,
               const uint8_t *data, size_t len);

int udp_send(ipv4_addr_t address, uint16_t port, uint16_t source_port,
             const void *data, size_t len);

/* Name resolution. A literal address is returned unchanged. */
int dns_resolve(const char *hostname, ipv4_addr_t *out, uint32_t timeout_ms);
ipv4_addr_t net_dns_server(void);
void        net_set_dns_server(ipv4_addr_t address);

/* Drivers. */
void ne2000_init(void);
void rtl8139_init(void);

#endif /* QIRA_NET_H */
