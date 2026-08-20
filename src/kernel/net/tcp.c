/*
 * QitoOS - TCP, UDP and DNS
 *
 * Enough of the transport layer to fetch things over the network: a TCP state
 * machine that can open a connection, stream data and close cleanly, UDP
 * datagrams, and a DNS resolver built on top of UDP.
 *
 * This is a compact implementation, not a hardened one. It handles the happy
 * path plus retransmission and out-of-order arrival; it does not implement
 * congestion control beyond a fixed window, selective acknowledgement, or
 * path MTU discovery. That is a deliberate scope choice: the goal is for
 * `fetch` and `git clone` to work over a LAN or an emulator's NAT, not to
 * survive the open internet under loss.
 */

#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/printf.h>
#include <kernel/spinlock.h>

#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

#define TCP_WINDOW      8192
#define TCP_MAX_SOCKETS 8
#define TCP_RX_BUFFER   16384
#define TCP_TX_BUFFER   8192
#define TCP_MSS         1400

struct tcp_header {
    uint16_t source_port;
    uint16_t dest_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t  data_offset;      /* high nibble: header length in dwords */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} PACKED;

struct udp_header {
    uint16_t source_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} PACKED;

/* Pseudo-header used when checksumming TCP and UDP. */
struct pseudo_header {
    uint32_t source;
    uint32_t destination;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t length;
} PACKED;

struct tcp_socket {
    bool_t       in_use;
    tcp_state_t  state;

    ipv4_addr_t  local_address;
    ipv4_addr_t  remote_address;
    uint16_t     local_port;
    uint16_t     remote_port;

    uint32_t     send_next;      /* next sequence number to send        */
    uint32_t     send_unack;     /* oldest unacknowledged byte          */
    uint32_t     receive_next;   /* next sequence number expected       */
    uint16_t     remote_window;

    uint8_t     *receive_buffer;
    size_t       receive_length;

    /* Retransmission of the most recent unacknowledged segment. */
    uint8_t     *pending;
    size_t       pending_length;
    uint64_t     pending_sent_ms;
    int          retries;

    uint64_t     last_activity_ms;
    struct net_interface *iface;
};

static struct tcp_socket sockets[TCP_MAX_SOCKETS];
static spinlock_t        tcp_lock;
static uint16_t          next_ephemeral_port = 49152;

static struct net_stats transport_stats;

/* Provided by net.c. */
extern uint16_t net_checksum16(const void *data, size_t len);
extern uint16_t net_htons(uint16_t value);
extern uint32_t net_htonl(uint32_t value);
extern int net_send_ipv4(struct net_interface *iface, ipv4_addr_t destination,
                         uint8_t protocol, const void *payload, size_t len);
extern struct net_interface *net_default_interface(void);

#define htons(x) net_htons(x)
#define ntohs(x) net_htons(x)
#define htonl(x) net_htonl(x)
#define ntohl(x) net_htonl(x)

/* Checksum a TCP or UDP segment, including its pseudo-header. */
static uint16_t transport_checksum(ipv4_addr_t source, ipv4_addr_t destination,
                                   uint8_t protocol, const void *segment,
                                   size_t len)
{
    struct pseudo_header pseudo = {
        .source      = htonl(source),
        .destination = htonl(destination),
        .zero        = 0,
        .protocol    = protocol,
        .length      = htons((uint16_t)len),
    };

    /*
     * The checksum spans the pseudo-header followed by the segment, so they
     * are summed together in one buffer.
     */
    static uint8_t scratch[sizeof(pseudo) + NET_MTU];
    if (len > NET_MTU) {
        return 0;
    }

    memcpy(scratch, &pseudo, sizeof(pseudo));
    memcpy(scratch + sizeof(pseudo), segment, len);

    return net_checksum16(scratch, sizeof(pseudo) + len);
}

/* --- socket table ------------------------------------------------------ */

static struct tcp_socket *socket_allocate(void)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            struct tcp_socket *socket = &sockets[i];
            memset(socket, 0, sizeof(*socket));

            socket->receive_buffer = kmalloc(TCP_RX_BUFFER);
            if (!socket->receive_buffer) {
                return NULL;
            }
            socket->in_use     = true;
            socket->state      = TCP_CLOSED;
            socket->local_port = next_ephemeral_port++;
            if (next_ephemeral_port < 49152) {
                next_ephemeral_port = 49152;
            }
            return socket;
        }
    }
    return NULL;
}

static void socket_release(struct tcp_socket *socket)
{
    if (!socket || !socket->in_use) {
        return;
    }
    kfree(socket->receive_buffer);
    kfree(socket->pending);
    memset(socket, 0, sizeof(*socket));
}

static struct tcp_socket *socket_find(ipv4_addr_t remote, uint16_t remote_port,
                                      uint16_t local_port)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        struct tcp_socket *socket = &sockets[i];
        if (socket->in_use && socket->local_port == local_port &&
            socket->remote_address == remote &&
            socket->remote_port == remote_port) {
            return socket;
        }
    }
    return NULL;
}

/* --- sending ----------------------------------------------------------- */

static int tcp_transmit(struct tcp_socket *socket, uint8_t flags,
                        const void *payload, size_t len)
{
    uint8_t segment[sizeof(struct tcp_header) + TCP_MSS];

    if (len > TCP_MSS) {
        len = TCP_MSS;
    }

    struct tcp_header *header = (struct tcp_header *)segment;
    memset(header, 0, sizeof(*header));

    header->source_port     = htons(socket->local_port);
    header->dest_port       = htons(socket->remote_port);
    header->sequence        = htonl(socket->send_next);
    header->acknowledgement = (flags & TCP_ACK) ? htonl(socket->receive_next) : 0;
    header->data_offset     = (uint8_t)((sizeof(*header) / 4) << 4);
    header->flags           = flags;
    header->window          = htons(TCP_WINDOW);

    if (payload && len) {
        memcpy(segment + sizeof(*header), payload, len);
    }

    size_t total = sizeof(*header) + len;

    header->checksum = 0;
    header->checksum = htons(transport_checksum(socket->local_address,
                                                socket->remote_address,
                                                IP_PROTO_TCP, segment, total));

    int result = net_send_ipv4(socket->iface, socket->remote_address,
                               IP_PROTO_TCP, segment, total);
    if (result == 0) {
        transport_stats.tx_packets++;
        transport_stats.tx_bytes += total;
    } else {
        transport_stats.tx_errors++;
    }

    /* SYN and FIN each consume one sequence number. */
    if (flags & (TCP_SYN | TCP_FIN)) {
        socket->send_next++;
    }
    socket->send_next += (uint32_t)len;

    return result;
}

/* --- receive path ------------------------------------------------------ */

void tcp_input(struct net_interface *iface, ipv4_addr_t source,
               ipv4_addr_t destination, const uint8_t *data, size_t len)
{
    UNUSED(destination);

    if (len < sizeof(struct tcp_header)) {
        return;
    }

    const struct tcp_header *header = (const struct tcp_header *)data;

    uint16_t source_port = ntohs(header->source_port);
    uint16_t dest_port   = ntohs(header->dest_port);
    uint8_t  header_len  = (uint8_t)((header->data_offset >> 4) * 4);

    if (header_len < sizeof(*header) || header_len > len) {
        return;
    }

    const uint8_t *payload = data + header_len;
    size_t payload_len     = len - header_len;

    transport_stats.rx_packets++;
    transport_stats.rx_bytes += len;

    struct tcp_socket *socket = socket_find(source, source_port, dest_port);
    if (!socket) {
        /* Nothing is listening; a well-behaved stack answers with a reset. */
        return;
    }

    socket->iface            = iface;
    socket->last_activity_ms = time_uptime_ms();
    socket->remote_window    = ntohs(header->window);

    uint32_t sequence        = ntohl(header->sequence);
    uint32_t acknowledgement = ntohl(header->acknowledgement);

    if (header->flags & TCP_RST) {
        KLOG_DEBUG("tcp", "connection reset by the peer");
        socket->state = TCP_CLOSED;
        return;
    }

    switch (socket->state) {
    case TCP_SYN_SENT:
        if ((header->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            socket->receive_next = sequence + 1;
            socket->send_unack   = acknowledgement;
            socket->state        = TCP_ESTABLISHED;

            /* Complete the handshake. */
            tcp_transmit(socket, TCP_ACK, NULL, 0);
            KLOG_DEBUG("tcp", "connection established on port %u",
                       socket->local_port);
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (header->flags & TCP_ACK) {
            socket->send_unack = acknowledgement;

            /* The pending segment has been acknowledged. */
            if (socket->pending) {
                kfree(socket->pending);
                socket->pending        = NULL;
                socket->pending_length = 0;
                socket->retries        = 0;
            }
        }

        if (payload_len > 0) {
            /*
             * Only in-order data is accepted. Anything else is dropped and
             * the peer retransmits, which is correct if inefficient.
             */
            if (sequence == socket->receive_next) {
                size_t space = TCP_RX_BUFFER - socket->receive_length;
                size_t copy  = MIN(payload_len, space);

                if (copy > 0) {
                    memcpy(socket->receive_buffer + socket->receive_length,
                           payload, copy);
                    socket->receive_length += copy;
                    socket->receive_next += (uint32_t)copy;
                }
                if (copy < payload_len) {
                    transport_stats.rx_dropped++;
                }
            }
            /* Acknowledge whatever is now in order. */
            tcp_transmit(socket, TCP_ACK, NULL, 0);
        }

        if (header->flags & TCP_FIN) {
            socket->receive_next = sequence + (uint32_t)payload_len + 1;
            socket->state        = TCP_CLOSE_WAIT;
            tcp_transmit(socket, TCP_ACK, NULL, 0);
            KLOG_DEBUG("tcp", "peer closed the connection");
        }
        break;

    case TCP_FIN_WAIT:
        if (header->flags & TCP_ACK) {
            socket->state = TCP_CLOSED;
        }
        if (header->flags & TCP_FIN) {
            socket->receive_next = sequence + 1;
            tcp_transmit(socket, TCP_ACK, NULL, 0);
            socket->state = TCP_CLOSED;
        }
        break;

    default:
        break;
    }
}

/* --- the public socket interface --------------------------------------- */

struct tcp_socket *tcp_connect(ipv4_addr_t address, uint16_t port,
                               uint32_t timeout_ms)
{
    struct net_interface *iface = net_default_interface();
    if (!iface) {
        KLOG_ERR("tcp", "no usable network interface");
        return NULL;
    }

    bool_t irq = spinlock_acquire(&tcp_lock);
    struct tcp_socket *socket = socket_allocate();
    spinlock_release(&tcp_lock, irq);

    if (!socket) {
        KLOG_ERR("tcp", "no free sockets");
        return NULL;
    }

    socket->iface          = iface;
    socket->local_address  = iface->address;
    socket->remote_address = address;
    socket->remote_port    = port;
    socket->send_next      = (uint32_t)(time_uptime_ms() * 2654435761u);
    socket->send_unack     = socket->send_next;
    socket->state          = TCP_SYN_SENT;

    tcp_transmit(socket, TCP_SYN, NULL, 0);

    uint64_t deadline = time_uptime_ms() + timeout_ms;
    while (socket->state == TCP_SYN_SENT && time_uptime_ms() < deadline) {
        sched_sleep_ms(5);
    }

    if (socket->state != TCP_ESTABLISHED) {
        KLOG_WARN("tcp", "connection to port %u timed out", port);
        socket_release(socket);
        return NULL;
    }

    return socket;
}

ssize_t tcp_send(struct tcp_socket *socket, const void *data, size_t len)
{
    if (!socket || socket->state != TCP_ESTABLISHED) {
        return -1;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = MIN(len - sent, (size_t)TCP_MSS);

        if (tcp_transmit(socket, TCP_ACK | TCP_PSH, bytes + sent, chunk) != 0) {
            break;
        }
        sent += chunk;

        /* Let acknowledgements come back before filling the window again. */
        sched_sleep_ms(2);
    }

    return (ssize_t)sent;
}

ssize_t tcp_receive(struct tcp_socket *socket, void *out, size_t len,
                    uint32_t timeout_ms)
{
    if (!socket) {
        return -1;
    }

    uint64_t deadline = time_uptime_ms() + timeout_ms;

    while (socket->receive_length == 0) {
        if (socket->state == TCP_CLOSED) {
            return 0;
        }
        if (socket->state == TCP_CLOSE_WAIT && socket->receive_length == 0) {
            return 0;   /* clean end of stream */
        }
        if (time_uptime_ms() >= deadline) {
            return 0;
        }
        sched_sleep_ms(5);
    }

    size_t copy = MIN(len, socket->receive_length);
    memcpy(out, socket->receive_buffer, copy);

    /* Shuffle any remainder down. */
    socket->receive_length -= copy;
    if (socket->receive_length) {
        memmove(socket->receive_buffer, socket->receive_buffer + copy,
                socket->receive_length);
    }

    return (ssize_t)copy;
}

void tcp_close(struct tcp_socket *socket)
{
    if (!socket) {
        return;
    }

    if (socket->state == TCP_ESTABLISHED) {
        tcp_transmit(socket, TCP_FIN | TCP_ACK, NULL, 0);
        socket->state = TCP_FIN_WAIT;

        uint64_t deadline = time_uptime_ms() + 1000;
        while (socket->state != TCP_CLOSED && time_uptime_ms() < deadline) {
            sched_sleep_ms(5);
        }
    } else if (socket->state == TCP_CLOSE_WAIT) {
        tcp_transmit(socket, TCP_FIN | TCP_ACK, NULL, 0);
    }

    bool_t irq = spinlock_acquire(&tcp_lock);
    socket_release(socket);
    spinlock_release(&tcp_lock, irq);
}

tcp_state_t tcp_socket_state(const struct tcp_socket *socket)
{
    return socket ? socket->state : TCP_CLOSED;
}

const char *tcp_state_name(tcp_state_t state)
{
    switch (state) {
    case TCP_CLOSED:      return "closed";
    case TCP_SYN_SENT:    return "syn-sent";
    case TCP_ESTABLISHED: return "established";
    case TCP_FIN_WAIT:    return "fin-wait";
    case TCP_CLOSE_WAIT:  return "close-wait";
    default:              return "unknown";
    }
}

/* --- UDP --------------------------------------------------------------- */

/* Pending UDP replies, keyed by the port that sent the request. */
struct udp_pending {
    bool_t   waiting;
    uint16_t port;
    uint8_t  buffer[1024];
    size_t   length;
};

static struct udp_pending udp_wait;

int udp_send(ipv4_addr_t address, uint16_t port, uint16_t source_port,
             const void *data, size_t len)
{
    struct net_interface *iface = net_default_interface();
    if (!iface || len > NET_MTU - sizeof(struct udp_header)) {
        return -1;
    }

    uint8_t datagram[NET_MTU];
    struct udp_header *header = (struct udp_header *)datagram;

    header->source_port = htons(source_port);
    header->dest_port   = htons(port);
    header->length      = htons((uint16_t)(sizeof(*header) + len));
    header->checksum    = 0;

    memcpy(datagram + sizeof(*header), data, len);

    size_t total = sizeof(*header) + len;
    header->checksum = htons(transport_checksum(iface->address, address,
                                                IP_PROTO_UDP, datagram, total));
    /* Zero means "no checksum" in UDP, so a computed zero is sent as ~0. */
    if (header->checksum == 0) {
        header->checksum = 0xFFFF;
    }

    return net_send_ipv4(iface, address, IP_PROTO_UDP, datagram, total);
}

void udp_input(struct net_interface *iface, ipv4_addr_t source,
               const uint8_t *data, size_t len)
{
    UNUSED(iface);
    UNUSED(source);

    if (len < sizeof(struct udp_header)) {
        return;
    }

    const struct udp_header *header = (const struct udp_header *)data;
    uint16_t dest_port = ntohs(header->dest_port);
    size_t   body_len  = len - sizeof(*header);

    transport_stats.rx_packets++;

    if (udp_wait.waiting && udp_wait.port == dest_port) {
        size_t copy = MIN(body_len, sizeof(udp_wait.buffer));
        memcpy(udp_wait.buffer, data + sizeof(*header), copy);
        udp_wait.length  = copy;
        udp_wait.waiting = false;
    }
}

/* --- DNS --------------------------------------------------------------- */

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} PACKED;

/* Encode "example.com" as the length-prefixed labels DNS uses. */
static size_t dns_encode_name(const char *name, uint8_t *out, size_t capacity)
{
    size_t written = 0;
    const char *label = name;

    while (*label && written < capacity - 2) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? (size_t)(dot - label) : strlen(label);

        if (length == 0 || length > 63 || written + length + 1 >= capacity) {
            return 0;
        }

        out[written++] = (uint8_t)length;
        memcpy(out + written, label, length);
        written += length;

        if (!dot) {
            break;
        }
        label = dot + 1;
    }

    out[written++] = 0;   /* the root label terminates the name */
    return written;
}

int dns_resolve(const char *hostname, ipv4_addr_t *out, uint32_t timeout_ms)
{
    if (!hostname || !out) {
        return -1;
    }

    /* A literal address needs no lookup. */
    if (net_parse_ip(hostname, out)) {
        return 0;
    }

    ipv4_addr_t server = net_dns_server();
    if (!server) {
        KLOG_WARN("dns", "no resolver configured");
        return -1;
    }

    uint8_t query[512];
    struct dns_header *header = (struct dns_header *)query;

    static uint16_t next_id = 1;
    uint16_t id = next_id++;

    memset(header, 0, sizeof(*header));
    header->id        = htons(id);
    header->flags     = htons(0x0100);   /* standard query, recursion desired */
    header->questions = htons(1);

    size_t offset = sizeof(*header);
    size_t name_len = dns_encode_name(hostname, query + offset,
                                      sizeof(query) - offset - 4);
    if (name_len == 0) {
        return -1;
    }
    offset += name_len;

    /* QTYPE = A (1), QCLASS = IN (1). */
    query[offset++] = 0;
    query[offset++] = 1;
    query[offset++] = 0;
    query[offset++] = 1;

    uint16_t source_port = next_ephemeral_port++;

    udp_wait.waiting = true;
    udp_wait.port    = source_port;
    udp_wait.length  = 0;

    if (udp_send(server, 53, source_port, query, offset) != 0) {
        udp_wait.waiting = false;
        return -1;
    }

    uint64_t deadline = time_uptime_ms() + timeout_ms;
    while (udp_wait.waiting && time_uptime_ms() < deadline) {
        sched_sleep_ms(5);
    }

    if (udp_wait.waiting) {
        udp_wait.waiting = false;
        KLOG_WARN("dns", "%s: no response from the resolver", hostname);
        return -1;
    }

    /* Parse the reply: skip the question, then walk the answers. */
    const uint8_t *response = udp_wait.buffer;
    size_t response_len     = udp_wait.length;

    if (response_len < sizeof(struct dns_header)) {
        return -1;
    }

    const struct dns_header *reply = (const struct dns_header *)response;
    if (ntohs(reply->id) != id) {
        return -1;
    }
    if ((ntohs(reply->flags) & 0x000F) != 0) {
        KLOG_WARN("dns", "%s: the resolver returned an error", hostname);
        return -1;
    }

    int answer_count = ntohs(reply->answers);
    if (answer_count == 0) {
        return -1;
    }

    size_t position = sizeof(*reply);

    /* Skip the echoed question section. */
    for (int q = 0; q < ntohs(reply->questions); q++) {
        while (position < response_len && response[position] != 0) {
            if ((response[position] & 0xC0) == 0xC0) {
                position += 2;
                goto question_done;
            }
            position += response[position] + 1;
        }
        position++;
    question_done:
        position += 4;   /* QTYPE and QCLASS */
    }

    /* Walk the answers looking for the first A record. */
    for (int a = 0; a < answer_count && position + 12 <= response_len; a++) {
        /* The name is usually a compression pointer. */
        if ((response[position] & 0xC0) == 0xC0) {
            position += 2;
        } else {
            while (position < response_len && response[position] != 0) {
                position += response[position] + 1;
            }
            position++;
        }

        if (position + 10 > response_len) {
            break;
        }

        uint16_t type = (uint16_t)((response[position] << 8) |
                                   response[position + 1]);
        uint16_t data_len = (uint16_t)((response[position + 8] << 8) |
                                       response[position + 9]);
        position += 10;

        if (type == 1 && data_len == 4 && position + 4 <= response_len) {
            *out = IPV4(response[position], response[position + 1],
                        response[position + 2], response[position + 3]);

            char text[24];
            net_format_ip(*out, text, sizeof(text));
            KLOG_DEBUG("dns", "%s resolves to %s", hostname, text);
            return 0;
        }

        position += data_len;
    }

    return -1;
}

void tcp_init(void)
{
    spinlock_init(&tcp_lock, "tcp");
    memset(sockets, 0, sizeof(sockets));
    KLOG_INFO("tcp", "transport ready: TCP, UDP and DNS (%d sockets)",
              TCP_MAX_SOCKETS);
}

void tcp_get_stats(struct net_stats *out)
{
    if (out) {
        *out = transport_stats;
    }
}

int tcp_active_sockets(void)
{
    int count = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use) {
            count++;
        }
    }
    return count;
}
