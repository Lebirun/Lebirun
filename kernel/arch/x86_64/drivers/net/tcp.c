#include <kernel/drivers/net/tcp.h>
#include <kernel/drivers/net/ipv4.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <kernel/pit.h>
#include <kernel/task.h>
#include <string.h>

static tcp_socket_t *tcp_sockets = NULL;
static uint16_t tcp_ephemeral_port = 49152;
static uint32_t tcp_isn = 0;

#define TCP_RETX_TIMEOUT_MS 500
#define TCP_RETX_MAX_RETRIES 8

static void tcp_retx_queue_add(tcp_socket_t *sock, uint8_t *data, uint64_t len, uint32_t seq) {
    tcp_retx_seg_t *seg;

    seg = (tcp_retx_seg_t *)kmalloc(sizeof(tcp_retx_seg_t));
    if (!seg) return;
    seg->data = (uint8_t *)kmalloc(len);
    if (!seg->data) {
        kfree(seg);
        return;
    }
    memcpy(seg->data, data, len);
    seg->len = len;
    seg->seq = seq;
    seg->send_time = pit_get_ticks();
    seg->retries = 0;
    seg->next = NULL;

    if (sock->retx_tail) {
        sock->retx_tail->next = seg;
    } else {
        sock->retx_head = seg;
    }
    sock->retx_tail = seg;
    sock->retx_count++;
}

static void tcp_retx_queue_ack(tcp_socket_t *sock, uint32_t ack_num) {
    tcp_retx_seg_t *seg;

    while (sock->retx_head) {
        seg = sock->retx_head;
        if ((int32_t)(ack_num - (seg->seq + seg->len)) < 0)
            break;
        sock->retx_head = seg->next;
        if (!sock->retx_head)
            sock->retx_tail = NULL;
        sock->retx_count--;
        kfree(seg->data);
        kfree(seg);
    }
}

static void tcp_retx_queue_free(tcp_socket_t *sock) {
    tcp_retx_seg_t *seg;
    tcp_retx_seg_t *next;

    seg = sock->retx_head;
    while (seg) {
        next = seg->next;
        kfree(seg->data);
        kfree(seg);
        seg = next;
    }
    sock->retx_head = NULL;
    sock->retx_tail = NULL;
    sock->retx_count = 0;
}

void tcp_init(void) {
    tcp_sockets = NULL;
    tcp_ephemeral_port = 49152;
    tcp_isn = pit_get_ticks() * 2654435761u;
}

static uint16_t tcp_alloc_port(void) {
    uint16_t port = tcp_ephemeral_port++;
    if (tcp_ephemeral_port >= 65500) {
        tcp_ephemeral_port = 49152;
    }
    return port;
}

static uint16_t tcp_checksum(ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint64_t len) {
    uint64_t sum;
    uint16_t *ptr;
    uint64_t remaining;

    sum = 0;

    sum += (src.octets[0] << 8) | src.octets[1];
    sum += (src.octets[2] << 8) | src.octets[3];
    sum += (dest.octets[0] << 8) | dest.octets[1];
    sum += (dest.octets[2] << 8) | dest.octets[3];
    sum += IP_PROTO_TCP;
    sum += len;

    ptr = (uint16_t *)data;
    remaining = len;
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

static int tcp_send_segment(tcp_socket_t *sock, uint8_t flags, uint8_t *data, uint64_t len) {
    uint64_t header_len;
    uint64_t tcp_len;
    uint8_t *packet;
    tcp_header_t *tcp;
    int result;
    uint64_t used;
    uint64_t free_space;

    if (!sock || !sock->netif) return -1;

    header_len = 20;
    tcp_len = header_len + len;
    packet = (uint8_t *)kmalloc(tcp_len);
    if (!packet) return -1;

    if (sock->recv_buffer_size > 0) {
        if (sock->recv_buffer_tail >= sock->recv_buffer_head) {
            used = sock->recv_buffer_tail - sock->recv_buffer_head;
        } else {
            used = sock->recv_buffer_size - sock->recv_buffer_head + sock->recv_buffer_tail;
        }
        free_space = sock->recv_buffer_size - 1 - used;
        if (free_space > 65535) free_space = 65535;
        sock->recv_window = (uint16_t)free_space;
    }

    tcp = (tcp_header_t *)packet;
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

    result = ipv4_send(sock->netif, sock->remote_ip, IP_PROTO_TCP, packet, tcp_len);
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
    tcp_socket_t *sock;

    sock = (tcp_socket_t *)kmalloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;

    memset(sock, 0, sizeof(tcp_socket_t));

    sock->state = TCP_STATE_CLOSED;
    sock->local_port = tcp_alloc_port();
    sock->recv_window = TCP_WINDOW_SIZE;
    sock->send_window = TCP_WINDOW_SIZE;

    sock->recv_buffer_size = 16384;
    sock->recv_buffer = (uint8_t *)kmalloc(sock->recv_buffer_size);
    if (!sock->recv_buffer) {
        kfree(sock);
        return NULL;
    }

    sock->send_buffer_size = 8192;
    sock->send_buffer = (uint8_t *)kmalloc(sock->send_buffer_size);
    if (!sock->send_buffer) {
        kfree(sock->recv_buffer);
        kfree(sock);
        return NULL;
    }

    sock->netif = netif_get_default();
    sock->retx_head = NULL;
    sock->retx_tail = NULL;
    sock->retx_count = 0;
    sock->retransmit_timeout = pit_ms_to_ticks(TCP_RETX_TIMEOUT_MS);
    sock->last_ack_time = pit_get_ticks();
    sock->next = tcp_sockets;
    tcp_sockets = sock;

    return sock;
}

void tcp_socket_close(tcp_socket_t *sock) {
    tcp_socket_t **prev;

    if (!sock) return;

    prev = &tcp_sockets;
    while (*prev) {
        if (*prev == sock) {
            *prev = sock->next;
            break;
        }
        prev = &(*prev)->next;
    }

    tcp_retx_queue_free(sock);
    if (sock->recv_buffer) kfree(sock->recv_buffer);
    if (sock->send_buffer) kfree(sock->send_buffer);
    kfree(sock);
}

int tcp_connect(tcp_socket_t *sock, ipv4_addr_t dest, uint16_t port, uint64_t timeout_ms) {
    uint64_t timeout_ticks;
    uint64_t start;
    uint64_t last_syn;
    uint64_t syn_interval;
    uint32_t syn_saved;

    if (!sock || sock->state != TCP_STATE_CLOSED) return -1;

    sock->remote_ip = dest;
    sock->remote_port = port;
    sock->local_ip = sock->netif->ipv4;
    sock->send_next = tcp_isn;
    tcp_isn += pit_get_ticks() + 64000;
    sock->send_una = sock->send_next;
    sock->recv_next = 0;

    sock->state = TCP_STATE_SYN_SENT;

    if (tcp_send_segment(sock, TCP_FLAG_SYN, NULL, 0) < 0) {
        sock->state = TCP_STATE_CLOSED;
        return -1;
    }

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();
    last_syn = start;
    syn_interval = pit_ms_to_ticks(500);
    syn_saved = sock->send_next - 1;
    while (sock->state == TCP_STATE_SYN_SENT) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (task_has_pending_signals()) {
            sock->state = TCP_STATE_CLOSED;
            return -1;
        }
        if (pit_get_ticks() - start > timeout_ticks) {
            sock->state = TCP_STATE_CLOSED;
            return -1;
        }
        if (pit_get_ticks() - last_syn > syn_interval) {
            sock->send_next = syn_saved;
            tcp_send_segment(sock, TCP_FLAG_SYN, NULL, 0);
            last_syn = pit_get_ticks();
        }
        schedule();
    }

    return sock->state == TCP_STATE_ESTABLISHED ? 0 : -1;
}

int tcp_send(tcp_socket_t *sock, uint8_t *data, uint64_t len) {
    uint64_t sent;
    uint64_t chunk;
    uint32_t seq_before;

    if (!sock || sock->state != TCP_STATE_ESTABLISHED) return -1;

    sent = 0;
    while (sent < len) {
        chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        seq_before = sock->send_next;
        if (tcp_send_segment(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, data + sent, chunk) < 0) {
            return sent > 0 ? (int)sent : -1;
        }
        tcp_retx_queue_add(sock, data + sent, chunk, seq_before);
        sent += chunk;

        __asm__ volatile("sti");
        netif_poll_all();
        schedule();
    }

    return sent;
}

int tcp_recv(tcp_socket_t *sock, uint8_t *buffer, uint64_t len, uint64_t timeout_ms) {
    uint64_t timeout_ticks;
    uint64_t start;
    uint64_t available;
    uint64_t to_copy;
    uint64_t copied;
    uint16_t old_window;

    if (!sock) return -1;
    if (sock->state != TCP_STATE_ESTABLISHED &&
        sock->state != TCP_STATE_FIN_WAIT1 &&
        sock->state != TCP_STATE_FIN_WAIT2 &&
        sock->state != TCP_STATE_CLOSE_WAIT) {
        return -1;
    }

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();

    while (sock->recv_buffer_head == sock->recv_buffer_tail) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (task_has_pending_signals()) {
            return -1;
        }
        if (pit_get_ticks() - start > timeout_ticks) {
            return 0;
        }
        if (sock->state == TCP_STATE_CLOSE_WAIT ||
            sock->state == TCP_STATE_CLOSED) {
            break;
        }
        schedule();
    }

    available = 0;
    if (sock->recv_buffer_tail >= sock->recv_buffer_head) {
        available = sock->recv_buffer_tail - sock->recv_buffer_head;
    } else {
        available = sock->recv_buffer_size - sock->recv_buffer_head + sock->recv_buffer_tail;
    }

    if (available == 0) return 0;

    old_window = sock->recv_window;
    to_copy = available < len ? available : len;
    copied = 0;

    while (copied < to_copy) {
        buffer[copied++] = sock->recv_buffer[sock->recv_buffer_head];
        sock->recv_buffer_head = (sock->recv_buffer_head + 1) % sock->recv_buffer_size;
    }

    if (old_window < 4096 && sock->state == TCP_STATE_ESTABLISHED) {
        tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
    }

    return copied;
}

int tcp_disconnect(tcp_socket_t *sock, uint64_t timeout_ms) {
    uint64_t timeout_ticks;
    uint64_t start;

    if (!sock) return -1;

    if (sock->state == TCP_STATE_ESTABLISHED) {
        sock->state = TCP_STATE_FIN_WAIT1;
        sock->fin_send_time = pit_get_ticks();
        sock->fin_retries = 0;
        tcp_send_segment(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);

        timeout_ticks = pit_ms_to_ticks(timeout_ms);
        start = pit_get_ticks();
        while (sock->state != TCP_STATE_CLOSED &&
               sock->state != TCP_STATE_TIME_WAIT) {
            __asm__ volatile("sti");
            netif_poll_all();
            if (task_has_pending_signals()) {
                sock->state = TCP_STATE_CLOSED;
                return -1;
            }
            if (pit_get_ticks() - start > timeout_ticks) {
                sock->state = TCP_STATE_CLOSED;
                return -1;
            }
            schedule();
        }
    }

    sock->state = TCP_STATE_CLOSED;
    return 0;
}

#define TCP_RECV_BUF_MAX 1048576

static void tcp_grow_recv_buffer(tcp_socket_t *sock) {
    uint64_t old_size;
    uint64_t new_size;
    uint8_t *new_buf;
    uint64_t used;
    uint64_t i;

    old_size = sock->recv_buffer_size;
    new_size = old_size * 2;
    if (new_size > TCP_RECV_BUF_MAX) new_size = TCP_RECV_BUF_MAX;
    if (new_size <= old_size) return;
    new_buf = (uint8_t *)kmalloc(new_size);
    if (!new_buf) return;
    used = 0;
    if (sock->recv_buffer_tail >= sock->recv_buffer_head) {
        used = sock->recv_buffer_tail - sock->recv_buffer_head;
    } else {
        used = old_size - sock->recv_buffer_head + sock->recv_buffer_tail;
    }
    for (i = 0; i < used; i++) {
        new_buf[i] = sock->recv_buffer[(sock->recv_buffer_head + i) % old_size];
    }
    kfree(sock->recv_buffer);
    sock->recv_buffer = new_buf;
    sock->recv_buffer_size = new_size;
    sock->recv_buffer_head = 0;
    sock->recv_buffer_tail = used;
}

void tcp_receive(netif_t *netif, ipv4_addr_t src, ipv4_addr_t dest, uint8_t *data, uint64_t len) {
    tcp_header_t *tcp;
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    uint64_t header_len;
    uint8_t *payload;
    uint64_t payload_len;
    tcp_socket_t *sock;
    uint64_t avail;
    uint64_t used;
    uint64_t i;
    uint64_t next;

    (void)dest;
    if (!netif || !data || len < sizeof(tcp_header_t)) return;

    tcp = (tcp_header_t *)data;
    src_port = ntohs(tcp->src_port);
    dest_port = ntohs(tcp->dest_port);
    seq = ntohl(tcp->seq_num);
    ack = ntohl(tcp->ack_num);
    flags = tcp->flags;
    header_len = ((tcp->data_offset >> 4) & 0x0F) * 4;

    if (header_len < 20 || header_len > len) return;

    payload = data + header_len;
    payload_len = len - header_len;

    sock = tcp_sockets;
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
            if (flags & TCP_FLAG_RST) {
                sock->state = TCP_STATE_CLOSED;
                tcp_retx_queue_free(sock);
                break;
            }
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->recv_next = seq + 1;
                sock->send_una = ack;
                sock->state = TCP_STATE_ESTABLISHED;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_FLAG_RST) {
                sock->state = TCP_STATE_CLOSED;
                tcp_retx_queue_free(sock);
                break;
            }
            if (flags & TCP_FLAG_ACK) {
                sock->send_una = ack;
                sock->last_ack_time = pit_get_ticks();
                tcp_retx_queue_ack(sock, ack);
            }
            if (payload_len > 0) {
                if (seq != sock->recv_next) {
                    if (seq + payload_len <= sock->recv_next) {
                        tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                        break;
                    }
                    if (seq < sock->recv_next) {
                        payload += (sock->recv_next - seq);
                        payload_len -= (sock->recv_next - seq);
                        seq = sock->recv_next;
                    } else {
                        tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                        break;
                    }
                }
                if (sock->recv_buffer_tail >= sock->recv_buffer_head) {
                    used = sock->recv_buffer_tail - sock->recv_buffer_head;
                } else {
                    used = sock->recv_buffer_size - sock->recv_buffer_head + sock->recv_buffer_tail;
                }
                avail = sock->recv_buffer_size - 1 - used;
                if (avail < payload_len && sock->recv_buffer_size < TCP_RECV_BUF_MAX) {
                    tcp_grow_recv_buffer(sock);
                }
                for (i = 0; i < payload_len; i++) {
                    next = (sock->recv_buffer_tail + 1) % sock->recv_buffer_size;
                    if (next != sock->recv_buffer_head) {
                        sock->recv_buffer[sock->recv_buffer_tail] = payload[i];
                        sock->recv_buffer_tail = next;
                    }
                }
                sock->recv_next = seq + payload_len;
            }
            if (flags & TCP_FLAG_FIN) {
                if (payload_len == 0 && seq != sock->recv_next) {
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                    break;
                }
                sock->recv_next += 1;
                sock->state = TCP_STATE_CLOSE_WAIT;
            }
            if (payload_len > 0 || (flags & TCP_FLAG_FIN))
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
            break;

        case TCP_STATE_FIN_WAIT1:
            if (flags & TCP_FLAG_RST) {
                sock->state = TCP_STATE_CLOSED;
                tcp_retx_queue_free(sock);
                break;
            }
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
            if (flags & TCP_FLAG_RST) {
                sock->state = TCP_STATE_CLOSED;
                tcp_retx_queue_free(sock);
                break;
            }
            if (flags & TCP_FLAG_FIN) {
                sock->recv_next = seq + 1;
                sock->state = TCP_STATE_TIME_WAIT;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_CLOSE_WAIT:
            if (flags & TCP_FLAG_ACK)
                sock->send_una = ack;
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
    tcp_socket_t *sock;
    tcp_retx_seg_t *seg;
    uint64_t now;
    uint64_t timeout_ticks;
    uint32_t saved_send_next;

    sock = tcp_sockets;
    now = pit_get_ticks();
    while (sock) {
        if (sock->state == TCP_STATE_TIME_WAIT) {
            sock->state = TCP_STATE_CLOSED;
        }
        if (sock->state == TCP_STATE_ESTABLISHED && sock->retx_head) {
            seg = sock->retx_head;
            timeout_ticks = sock->retransmit_timeout << seg->retries;
            if (now - seg->send_time > timeout_ticks) {
                if (seg->retries >= TCP_RETX_MAX_RETRIES) {
                    sock->state = TCP_STATE_CLOSED;
                    tcp_retx_queue_free(sock);
                } else {
                    saved_send_next = sock->send_next;
                    sock->send_next = seg->seq;
                    tcp_send_segment(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, seg->data, seg->len);
                    sock->send_next = saved_send_next;
                    seg->send_time = now;
                    seg->retries++;
                }
            }
        }
        if (sock->state == TCP_STATE_FIN_WAIT1) {
            timeout_ticks = sock->retransmit_timeout << sock->fin_retries;
            if (now - sock->fin_send_time > timeout_ticks) {
                if (sock->fin_retries >= TCP_RETX_MAX_RETRIES) {
                    sock->state = TCP_STATE_CLOSED;
                } else {
                    tcp_send_segment(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
                    sock->fin_send_time = now;
                    sock->fin_retries++;
                }
            }
        }
        sock = sock->next;
    }
}
