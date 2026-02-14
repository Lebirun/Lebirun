#include <kernel/drivers/net/icmp.h>
#include <kernel/drivers/net/ipv4.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <kernel/pit.h>
#include <kernel/keyboard.h>
#include <kernel/task.h>
#include <string.h>

static ping_state_t g_ping_state;

void icmp_receive(netif_t *netif, ipv4_addr_t src, uint8_t *data, uint32_t len) {
    if (!netif || !data || len < sizeof(icmp_header_t)) return;

    icmp_header_t *icmp = (icmp_header_t *)data;

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

int icmp_send_echo_request(netif_t *netif, ipv4_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint32_t len) {
    if (!netif) return -1;

    uint32_t icmp_len = sizeof(icmp_header_t) + len;
    uint8_t *packet = (uint8_t *)kmalloc(icmp_len);
    if (!packet) return -1;

    icmp_header_t *icmp = (icmp_header_t *)packet;
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = htons(id);
    icmp->sequence = htons(seq);

    if (data && len > 0) {
        memcpy(packet + sizeof(icmp_header_t), data, len);
    }

    icmp->checksum = ipv4_checksum(packet, icmp_len);

    int result = ipv4_send(netif, dest, IP_PROTO_ICMP, packet, icmp_len);
    kfree(packet);

    return result;
}

int icmp_send_echo_reply(netif_t *netif, ipv4_addr_t dest, uint16_t id, uint16_t seq, uint8_t *data, uint32_t len) {
    if (!netif) return -1;

    uint32_t icmp_len = sizeof(icmp_header_t) + len;
    uint8_t *packet = (uint8_t *)kmalloc(icmp_len);
    if (!packet) return -1;

    icmp_header_t *icmp = (icmp_header_t *)packet;
    icmp->type = ICMP_ECHO_REPLY;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = htons(id);
    icmp->sequence = htons(seq);

    if (data && len > 0) {
        memcpy(packet + sizeof(icmp_header_t), data, len);
    }

    icmp->checksum = ipv4_checksum(packet, icmp_len);

    int result = ipv4_send(netif, dest, IP_PROTO_ICMP, packet, icmp_len);
    kfree(packet);

    return result;
}

int ping(ipv4_addr_t target, uint32_t count, uint32_t timeout_ms) {
    netif_t *netif = netif_get_default();
    if (!netif) {
        printf("ping: no network interface\n");
        return -1;
    }

    printf("PING %u.%u.%u.%u\n",
           target.octets[0], target.octets[1],
           target.octets[2], target.octets[3]);

    uint16_t id = (uint16_t)(net_get_ticks() & 0xFFFF);
    uint32_t received = 0;
    uint32_t total_rtt = 0;

    for (uint32_t seq = 0; seq < count; seq++) {
        g_ping_state.target = target;
        g_ping_state.id = id;
        g_ping_state.seq = seq;
        g_ping_state.received = 0;
        g_ping_state.rtt = 0;
        g_ping_state.send_time = net_get_ticks();

        uint8_t payload[56];
        for (int i = 0; i < 56; i++) {
            payload[i] = i;
        }

        if (icmp_send_echo_request(netif, target, id, seq, payload, sizeof(payload)) < 0) {
            printf("ping: send failed\n");
            continue;
        }

        uint32_t timeout_ticks = pit_ms_to_ticks(timeout_ms);
        uint32_t start = pit_get_ticks();
        while (!g_ping_state.received) {
            __asm__ volatile("sti");
            netif_poll_all();
            if (pit_get_ticks() - start > timeout_ticks) {
                break;
            }
            if (keyboard_has_data()) {
                int k = keyboard_getchar_nb();
                if (k == 0x03) {
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

int ping_one(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms) {
    netif_t *netif = netif_get_default();
    if (!netif) {
        return -4;
    }

    static uint16_t ping_id = 0;
    if (seq == 0) {
        ping_id = (uint16_t)(net_get_ticks() & 0xFFFF);
    }

    g_ping_state.target = target;
    g_ping_state.id = ping_id;
    g_ping_state.seq = seq;
    g_ping_state.received = 0;
    g_ping_state.rtt = 0;
    g_ping_state.send_time = net_get_ticks();

    uint8_t payload[56];
    for (int i = 0; i < 56; i++) {
        payload[i] = i;
    }

    if (icmp_send_echo_request(netif, target, ping_id, seq, payload, sizeof(payload)) < 0) {
        return -3;
    }

    uint32_t timeout_ticks = pit_ms_to_ticks(timeout_ms);
    uint32_t start = pit_get_ticks();
    while (!g_ping_state.received) {
        __asm__ volatile("sti");
        netif_poll_all();
        if (pit_get_ticks() - start > timeout_ticks) {
            return -1; 
        }
        if (keyboard_has_data()) {
            int k = keyboard_getchar_nb();
            if (k == 0x03) {
                return -2;
            }
        }
        schedule();
    }

    return (int)g_ping_state.rtt;
}
