/*
 * QitoOS - IPv4 network stack
 *
 * Implements Ethernet framing, ARP, IPv4 and ICMP echo. A loopback interface
 * is always present so the protocol paths can be exercised (and tested) even
 * on machines where no supported NIC exists.
 */

#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/shell.h>
#include <kernel/printf.h>
#include <kernel/config.h>

/* --- wire formats ----------------------------------------------------- */

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct eth_header {
    uint8_t  destination[ETH_ALEN];
    uint8_t  source[ETH_ALEN];
    uint16_t type;
} PACKED;

struct arp_packet {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t  hardware_len;
    uint8_t  protocol_len;
    uint16_t operation;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} PACKED;

struct ipv4_header {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t source;
    uint32_t destination;
} PACKED;

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} PACKED;

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* --- byte order ------------------------------------------------------- */

static inline uint16_t bswap16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static inline uint32_t bswap32(uint32_t value)
{
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)

/* --- state ------------------------------------------------------------ */

static struct net_interface interfaces[NET_MAX_IFACES];
static int                  interface_count;

/* A small ARP cache. */
#define ARP_CACHE_SIZE 16
static struct {
    ipv4_addr_t address;
    uint8_t     mac[ETH_ALEN];
    uint64_t    expires_ms;
    bool_t      valid;
} arp_cache[ARP_CACHE_SIZE];

/* Set when an echo reply we are waiting for arrives. */
static volatile bool_t   ping_reply_received;
static volatile uint16_t ping_expect_sequence;

void net_format_ip(ipv4_addr_t address, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", (unsigned)((address >> 24) & 0xFF),
             (unsigned)((address >> 16) & 0xFF), (unsigned)((address >> 8) & 0xFF),
             (unsigned)(address & 0xFF));
}

bool_t net_parse_ip(const char *text, ipv4_addr_t *out)
{
    uint32_t octets[4] = {0};
    int      index     = 0;

    for (const char *p = text; *p && index < 4; p++) {
        if (isdigit((uint8_t)*p)) {
            octets[index] = octets[index] * 10 + (uint32_t)(*p - '0');
            if (octets[index] > 255) {
                return false;
            }
        } else if (*p == '.') {
            index++;
        } else {
            return false;
        }
    }

    if (index != 3) {
        return false;
    }
    *out = IPV4(octets[0], octets[1], octets[2], octets[3]);
    return true;
}

uint16_t net_htons(uint16_t value)
{
    return bswap16(value);
}

uint32_t net_htonl(uint32_t value)
{
    return bswap32(value);
}

/* Internet checksum (RFC 1071). */
static uint16_t checksum16(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t       sum   = 0;

    while (len > 1) {
        sum += (uint32_t)((bytes[0] << 8) | bytes[1]);
        bytes += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)(bytes[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint16_t net_checksum16(const void *data, size_t len)
{
    return checksum16(data, len);
}

/* --- interfaces ------------------------------------------------------- */

static int loopback_transmit(struct net_interface *iface, const void *frame,
                             size_t len)
{
    /* Loop the frame straight back into the receive path. */
    net_receive(iface, frame, len);
    return 0;
}

struct net_interface *net_register_interface(const char *name, const uint8_t *mac,
                                             int (*transmit)(struct net_interface *,
                                                             const void *, size_t),
                                             void *driver_data)
{
    if (interface_count >= NET_MAX_IFACES) {
        return NULL;
    }

    struct net_interface *iface = &interfaces[interface_count++];
    memset(iface, 0, sizeof(*iface));

    strlcpy(iface->name, name, sizeof(iface->name));
    if (mac) {
        memcpy(iface->mac, mac, ETH_ALEN);
    }
    iface->transmit    = transmit;
    iface->driver_data = driver_data;
    iface->up          = true;
    return iface;
}

int net_interface_count(void)
{
    return interface_count;
}

struct net_interface *net_interface_at(int index)
{
    if (index < 0 || index >= interface_count) {
        return NULL;
    }
    return &interfaces[index];
}

struct net_interface *net_find_interface(const char *name)
{
    for (int i = 0; i < interface_count; i++) {
        if (strcmp(interfaces[i].name, name) == 0) {
            return &interfaces[i];
        }
    }
    return NULL;
}

/* --- ARP -------------------------------------------------------------- */

static void arp_cache_insert(ipv4_addr_t address, const uint8_t *mac)
{
    int slot = -1;

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].address == address) {
            slot = i;
            break;
        }
        if (!arp_cache[i].valid && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;   /* evict the first entry */
    }

    arp_cache[slot].address    = address;
    arp_cache[slot].valid      = true;
    arp_cache[slot].expires_ms = time_uptime_ms() + 120000;
    memcpy(arp_cache[slot].mac, mac, ETH_ALEN);
}

static bool_t arp_cache_lookup(ipv4_addr_t address, uint8_t *mac_out)
{
    uint64_t now = time_uptime_ms();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].address == address) {
            if (now > arp_cache[i].expires_ms) {
                arp_cache[i].valid = false;
                return false;
            }
            memcpy(mac_out, arp_cache[i].mac, ETH_ALEN);
            return true;
        }
    }
    return false;
}

static int eth_send(struct net_interface *iface, const uint8_t *destination,
                    uint16_t type, const void *payload, size_t len)
{
    if (!iface || !iface->up || !iface->transmit) {
        return -1;
    }
    if (len > NET_MTU) {
        return -1;
    }

    uint8_t frame[sizeof(struct eth_header) + NET_MTU];
    struct eth_header *header = (struct eth_header *)frame;

    memcpy(header->destination, destination, ETH_ALEN);
    memcpy(header->source, iface->mac, ETH_ALEN);
    header->type = htons(type);
    memcpy(frame + sizeof(*header), payload, len);

    size_t total = sizeof(*header) + len;
    /* Ethernet requires a minimum 60-byte frame excluding the FCS. */
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }

    int result = iface->transmit(iface, frame, total);
    if (result == 0) {
        iface->stats.tx_packets++;
        iface->stats.tx_bytes += total;
    } else {
        iface->stats.tx_errors++;
    }
    return result;
}

static void arp_handle(struct net_interface *iface, const struct arp_packet *arp)
{
    if (ntohs(arp->hardware_type) != 1 || ntohs(arp->protocol_type) != ETHERTYPE_IPV4) {
        return;
    }

    ipv4_addr_t sender = ntohl(arp->sender_ip);
    ipv4_addr_t target = ntohl(arp->target_ip);

    arp_cache_insert(sender, arp->sender_mac);

    uint16_t operation = ntohs(arp->operation);
    if (operation == 1) {
        iface->stats.arp_requests++;
        /* Answer requests for our own address. */
        if (target != iface->address) {
            return;
        }

        struct arp_packet reply;
        memset(&reply, 0, sizeof(reply));
        reply.hardware_type = htons(1);
        reply.protocol_type = htons(ETHERTYPE_IPV4);
        reply.hardware_len  = ETH_ALEN;
        reply.protocol_len  = 4;
        reply.operation     = htons(2);
        memcpy(reply.sender_mac, iface->mac, ETH_ALEN);
        reply.sender_ip = htonl(iface->address);
        memcpy(reply.target_mac, arp->sender_mac, ETH_ALEN);
        reply.target_ip = htonl(sender);

        eth_send(iface, arp->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
        iface->stats.arp_replies++;
    } else if (operation == 2) {
        iface->stats.arp_replies++;
    }
}

/* --- ICMP ------------------------------------------------------------- */

static void icmp_handle(struct net_interface *iface, const struct ipv4_header *ip,
                        const uint8_t *payload, size_t len)
{
    if (len < sizeof(struct icmp_header)) {
        return;
    }

    const struct icmp_header *icmp = (const struct icmp_header *)payload;

    if (icmp->type == ICMP_ECHO_REQUEST) {
        iface->stats.icmp_echo_requests++;

        /* Build the reply by copying the request and flipping the type. */
        uint8_t reply[NET_MTU];
        size_t  reply_len = MIN(len, sizeof(reply));
        memcpy(reply, payload, reply_len);

        struct icmp_header *reply_icmp = (struct icmp_header *)reply;
        reply_icmp->type     = ICMP_ECHO_REPLY;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = htons(checksum16(reply, reply_len));

        net_send_ipv4(iface, ntohl(ip->source), IP_PROTO_ICMP, reply, reply_len);
        iface->stats.icmp_echo_replies++;
    } else if (icmp->type == ICMP_ECHO_REPLY) {
        iface->stats.icmp_echo_replies++;
        if (ntohs(icmp->sequence) == ping_expect_sequence) {
            ping_reply_received = true;
        }
    }
}

/* --- receive path ----------------------------------------------------- */

void net_receive(struct net_interface *iface, const void *frame, size_t len)
{
    if (!iface || len < sizeof(struct eth_header)) {
        if (iface) {
            iface->stats.rx_dropped++;
        }
        return;
    }

    iface->stats.rx_packets++;
    iface->stats.rx_bytes += len;

    const struct eth_header *eth = (const struct eth_header *)frame;
    const uint8_t *payload = (const uint8_t *)frame + sizeof(*eth);
    size_t payload_len     = len - sizeof(*eth);

    switch (ntohs(eth->type)) {
    case ETHERTYPE_ARP:
        if (payload_len >= sizeof(struct arp_packet)) {
            arp_handle(iface, (const struct arp_packet *)payload);
        }
        break;

    case ETHERTYPE_IPV4: {
        if (payload_len < sizeof(struct ipv4_header)) {
            iface->stats.rx_dropped++;
            return;
        }
        const struct ipv4_header *ip = (const struct ipv4_header *)payload;

        uint8_t header_len = (uint8_t)((ip->version_ihl & 0x0F) * 4);
        if ((ip->version_ihl >> 4) != 4 || header_len < sizeof(*ip)) {
            iface->stats.rx_dropped++;
            return;
        }

        uint16_t total = ntohs(ip->total_length);
        if (total > payload_len || total < header_len) {
            iface->stats.rx_dropped++;
            return;
        }

        const uint8_t *body     = payload + header_len;
        size_t         body_len = (size_t)(total - header_len);

        switch (ip->protocol) {
        case IP_PROTO_ICMP:
            icmp_handle(iface, ip, body, body_len);
            break;
        case IP_PROTO_TCP:
            tcp_input(iface, ntohl(ip->source), ntohl(ip->destination), body,
                      body_len);
            break;
        case IP_PROTO_UDP:
            udp_input(iface, ntohl(ip->source), body, body_len);
            break;
        default:
            break;
        }
        break;
    }

    default:
        iface->stats.rx_dropped++;
        break;
    }
}

/* --- transmit path ---------------------------------------------------- */

int net_send_ipv4(struct net_interface *iface, ipv4_addr_t destination,
                  uint8_t protocol, const void *payload, size_t len)
{
    if (!iface || len + sizeof(struct ipv4_header) > NET_MTU) {
        return -1;
    }

    uint8_t packet[NET_MTU];
    struct ipv4_header *ip = (struct ipv4_header *)packet;

    static uint16_t identification;

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl    = 0x45;   /* IPv4, 5 dwords of header */
    ip->total_length   = htons((uint16_t)(sizeof(*ip) + len));
    ip->identification = htons(++identification);
    ip->ttl            = 64;
    ip->protocol       = protocol;
    ip->source         = htonl(iface->address);
    ip->destination    = htonl(destination);
    ip->checksum       = 0;
    ip->checksum       = htons(checksum16(ip, sizeof(*ip)));

    memcpy(packet + sizeof(*ip), payload, len);

    /* Resolve the next hop. */
    uint8_t destination_mac[ETH_ALEN];

    if (iface->loopback) {
        memcpy(destination_mac, iface->mac, ETH_ALEN);
    } else if (!arp_cache_lookup(destination, destination_mac)) {
        /* No cache entry: broadcast an ARP request and drop this packet. */
        struct arp_packet request;
        memset(&request, 0, sizeof(request));
        request.hardware_type = htons(1);
        request.protocol_type = htons(ETHERTYPE_IPV4);
        request.hardware_len  = ETH_ALEN;
        request.protocol_len  = 4;
        request.operation     = htons(1);
        memcpy(request.sender_mac, iface->mac, ETH_ALEN);
        request.sender_ip = htonl(iface->address);
        request.target_ip = htonl(destination);

        static const uint8_t broadcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF};
        eth_send(iface, broadcast, ETHERTYPE_ARP, &request, sizeof(request));
        iface->stats.arp_requests++;
        return -1;
    }

    return eth_send(iface, destination_mac, ETHERTYPE_IPV4, packet,
                    sizeof(*ip) + len);
}

/* --- ping ------------------------------------------------------------- */

int net_ping(struct shell *sh, const char *address)
{
    ipv4_addr_t target;

    if (!net_parse_ip(address, &target)) {
        shell_printf(sh, "ping: cannot resolve '%s': name resolution is not "
                         "implemented, use a numeric address\n", address);
        return 1;
    }

    /* Choose the interface: loopback for 127.x, otherwise the first one up. */
    struct net_interface *iface = NULL;
    if ((target >> 24) == 127) {
        iface = net_find_interface("lo");
    } else {
        for (int i = 0; i < interface_count; i++) {
            if (interfaces[i].up && !interfaces[i].loopback) {
                iface = &interfaces[i];
                break;
            }
        }
    }
    if (!iface) {
        shell_printf(sh, "ping: no usable network interface\n");
        return 1;
    }

    char target_text[24];
    net_format_ip(target, target_text, sizeof(target_text));
    shell_printf(sh, "PING %s via %s, 56 data bytes\n", target_text, iface->name);

    int received = 0;
    int sent     = 0;

    for (uint16_t sequence = 1; sequence <= 4; sequence++) {
        uint8_t buffer[64];
        struct icmp_header *icmp = (struct icmp_header *)buffer;

        memset(buffer, 0, sizeof(buffer));
        icmp->type       = ICMP_ECHO_REQUEST;
        icmp->code       = 0;
        icmp->identifier = htons(0x9110);
        icmp->sequence   = htons(sequence);
        for (size_t i = sizeof(*icmp); i < sizeof(buffer); i++) {
            buffer[i] = (uint8_t)('a' + (i % 23));
        }
        icmp->checksum = 0;
        icmp->checksum = htons(checksum16(buffer, sizeof(buffer)));

        ping_reply_received  = false;
        ping_expect_sequence = sequence;

        uint64_t start = time_uptime_us();
        sent++;

        if (net_send_ipv4(iface, target, IP_PROTO_ICMP, buffer, sizeof(buffer)) != 0) {
            shell_printf(sh, "  seq=%u  send failed (no ARP entry yet)\n", sequence);
            time_sleep_ms(200);
            continue;
        }

        /* Wait up to a second for the reply. */
        for (int wait = 0; wait < 100 && !ping_reply_received; wait++) {
            time_sleep_ms(10);
        }

        uint64_t elapsed = time_uptime_us() - start;

        if (ping_reply_received) {
            received++;
            shell_printf(sh, "  64 bytes from %s: seq=%u ttl=64 time=%llu.%03llu ms\n",
                         target_text, sequence,
                         (unsigned long long)(elapsed / 1000),
                         (unsigned long long)(elapsed % 1000));
        } else {
            shell_printf(sh, "  seq=%u  request timed out\n", sequence);
        }

        if (sequence < 4) {
            time_sleep_ms(200);
        }
    }

    int loss = sent ? ((sent - received) * 100) / sent : 100;
    shell_printf(sh, "\n--- %s ping statistics ---\n", target_text);
    shell_printf(sh, "%d packets transmitted, %d received, %d%% packet loss\n", sent,
                 received, loss);
    return (received > 0) ? 0 : 1;
}

/*
 * Send a burst of echo requests without printing anything. The graphical
 * network tool has no shell sink to write through, so it drives this and
 * reads the results out of the interface counters afterwards.
 */
int net_ping_quiet(ipv4_addr_t target, int count)
{
    struct net_interface *iface =
        ((target >> 24) == 127) ? net_find_interface("lo")
                                : net_default_interface();
    if (!iface) {
        return -1;
    }

    int sent = 0;

    for (uint16_t sequence = 1; sequence <= (uint16_t)count; sequence++) {
        uint8_t buffer[64];
        struct icmp_header *icmp = (struct icmp_header *)buffer;

        memset(buffer, 0, sizeof(buffer));
        icmp->type       = ICMP_ECHO_REQUEST;
        icmp->identifier = htons(0x9110);
        icmp->sequence   = htons(sequence);
        for (size_t i = sizeof(*icmp); i < sizeof(buffer); i++) {
            buffer[i] = (uint8_t)('a' + (i % 23));
        }
        icmp->checksum = 0;
        icmp->checksum = htons(checksum16(buffer, sizeof(buffer)));

        ping_reply_received  = false;
        ping_expect_sequence = sequence;

        if (net_send_ipv4(iface, target, IP_PROTO_ICMP, buffer,
                          sizeof(buffer)) == 0) {
            sent++;
        }

        for (int wait = 0; wait < 60 && !ping_reply_received; wait++) {
            time_sleep_ms(10);
        }
    }

    return sent;
}

/* --- reporting -------------------------------------------------------- */

void net_print_info(struct shell *sh)
{
    if (interface_count == 0) {
        shell_printf(sh, "  no network interfaces\n");
        return;
    }

    for (int i = 0; i < interface_count; i++) {
        struct net_interface *iface = &interfaces[i];

        char address[24], netmask[24], gateway[24];
        net_format_ip(iface->address, address, sizeof(address));
        net_format_ip(iface->netmask, netmask, sizeof(netmask));
        net_format_ip(iface->gateway, gateway, sizeof(gateway));

        shell_printf(sh, "%s: flags=<%s%s>  mtu %d\n", iface->name,
                     iface->up ? "UP" : "DOWN",
                     iface->loopback ? ",LOOPBACK" : ",BROADCAST", NET_MTU);
        shell_printf(sh, "    inet %s  netmask %s", address, netmask);
        if (iface->gateway) {
            shell_printf(sh, "  gateway %s", gateway);
        }
        shell_printf(sh, "\n");

        if (!iface->loopback) {
            shell_printf(sh, "    ether %02x:%02x:%02x:%02x:%02x:%02x\n",
                         iface->mac[0], iface->mac[1], iface->mac[2],
                         iface->mac[3], iface->mac[4], iface->mac[5]);
        }

        shell_printf(sh, "    RX packets %llu  bytes %llu  dropped %llu\n",
                     (unsigned long long)iface->stats.rx_packets,
                     (unsigned long long)iface->stats.rx_bytes,
                     (unsigned long long)iface->stats.rx_dropped);
        shell_printf(sh, "    TX packets %llu  bytes %llu  errors %llu\n",
                     (unsigned long long)iface->stats.tx_packets,
                     (unsigned long long)iface->stats.tx_bytes,
                     (unsigned long long)iface->stats.tx_errors);
        shell_printf(sh, "    ARP requests %llu  replies %llu  "
                         "ICMP echo rx %llu tx %llu\n",
                     (unsigned long long)iface->stats.arp_requests,
                     (unsigned long long)iface->stats.arp_replies,
                     (unsigned long long)iface->stats.icmp_echo_requests,
                     (unsigned long long)iface->stats.icmp_echo_replies);
        shell_printf(sh, "\n");
    }

    shell_printf(sh, "Protocols: Ethernet, ARP, IPv4, ICMP echo. TCP is not "
                     "implemented.\n");
}

static ipv4_addr_t dns_server;

struct net_interface *net_default_interface(void)
{
    for (int i = 0; i < interface_count; i++) {
        if (interfaces[i].up && !interfaces[i].loopback) {
            return &interfaces[i];
        }
    }
    /* Loopback is better than nothing: it keeps the stack testable. */
    return interface_count ? &interfaces[0] : NULL;
}

ipv4_addr_t net_dns_server(void)
{
    return dns_server;
}

void net_set_dns_server(ipv4_addr_t address)
{
    dns_server = address;

    char text[24];
    net_format_ip(address, text, sizeof(text));
    KLOG_INFO("net", "resolver set to %s", text);
}

void net_init(void)
{
    interface_count = 0;
    memset(arp_cache, 0, sizeof(arp_cache));

    /* Loopback is always available. */
    static const uint8_t loopback_mac[ETH_ALEN] = {0, 0, 0, 0, 0, 0};
    struct net_interface *lo =
        net_register_interface("lo", loopback_mac, loopback_transmit, NULL);
    if (lo) {
        lo->address  = IPV4(127, 0, 0, 1);
        lo->netmask  = IPV4(255, 0, 0, 0);
        lo->loopback = true;
        lo->up       = true;
        /* Loopback resolves its own address without ARP. */
        arp_cache_insert(lo->address, loopback_mac);
    }

    /* Probe for supported NICs. */
    if (config_get_bool("net.enabled", true)) {
        ne2000_init();
        rtl8139_init();
    }

    /* The gateway doubles as the resolver under emulator NAT. */
    struct net_interface *primary = net_default_interface();
    if (primary && primary->gateway) {
        dns_server = primary->gateway;
    } else {
        dns_server = IPV4(10, 0, 2, 3);   /* the usual QEMU resolver */
    }

    tcp_init();

    KLOG_INFO("net", "%d interface(s): IPv4, ARP, ICMP, TCP, UDP and DNS",
              interface_count);
}
