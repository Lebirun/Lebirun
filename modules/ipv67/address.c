#include "internal.h"

static int is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_valid_zone(const char *s) {
    int i;
    int len;
    len = 0;
    for (i = 0; s[i]; i++) {
        if (!is_hex_char(s[i])) return 0;
        len++;
    }
    return len > 0 && len <= IPV67_ZONE_MAX;
}

static int is_valid_domain(const char *s) {
    char c;
    int i;
    int len;

    len = 0;
    for (i = 0; s[i]; i++) {
        c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        len++;
    }
    return len > 0 && len <= IPV67_DOMAIN_MAX;
}

int ipv67_addr_parse(const char *str, ipv67_addr_t *out) {
    const char *p;
    const char *dot;
    int part;
    int i;
    int len;

    if (!str || !out) return IPV67_ERR_INVAL;

    memset(out, 0, sizeof(ipv67_addr_t));
    p = str;
    part = 0;

    for (part = 0; part < 5; part++) {
        dot = p;
        while (*dot && *dot != '.') dot++;
        len = (int)(dot - p);

        if (len == 0) return IPV67_ERR_INVAL;

        switch (part) {
        case 0:
            if (len > IPV67_ZONE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->zone1, p, len);
            out->zone1[len] = '\0';
            break;
        case 1:
            if (len > IPV67_ZONE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->zone2, p, len);
            out->zone2[len] = '\0';
            break;
        case 2:
            if (len > IPV67_DOMAIN_MAX) return IPV67_ERR_TOOLONG;
            memcpy(out->domain, p, len);
            out->domain[len] = '\0';
            break;
        case 3:
            if (len > IPV67_NODE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->node1, p, len);
            out->node1[len] = '\0';
            break;
        case 4:
            if (len > IPV67_NODE_MAX) return IPV67_ERR_TOOLONG;
            for (i = 0; i < len; i++) {
                if (!is_hex_char(p[i])) return IPV67_ERR_INVAL;
            }
            memcpy(out->node2, p, len);
            out->node2[len] = '\0';
            break;
        default:
            return IPV67_ERR_INVAL;
        }

        p = dot;
        if (part < 4) {
            if (*p != '.') return IPV67_ERR_INVAL;
            p++;
        }
    }

    if (*p != '\0') return IPV67_ERR_INVAL;

    if (!is_valid_zone(out->zone1)) return IPV67_ERR_INVAL;
    if (!is_valid_zone(out->zone2)) return IPV67_ERR_INVAL;
    if (!is_valid_domain(out->domain)) return IPV67_ERR_INVAL;
    if (!is_valid_zone(out->node1)) return IPV67_ERR_INVAL;
    if (!is_valid_zone(out->node2)) return IPV67_ERR_INVAL;

    return IPV67_ERR_OK;
}

int ipv67_addr_format(const ipv67_addr_t *addr, char *buf, uint64_t bufsz) {
    int needed;
    int z1, z2, dom, n1, n2;

    if (!addr || !buf || bufsz == 0) return IPV67_ERR_INVAL;

    z1  = (int)strlen(addr->zone1);
    z2  = (int)strlen(addr->zone2);
    dom = (int)strlen(addr->domain);
    n1  = (int)strlen(addr->node1);
    n2  = (int)strlen(addr->node2);
    needed = z1 + 1 + z2 + 1 + dom + 1 + n1 + 1 + n2 + 1;

    if ((uint64_t)needed > bufsz) return IPV67_ERR_TOOLONG;

    snprintf(buf, bufsz, "%s.%s.%s.%s.%s",
             addr->zone1, addr->zone2, addr->domain, addr->node1, addr->node2);
    return IPV67_ERR_OK;
}

int ipv67_addr_eq(const ipv67_addr_t *a, const ipv67_addr_t *b) {
    if (!a || !b) return 0;
    return strcmp(a->zone1, b->zone1) == 0 &&
           strcmp(a->zone2, b->zone2) == 0 &&
           strcmp(a->domain, b->domain) == 0 &&
           strcmp(a->node1, b->node1) == 0 &&
           strcmp(a->node2, b->node2) == 0;
}

void ipv67_self_addr(ipv67_addr_t *out) {
    if (!out) return;
    if (ipv67_self_set) {
        memcpy(out, &ipv67_self, sizeof(ipv67_addr_t));
    } else {
        memset(out, 0, sizeof(ipv67_addr_t));
    }
}

int ipv67_set_self_addr(const ipv67_addr_t *addr) {
    int owned;

    if (!addr) return IPV67_ERR_INVAL;
    if (ipv67_identity_key_set) {
        owned = 0;
        if (ipv67_asn_claim_allows_key_addr(ipv67_identity_public, addr)) owned = 1;
        if (!owned) return IPV67_ERR_INVAL;
    }
    memcpy(&ipv67_self, addr, sizeof(ipv67_addr_t));
    ipv67_self_set = 1;
    return IPV67_ERR_OK;
}
