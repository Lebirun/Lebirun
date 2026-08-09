#include <lebirun/drivers/net/icmp.h>
#include <lebirun/drivers/net/ipv4.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/pit.h>
#include <lebirun/keyboard.h>
#include <lebirun/task.h>
#include <string.h>

static ping_state_t g_ping_state;

void icmp_receive(netif_t *netif, ipv4_addr_t src, uint8_t *data, uint64_t len) {
    icmp_header_t *icmp;

    if (!netif || !data || len < sizeof(icmp_header_t)) return;

    icmp = (icmp_header_t *)data;

    if (ipv4_checksum(data, len) != 0) return;

    switch (icmp->type) {
        case ICMP_ECHO_REQUEST:
            icmp_send_echo_reply(netif, src, ntohs(icmp->identifier),
                                ntohs(icmp->sequence),
                                data + sizeof(icmp_header_t),
                                len - sizeof(icmp_header_t));
            break;

        case ICMP_ECHO_REPLY:
            if (g_ping_state.received == 0 &&
                ntohs(icmp->identifier) == g_ping_state.id &&
                ntohs(icmp->sequence) == g_ping_state.seq &&
                ipv4_eq(src, g_ping_state.target)) {
                g_ping_state.received = 1;
                g_ping_state.rtt = net_get_ticks() - g_ping_state.send_time;
            }
            break;

        default:
            break;
    }
}

static int icmp_send_echo(netif_t *netif, ipv4_addr_t dest, uint16_t id,
                          uint16_t seq, uint8_t *data, uint64_t len,
                          uint8_t type) {
    uint64_t icmp_len;
    uint8_t *packet;
    icmp_header_t *icmp;
    int result;

    if (!netif) return -1;

    icmp_len = sizeof(icmp_header_t) + len;
    packet = (uint8_t *)kmalloc(icmp_len);
    if (!packet) return -1;

    icmp = (icmp_header_t *)packet;
    icmp->type = type;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = htons(id);
    icmp->sequence = htons(seq);

    if (data && len > 0) {
        memcpy(packet + sizeof(icmp_header_t), data, len);
    }

    icmp->checksum = ipv4_checksum(packet, icmp_len);

    result = ipv4_send(netif, dest, IP_PROTO_ICMP, packet, icmp_len);
    kfree(packet);

    return result;
}

int icmp_send_echo_request(netif_t *netif, ipv4_addr_t dest, uint16_t id,
                           uint16_t seq, uint8_t *data, uint64_t len) {
    return icmp_send_echo(netif, dest, id, seq, data, len,
                          ICMP_ECHO_REQUEST);
}

int icmp_send_echo_reply(netif_t *netif, ipv4_addr_t dest, uint16_t id,
                         uint16_t seq, uint8_t *data, uint64_t len) {
    return icmp_send_echo(netif, dest, id, seq, data, len, ICMP_ECHO_REPLY);
}

int ping(ipv4_addr_t target, uint64_t count, uint64_t timeout_ms) {
    netif_t *netif;
    uint16_t id;
    uint64_t received;
    uint64_t total_rtt;
    uint64_t seq;
    uint64_t timeout_ticks;
    uint64_t start;
    uint8_t payload[56];
    int i;
    int key;

    netif = netif_get_default();
    if (!netif) {
        printf("ping: no network interface\n");
        return -1;
    }

    printf("PING %u.%u.%u.%u\n",
           target.octets[0], target.octets[1],
           target.octets[2], target.octets[3]);

    id = (uint16_t)(net_get_ticks() & 0xFFFF);
    received = 0;
    total_rtt = 0;
    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    for (i = 0; i < 56; i++) payload[i] = i;

    for (seq = 0; seq < count; seq++) {
        g_ping_state.target = target;
        g_ping_state.id = id;
        g_ping_state.seq = seq;
        g_ping_state.received = 0;
        g_ping_state.rtt = 0;
        g_ping_state.send_time = net_get_ticks();

        if (icmp_send_echo_request(netif, target, id, seq, payload, sizeof(payload)) < 0) {
            printf("ping: send failed\n");
            continue;
        }

        start = pit_get_ticks();
        while (!g_ping_state.received) {
            __asm__ volatile("sti");
            netif_poll_all();
            if (pit_get_ticks() - start > timeout_ticks) {
                break;
            }
            if (keyboard_has_data()) {
                key = keyboard_getchar_nb();
                if (key == 0x03) {
                    printf("ping: interrupted\n");
                    return -1;
                }
            }
            schedule();
        }

        if (g_ping_state.received) {
            printf("Reply from %u.%u.%u.%u: seq=%u time=%u ms\n",
                   target.octets[0], target.octets[1],
                   target.octets[2], target.octets[3],
                   seq, g_ping_state.rtt);
            received++;
            total_rtt += g_ping_state.rtt;
        } else {
            printf("Request timeout for seq %u\n", seq);
        }
    }

    printf("\n--- %u.%u.%u.%u ping statistics ---\n",
           target.octets[0], target.octets[1],
           target.octets[2], target.octets[3]);
    printf("%u packets transmitted, %u received, %u%% loss\n",
           count, received, (count - received) * 100 / count);

    if (received > 0) {
        printf("avg rtt = %u ms\n", total_rtt / received);
    }

    return received > 0 ? 0 : -1;
}

int ping_one(ipv4_addr_t target, uint16_t seq, uint64_t timeout_ms) {
    netif_t *netif;
    static uint16_t ping_id = 0;
    uint8_t payload[56];
    uint64_t timeout_ticks;
    uint64_t start;
    int i;
    int key;

    netif = netif_get_default();
    if (!netif) {
        return -4;
    }

    if (seq == 0) {
        ping_id = (uint16_t)(net_get_ticks() & 0xFFFF);
    }

    g_ping_state.target = target;
    g_ping_state.id = ping_id;
    g_ping_state.seq = seq;
    g_ping_state.received = 0;
    g_ping_state.rtt = 0;
    g_ping_state.send_time = net_get_ticks();

    for (i = 0; i < 56; i++) {
        payload[i] = i;
    }

    if (icmp_send_echo_request(netif, target, ping_id, seq, payload, sizeof(payload)) < 0) {
        return -3;
    }

    timeout_ticks = pit_ms_to_ticks(timeout_ms);
    start = pit_get_ticks();
    while (!g_ping_state.received) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (pit_get_ticks() - start > timeout_ticks) {
            return -1; 
        }
        if (keyboard_has_data()) {
            key = keyboard_getchar_nb();
            if (key == 0x03) {
                return -2;
            }
        }
        schedule();
    }

    return (int)g_ping_state.rtt;
}
