#include <kernel/drivers/net/dhcp.h>
#include <kernel/drivers/net/udp.h>
#include <kernel/drivers/net/dns.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

static dhcp_state_t g_dhcp_state;

static uint32_t dhcp_rand_xid(void) {
    static uint32_t seed = 12345;
    seed = seed * 1103515245 + 12345;
    return seed;
}

static void dhcp_add_option(uint8_t *options, uint32_t *offset, uint8_t code, uint8_t len, void *data) {
    options[(*offset)++] = code;
    if (len > 0) {
        options[(*offset)++] = len;
        memcpy(&options[*offset], data, len);
        *offset += len;
    }
}

static int dhcp_parse_options(uint8_t *options, uint32_t len, dhcp_state_t *state) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t code = options[i++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (i >= len) break;
        uint8_t opt_len = options[i++];
        if (i + opt_len > len) break;

        switch (code) {
            case DHCP_OPT_SUBNET:
                if (opt_len >= 4) {
                    memcpy(&state->subnet_mask, &options[i], 4);
                }
                break;
            case DHCP_OPT_ROUTER:
                if (opt_len >= 4) {
                    memcpy(&state->gateway, &options[i], 4);
                }
                break;
            case DHCP_OPT_DNS:
                if (opt_len >= 4) {
                    memcpy(&state->dns1, &options[i], 4);
                }
                if (opt_len >= 8) {
                    memcpy(&state->dns2, &options[i + 4], 4);
                }
                break;
            case DHCP_OPT_LEASE_TIME:
                if (opt_len >= 4) {
                    state->lease_time = ntohl(*(uint32_t *)&options[i]);
                }
                break;
            case DHCP_OPT_SERVER_ID:
                if (opt_len >= 4) {
                    memcpy(&state->server_ip, &options[i], 4);
                }
                break;
            default:
                break;
        }
        i += opt_len;
    }
    return 0;
}

static int dhcp_send_discover(netif_t *netif) {
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = DHCP_OP_REQUEST;
    pkt.htype = 1;
    pkt.hlen = 6;
    pkt.hops = 0;
    pkt.xid = htonl(g_dhcp_state.xid);
    pkt.secs = 0;
    pkt.flags = htons(0x8000);
    memcpy(pkt.chaddr, &netif->mac, 6);
    pkt.magic = htonl(DHCP_MAGIC);

    uint32_t opt_off = 0;
    uint8_t msg_type = DHCP_MSG_DISCOVER;
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_MSG_TYPE, 1, &msg_type);

    uint8_t param_list[] = {DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS};
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_PARAM_LIST, sizeof(param_list), param_list);

    pkt.options[opt_off++] = DHCP_OPT_END;

    printf("DHCP: Sending DISCOVER (xid=0x%08X)\n", g_dhcp_state.xid);

    return udp_send_from(netif, IPV4_ZERO, IPV4_BROADCAST,
                         DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                         (uint8_t *)&pkt, sizeof(pkt));
}

static int dhcp_send_request(netif_t *netif) {
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = DHCP_OP_REQUEST;
    pkt.htype = 1;
    pkt.hlen = 6;
    pkt.hops = 0;
    pkt.xid = htonl(g_dhcp_state.xid);
    pkt.secs = 0;
    pkt.flags = htons(0x8000);
    memcpy(pkt.chaddr, &netif->mac, 6);
    pkt.magic = htonl(DHCP_MAGIC);

    uint32_t opt_off = 0;
    uint8_t msg_type = DHCP_MSG_REQUEST;
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_MSG_TYPE, 1, &msg_type);
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_REQUESTED_IP, 4, &g_dhcp_state.offered_ip);
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_SERVER_ID, 4, &g_dhcp_state.server_ip);

    uint8_t param_list[] = {DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS};
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_PARAM_LIST, sizeof(param_list), param_list);

    pkt.options[opt_off++] = DHCP_OPT_END;

    printf("DHCP: Sending REQUEST for %u.%u.%u.%u\n",
           g_dhcp_state.offered_ip.octets[0], g_dhcp_state.offered_ip.octets[1],
           g_dhcp_state.offered_ip.octets[2], g_dhcp_state.offered_ip.octets[3]);

    return udp_send_from(netif, IPV4_ZERO, IPV4_BROADCAST,
                         DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                         (uint8_t *)&pkt, sizeof(pkt));
}

void dhcp_init(netif_t *netif) {
    memset(&g_dhcp_state, 0, sizeof(g_dhcp_state));
    g_dhcp_state.state = DHCP_STATE_INIT;
    g_dhcp_state.netif = netif;
}

void dhcp_start(netif_t *netif) {
    if (!netif) return;

    g_dhcp_state.xid = dhcp_rand_xid();
    g_dhcp_state.state = DHCP_STATE_SELECTING;
    g_dhcp_state.netif = netif;

    dhcp_send_discover(netif);
}

void dhcp_stop(netif_t *netif) {
    (void)netif;
    g_dhcp_state.state = DHCP_STATE_INIT;
}

void dhcp_receive(netif_t *netif, uint8_t *data, uint32_t len) {
    if (!netif || !data || len < sizeof(dhcp_packet_t) - 308) return;

    dhcp_packet_t *pkt = (dhcp_packet_t *)data;

    if (pkt->op != DHCP_OP_REPLY) return;
    if (ntohl(pkt->xid) != g_dhcp_state.xid) return;
    if (ntohl(pkt->magic) != DHCP_MAGIC) return;

    dhcp_state_t temp;
    memset(&temp, 0, sizeof(temp));
    temp.offered_ip = pkt->yiaddr;

    uint32_t options_len = len - (sizeof(dhcp_packet_t) - 308);
    dhcp_parse_options(pkt->options, options_len > 308 ? 308 : options_len, &temp);

    uint8_t msg_type = 0;
    uint32_t i = 0;
    while (i < options_len && i < 308) {
        if (pkt->options[i] == DHCP_OPT_PAD) { i++; continue; }
        if (pkt->options[i] == DHCP_OPT_END) break;
        if (pkt->options[i] == DHCP_OPT_MSG_TYPE && i + 2 < options_len) {
            msg_type = pkt->options[i + 2];
            break;
        }
        if (i + 1 >= options_len) break;
        i += 2 + pkt->options[i + 1];
    }

    switch (g_dhcp_state.state) {
        case DHCP_STATE_SELECTING:
            if (msg_type == DHCP_MSG_OFFER) {
                printf("DHCP: Received OFFER: %u.%u.%u.%u\n",
                       temp.offered_ip.octets[0], temp.offered_ip.octets[1],
                       temp.offered_ip.octets[2], temp.offered_ip.octets[3]);

                g_dhcp_state.offered_ip = temp.offered_ip;
                g_dhcp_state.server_ip = temp.server_ip;
                g_dhcp_state.subnet_mask = temp.subnet_mask;
                g_dhcp_state.gateway = temp.gateway;
                g_dhcp_state.dns1 = temp.dns1;
                g_dhcp_state.dns2 = temp.dns2;
                g_dhcp_state.lease_time = temp.lease_time;

                g_dhcp_state.state = DHCP_STATE_REQUESTING;
                dhcp_send_request(netif);
            }
            break;

        case DHCP_STATE_REQUESTING:
            if (msg_type == DHCP_MSG_ACK) {
                printf("DHCP: Received ACK\n");

                g_dhcp_state.state = DHCP_STATE_BOUND;
                g_dhcp_state.lease_start = net_get_ticks();

                netif_set_ipv4(netif, g_dhcp_state.offered_ip,
                              g_dhcp_state.subnet_mask,
                              g_dhcp_state.gateway);
                netif_set_dns(netif, g_dhcp_state.dns1, g_dhcp_state.dns2);
                netif->dhcp_configured = 1;

                dns_set_server(g_dhcp_state.dns1);
                if (!ipv4_eq(g_dhcp_state.dns2, IPV4_ZERO)) {
                    dns_set_server2(g_dhcp_state.dns2);
                }

                printf("DHCP: Configured:\n");
                printf("  IP: %u.%u.%u.%u\n",
                       netif->ipv4.octets[0], netif->ipv4.octets[1],
                       netif->ipv4.octets[2], netif->ipv4.octets[3]);
                printf("  Netmask: %u.%u.%u.%u\n",
                       netif->netmask.octets[0], netif->netmask.octets[1],
                       netif->netmask.octets[2], netif->netmask.octets[3]);
                printf("  Gateway: %u.%u.%u.%u\n",
                       netif->gateway.octets[0], netif->gateway.octets[1],
                       netif->gateway.octets[2], netif->gateway.octets[3]);
                printf("  DNS: %u.%u.%u.%u\n",
                       netif->dns_server.octets[0], netif->dns_server.octets[1],
                       netif->dns_server.octets[2], netif->dns_server.octets[3]);
            } else if (msg_type == DHCP_MSG_NAK) {
                printf("DHCP: Received NAK, restarting\n");
                g_dhcp_state.state = DHCP_STATE_INIT;
                dhcp_start(netif);
            }
            break;

        default:
            break;
    }
}

void dhcp_tick(void) {
    if (g_dhcp_state.state == DHCP_STATE_BOUND && g_dhcp_state.lease_time > 0) {
        uint32_t elapsed = net_get_ticks() - g_dhcp_state.lease_start;
        if (elapsed > g_dhcp_state.lease_time * 500) {
            printf("DHCP: Lease renewing\n");
            g_dhcp_state.state = DHCP_STATE_SELECTING;
            dhcp_send_discover(g_dhcp_state.netif);
        }
    }
}

int dhcp_is_bound(netif_t *netif) {
    (void)netif;
    return g_dhcp_state.state == DHCP_STATE_BOUND;
}
