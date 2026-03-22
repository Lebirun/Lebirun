#include <kernel/drivers/net/dhcp.h>
#include <kernel/drivers/net/udp.h>
#include <kernel/drivers/net/dns.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

static dhcp_state_t g_dhcp_state;

static uint32_t dhcp_rand_xid(void) {
    static uint64_t seed = 12345;
    seed = seed * 1103515245 + 12345;
    return (uint32_t)seed;
}

static void dhcp_add_option(uint8_t *options, uint64_t *offset, uint8_t code, uint8_t len, void *data) {
    options[(*offset)++] = code;
    if (len > 0) {
        options[(*offset)++] = len;
        memcpy(&options[*offset], data, len);
        *offset += len;
    }
}

static uint8_t dhcp_parse_options(uint8_t *options, uint64_t len, dhcp_state_t *state) {
    uint64_t i;
    uint8_t code;
    uint8_t opt_len;
    uint8_t msg_type;
    uint32_t lt;

    msg_type = 0;
    i = 0;
    while (i < len) {
        code = options[i++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (i >= len) break;
        opt_len = options[i++];
        if (i + opt_len > len) break;

        switch (code) {
            case DHCP_OPT_MSG_TYPE:
                if (opt_len >= 1) {
                    msg_type = options[i];
                }
                break;
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
                    memcpy(&lt, &options[i], 4);
                    state->lease_time = ntohl(lt);
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
    return msg_type;
}

static int dhcp_send_discover(netif_t *netif) {
    dhcp_packet_t pkt;
    uint64_t opt_off;
    uint8_t msg_type;
    uint8_t param_list[3];

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

    opt_off = 0;
    msg_type = DHCP_MSG_DISCOVER;
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_MSG_TYPE, 1, &msg_type);

    param_list[0] = DHCP_OPT_SUBNET;
    param_list[1] = DHCP_OPT_ROUTER;
    param_list[2] = DHCP_OPT_DNS;
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_PARAM_LIST, sizeof(param_list), param_list);

    pkt.options[opt_off++] = DHCP_OPT_END;

    g_dhcp_state.last_send_time = net_get_ticks();

    printf("DHCP: Sending DISCOVER (xid=0x%08lX)\n", g_dhcp_state.xid);

    return udp_send_from(netif, IPV4_ZERO, IPV4_BROADCAST,
                         DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                         (uint8_t *)&pkt, sizeof(pkt));
}

static int dhcp_send_request(netif_t *netif) {
    dhcp_packet_t pkt;
    uint64_t opt_off;
    uint8_t msg_type;
    uint8_t param_list[3];

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

    opt_off = 0;
    msg_type = DHCP_MSG_REQUEST;
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_MSG_TYPE, 1, &msg_type);
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_REQUESTED_IP, 4, &g_dhcp_state.offered_ip);
    if (!ipv4_eq(g_dhcp_state.server_ip, IPV4_ZERO)) {
        dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_SERVER_ID, 4, &g_dhcp_state.server_ip);
    }

    param_list[0] = DHCP_OPT_SUBNET;
    param_list[1] = DHCP_OPT_ROUTER;
    param_list[2] = DHCP_OPT_DNS;
    dhcp_add_option(pkt.options, &opt_off, DHCP_OPT_PARAM_LIST, sizeof(param_list), param_list);

    pkt.options[opt_off++] = DHCP_OPT_END;

    g_dhcp_state.last_send_time = net_get_ticks();

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
    g_dhcp_state.retries = 0;

    dhcp_send_discover(netif);
}

void dhcp_stop(netif_t *netif) {
    (void)netif;
    g_dhcp_state.state = DHCP_STATE_INIT;
}

static void dhcp_apply_config(netif_t *netif, dhcp_state_t *temp) {
    g_dhcp_state.offered_ip = temp->offered_ip;
    if (!ipv4_eq(temp->server_ip, IPV4_ZERO)) {
        g_dhcp_state.server_ip = temp->server_ip;
    }
    if (!ipv4_eq(temp->subnet_mask, IPV4_ZERO)) {
        g_dhcp_state.subnet_mask = temp->subnet_mask;
    }
    if (!ipv4_eq(temp->gateway, IPV4_ZERO)) {
        g_dhcp_state.gateway = temp->gateway;
    }
    if (!ipv4_eq(temp->dns1, IPV4_ZERO)) {
        g_dhcp_state.dns1 = temp->dns1;
    }
    if (!ipv4_eq(temp->dns2, IPV4_ZERO)) {
        g_dhcp_state.dns2 = temp->dns2;
    }
    if (temp->lease_time != 0) {
        g_dhcp_state.lease_time = temp->lease_time;
    }

    g_dhcp_state.state = DHCP_STATE_BOUND;
    g_dhcp_state.lease_start = net_get_ticks();
    g_dhcp_state.retries = 0;

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
}

void dhcp_receive(netif_t *netif, uint8_t *data, uint64_t len) {
    dhcp_packet_t *pkt;
    dhcp_state_t temp;
    uint64_t hdr_size;
    uint64_t options_len;
    uint8_t msg_type;

    hdr_size = sizeof(dhcp_packet_t) - 308;
    if (!netif || !data || len < hdr_size) return;

    pkt = (dhcp_packet_t *)data;

    if (pkt->op != DHCP_OP_REPLY) return;
    if (ntohl(pkt->xid) != g_dhcp_state.xid) return;
    if (ntohl(pkt->magic) != DHCP_MAGIC) return;

    memset(&temp, 0, sizeof(temp));
    temp.offered_ip = pkt->yiaddr;

    options_len = len - hdr_size;
    if (options_len > 308) {
        options_len = 308;
    }
    msg_type = dhcp_parse_options(pkt->options, options_len, &temp);

    if (ipv4_eq(temp.server_ip, IPV4_ZERO) && !ipv4_eq(pkt->siaddr, IPV4_ZERO)) {
        temp.server_ip = pkt->siaddr;
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
                g_dhcp_state.retries = 0;
                dhcp_send_request(netif);
            }
            break;

        case DHCP_STATE_REQUESTING:
            if (msg_type == DHCP_MSG_ACK) {
                printf("DHCP: Received ACK\n");
                dhcp_apply_config(netif, &temp);
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

int dhcp_is_negotiating(void) {
    return g_dhcp_state.state == DHCP_STATE_SELECTING ||
           g_dhcp_state.state == DHCP_STATE_REQUESTING;
}

void dhcp_tick(void) {
    uint64_t elapsed;
    uint64_t now;
    uint64_t retry_interval;

    now = net_get_ticks();

    if (g_dhcp_state.state == DHCP_STATE_SELECTING) {
        elapsed = now - g_dhcp_state.last_send_time;
        retry_interval = DHCP_RETRY_INTERVAL * (1u + g_dhcp_state.retries);
        if (retry_interval > DHCP_RETRY_INTERVAL * 4) {
            retry_interval = DHCP_RETRY_INTERVAL * 4;
        }
        if (elapsed >= retry_interval) {
            if (g_dhcp_state.retries >= DHCP_MAX_RETRIES) {
                printf("DHCP: DISCOVER timed out after %u retries, restarting\n",
                       (unsigned)g_dhcp_state.retries);
                g_dhcp_state.retries = 0;
                g_dhcp_state.xid = dhcp_rand_xid();
                dhcp_send_discover(g_dhcp_state.netif);
            } else {
                g_dhcp_state.retries++;
                dhcp_send_discover(g_dhcp_state.netif);
            }
        }
        return;
    }

    if (g_dhcp_state.state == DHCP_STATE_REQUESTING) {
        elapsed = now - g_dhcp_state.last_send_time;
        retry_interval = DHCP_RETRY_INTERVAL * (1u + g_dhcp_state.retries);
        if (retry_interval > DHCP_RETRY_INTERVAL * 4) {
            retry_interval = DHCP_RETRY_INTERVAL * 4;
        }
        if (elapsed >= retry_interval) {
            if (g_dhcp_state.retries >= DHCP_MAX_RETRIES) {
                printf("DHCP: REQUEST timed out after %u retries, restarting\n",
                       (unsigned)g_dhcp_state.retries);
                g_dhcp_state.state = DHCP_STATE_SELECTING;
                g_dhcp_state.retries = 0;
                g_dhcp_state.xid = dhcp_rand_xid();
                dhcp_send_discover(g_dhcp_state.netif);
            } else {
                g_dhcp_state.retries++;
                dhcp_send_request(g_dhcp_state.netif);
            }
        }
        return;
    }

    if (g_dhcp_state.state == DHCP_STATE_BOUND && g_dhcp_state.lease_time > 0) {
        elapsed = now - g_dhcp_state.lease_start;
        if (elapsed > g_dhcp_state.lease_time * 500) {
            printf("DHCP: Lease renewing\n");
            g_dhcp_state.state = DHCP_STATE_SELECTING;
            g_dhcp_state.retries = 0;
            g_dhcp_state.xid = dhcp_rand_xid();
            dhcp_send_discover(g_dhcp_state.netif);
        }
    }
}

int dhcp_is_bound(netif_t *netif) {
    (void)netif;
    return g_dhcp_state.state == DHCP_STATE_BOUND;
}
