#include <kernel/drivers/net/tcp.h>
#include <kernel/drivers/net/ipv4.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <kernel/pit.h>
#include <string.h>

static tcp_socket_t *tcp_sockets = NULL;
static uint16_t tcp_ephemeral_port = 49152;
static uint32_t tcp_isn = 12345678;

void tcp_init(void) {
    tcp_sockets = NULL;
    tcp_ephemeral_port = 49152;
}

static uint16_t tcp_checksum(ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint32_t len) {
    uint32_t sum = 0;

    sum += (src.octets[0] << 8) | src.octets[1];
    sum += (src.octets[2] << 8) | src.octets[3];
    sum += (dest.octets[0] << 8) | dest.octets[1];
    sum += (dest.octets[2] << 8) | dest.octets[3];
    sum += IP_PROTO_TCP;
    sum += len;

    uint16_t *ptr = (uint16_t *)data;
    uint32_t remaining = len;
    while (remaining > 1) {
        sum += ntohs(*ptr++);
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += (*((uint8_t *)ptr)) << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons(~sum);
}

static int tcp_send_segment(tcp_socket_t *sock, uint8_t flags, uint8_t *data, uint32_t len) {
    if (!sock || !sock->netif) return -1;

    uint32_t header_len = 20;
    uint32_t tcp_len = header_len + len;
    uint8_t *packet = (uint8_t *)kmalloc(tcp_len);
    if (!packet) return -1;

    tcp_header_t *tcp = (tcp_header_t *)packet;
    memset(tcp, 0, sizeof(tcp_header_t));

    tcp->src_port = htons(sock->local_port);
    tcp->dest_port = htons(sock->remote_port);
    tcp->seq_num = htonl(sock->send_next);
    tcp->ack_num = htonl(sock->recv_next);
    tcp->data_offset = (header_len / 4) << 4;
    tcp->flags = flags;
    tcp->window = htons(sock->recv_window);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (data && len > 0) {
        memcpy(packet + header_len, data, len);
    }

    tcp->checksum = tcp_checksum(sock->local_ip, sock->remote_ip, packet, tcp_len);

    int result = ipv4_send(sock->netif, sock->remote_ip, IP_PROTO_TCP, packet, tcp_len);
    kfree(packet);

    if (result == 0 && (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))) {
        sock->send_next++;
    }
    if (result == 0 && len > 0) {
        sock->send_next += len;
    }

    return result;
}

tcp_socket_t *tcp_socket_create(void) {
    tcp_socket_t *sock = (tcp_socket_t *)kmalloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;

    memset(sock, 0, sizeof(tcp_socket_t));

    sock->state = TCP_STATE_CLOSED;
    sock->local_port = tcp_ephemeral_port++;
    sock->recv_window = TCP_WINDOW_SIZE;
    sock->send_window = TCP_WINDOW_SIZE;

    sock->recv_buffer_size = 16384;
    sock->recv_buffer = (uint8_t *)kmalloc(sock->recv_buffer_size);
    if (!sock->recv_buffer) {
        kfree(sock);
        return NULL;
    }

    sock->send_buffer_size = 16384;
    sock->send_buffer = (uint8_t *)kmalloc(sock->send_buffer_size);
    if (!sock->send_buffer) {
        kfree(sock->recv_buffer);
        kfree(sock);
        return NULL;
    }

    sock->netif = netif_get_default();
    sock->next = tcp_sockets;
    tcp_sockets = sock;

    return sock;
}

void tcp_socket_close(tcp_socket_t *sock) {
    if (!sock) return;

    tcp_socket_t **prev = &tcp_sockets;
    while (*prev) {
        if (*prev == sock) {
            *prev = sock->next;
            break;
        }
        prev = &(*prev)->next;
    }

    if (sock->recv_buffer) kfree(sock->recv_buffer);
    if (sock->send_buffer) kfree(sock->send_buffer);
    kfree(sock);
}

int tcp_connect(tcp_socket_t *sock, ipv4_addr_t dest, uint16_t port, uint32_t timeout_ms) {
    if (!sock || sock->state != TCP_STATE_CLOSED) return -1;

    sock->remote_ip = dest;
    sock->remote_port = port;
    sock->local_ip = sock->netif->ipv4;
    sock->send_next = tcp_isn++;
    sock->send_una = sock->send_next;
    sock->recv_next = 0;

    sock->state = TCP_STATE_SYN_SENT;

    if (tcp_send_segment(sock, TCP_FLAG_SYN, NULL, 0) < 0) {
        sock->state = TCP_STATE_CLOSED;
        return -1;
    }

    uint32_t timeout_ticks = pit_ms_to_ticks(timeout_ms);
    uint32_t start = pit_get_ticks();
    while (sock->state == TCP_STATE_SYN_SENT) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (pit_get_ticks() - start > timeout_ticks) {
            sock->state = TCP_STATE_CLOSED;
            return -1;
        }
        __asm__ volatile("hlt");
    }

    return sock->state == TCP_STATE_ESTABLISHED ? 0 : -1;
}

int tcp_send(tcp_socket_t *sock, uint8_t *data, uint32_t len) {
    if (!sock || sock->state != TCP_STATE_ESTABLISHED) return -1;

    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        if (tcp_send_segment(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, data + sent, chunk) < 0) {
            return sent > 0 ? (int)sent : -1;
        }
        sent += chunk;

        __asm__ volatile("sti");
        __asm__ volatile("hlt");
        netif_poll_all();
    }

    return sent;
}

int tcp_recv(tcp_socket_t *sock, uint8_t *buffer, uint32_t len, uint32_t timeout_ms) {
    if (!sock) return -1;
    if (sock->state != TCP_STATE_ESTABLISHED &&
        sock->state != TCP_STATE_FIN_WAIT1 &&
        sock->state != TCP_STATE_FIN_WAIT2 &&
        sock->state != TCP_STATE_CLOSE_WAIT) {
        return -1;
    }

    uint32_t timeout_ticks = pit_ms_to_ticks(timeout_ms);
    uint32_t start = pit_get_ticks();

    while (sock->recv_buffer_head == sock->recv_buffer_tail) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (pit_get_ticks() - start > timeout_ticks) {
            return 0;
        }
        if (sock->state == TCP_STATE_CLOSE_WAIT ||
            sock->state == TCP_STATE_CLOSED) {
            break;
        }
        __asm__ volatile("hlt");
    }

    uint32_t available = 0;
    if (sock->recv_buffer_tail >= sock->recv_buffer_head) {
        available = sock->recv_buffer_tail - sock->recv_buffer_head;
    } else {
        available = sock->recv_buffer_size - sock->recv_buffer_head + sock->recv_buffer_tail;
    }

    if (available == 0) return 0;

    uint32_t to_copy = available < len ? available : len;
    uint32_t copied = 0;

    while (copied < to_copy) {
        buffer[copied++] = sock->recv_buffer[sock->recv_buffer_head];
        sock->recv_buffer_head = (sock->recv_buffer_head + 1) % sock->recv_buffer_size;
    }

    return copied;
}

int tcp_disconnect(tcp_socket_t *sock, uint32_t timeout_ms) {
    if (!sock) return -1;

    if (sock->state == TCP_STATE_ESTABLISHED) {
        sock->state = TCP_STATE_FIN_WAIT1;
        tcp_send_segment(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);

        uint32_t timeout_ticks = pit_ms_to_ticks(timeout_ms);
        uint32_t start = pit_get_ticks();
        while (sock->state != TCP_STATE_CLOSED &&
               sock->state != TCP_STATE_TIME_WAIT) {
            __asm__ volatile("sti");
            netif_poll_all();
            if (pit_get_ticks() - start > timeout_ticks) {
                sock->state = TCP_STATE_CLOSED;
                return -1;
            }
            __asm__ volatile("hlt");
        }
    }

    sock->state = TCP_STATE_CLOSED;
    return 0;
}

void tcp_receive(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint32_t len) {
    if (!netif || !data || len < sizeof(tcp_header_t)) return;

    tcp_header_t *tcp = (tcp_header_t *)data;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dest_port = ntohs(tcp->dest_port);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;
    uint32_t header_len = ((tcp->data_offset >> 4) & 0x0F) * 4;

    if (header_len < 20 || header_len > len) return;

    uint8_t *payload = data + header_len;
    uint32_t payload_len = len - header_len;

    tcp_socket_t *sock = tcp_sockets;
    while (sock) {
        if (sock->local_port == dest_port &&
            sock->remote_port == src_port &&
            ipv4_eq(sock->remote_ip, src)) {
            break;
        }
        sock = sock->next;
    }

    if (!sock) return;

    sock->send_window = ntohs(tcp->window);

    switch (sock->state) {
        case TCP_STATE_SYN_SENT:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->recv_next = seq + 1;
                sock->send_una = ack;
                sock->state = TCP_STATE_ESTABLISHED;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_FLAG_FIN) {
                sock->recv_next = seq + 1;
                sock->state = TCP_STATE_CLOSE_WAIT;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
            } else if (flags & TCP_FLAG_ACK) {
                if (payload_len > 0) {
                    for (uint32_t i = 0; i < payload_len; i++) {
                        uint32_t next = (sock->recv_buffer_tail + 1) % sock->recv_buffer_size;
                        if (next != sock->recv_buffer_head) {
                            sock->recv_buffer[sock->recv_buffer_tail] = payload[i];
                            sock->recv_buffer_tail = next;
                        }
                    }
                    sock->recv_next = seq + payload_len;
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                }
                sock->send_una = ack;
            }
            break;

        case TCP_STATE_FIN_WAIT1:
            if (flags & TCP_FLAG_ACK) {
                sock->send_una = ack;
                if (flags & TCP_FLAG_FIN) {
                    sock->recv_next = seq + 1;
                    sock->state = TCP_STATE_TIME_WAIT;
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                } else {
                    sock->state = TCP_STATE_FIN_WAIT2;
                }
            }
            break;

        case TCP_STATE_FIN_WAIT2:
            if (flags & TCP_FLAG_FIN) {
                sock->recv_next = seq + 1;
                sock->state = TCP_STATE_TIME_WAIT;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_CLOSE_WAIT:
            break;

        case TCP_STATE_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_STATE_CLOSED;
            }
            break;

        default:
            break;
    }
}

void tcp_tick(void) {
    tcp_socket_t *sock = tcp_sockets;
    while (sock) {
        if (sock->state == TCP_STATE_TIME_WAIT) {
            sock->state = TCP_STATE_CLOSED;
        }
        sock = sock->next;
    }
}
