#include "internal.h"

#define IPV67_ASN_OWNERSHIP_SPECIFICITY 136

static uint32_t ipv67_hex_part_value(const char *s) {
    uint32_t v;
    int i;
    char c;

    v = 0;
    if (!s) return 0;
    for (i = 0; s[i]; i++) {
        c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

static int ipv67_part_cmp(const char *a, const char *b, int hex) {
    uint32_t av;
    uint32_t bv;

    if (hex) {
        av = ipv67_hex_part_value(a);
        bv = ipv67_hex_part_value(b);
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }
    return strcmp(a, b);
}

static int ipv67_addr_cmp_order(const ipv67_addr_t *a, const ipv67_addr_t *b) {
    int ret;

    ret = ipv67_part_cmp(a->zone1, b->zone1, 1);
    if (ret) return ret;
    ret = ipv67_part_cmp(a->zone2, b->zone2, 1);
    if (ret) return ret;
    ret = ipv67_part_cmp(a->domain, b->domain, 0);
    if (ret) return ret;
    ret = ipv67_part_cmp(a->node1, b->node1, 1);
    if (ret) return ret;
    return ipv67_part_cmp(a->node2, b->node2, 1);
}

static uint8_t ipv67_asn_range_specificity(const ipv67_addr_t *start, const ipv67_addr_t *end) {
    if (!start || !end) return 0;
    if (strcmp(start->zone1, end->zone1) != 0) return 0;
    if (strcmp(start->zone2, end->zone2) != 0) return 24;
    if (strcmp(start->domain, end->domain) != 0) return 48;
    if (strcmp(start->node1, end->node1) != 0) return 112;
    if (strcmp(start->node2, end->node2) != 0) return 136;
    return 160;
}

static int ipv67_asn_claim_same_range(const ipv67_asn_claim_t *a, const ipv67_asn_claim_t *b) {
    if (!a || !b) return 0;
    return a->asn == b->asn &&
           ipv67_addr_eq(&a->start, &b->start) &&
           ipv67_addr_eq(&a->end, &b->end);
}

static int ipv67_asn_claim_overlaps(const ipv67_asn_claim_t *a, const ipv67_asn_claim_t *b) {
    if (!a || !b || !a->valid || !b->valid) return 0;
    if (ipv67_addr_cmp_order(&a->start, &b->end) > 0) return 0;
    if (ipv67_addr_cmp_order(&b->start, &a->end) > 0) return 0;
    return 1;
}

static int ipv67_asn_claim_weakness(const ipv67_asn_claim_t *claim, uint8_t incoming_specificity) {
    int score;

    if (!claim) return -1;
    score = 0;
    if (!(claim->flags & IPV67_ASN_FLAG_AUTH)) score += 4;
    if (claim->flags & IPV67_ASN_FLAG_RELAY) score += 3;
    if (claim->flags & IPV67_ASN_FLAG_BROAD) score += 2;
    if (claim->specificity < incoming_specificity) score++;
    return score;
}

static int ipv67_asn_claim_grants_ownership(const ipv67_asn_claim_t *claim) {
    if (!claim || !claim->valid) return 0;
    if (!(claim->flags & IPV67_ASN_FLAG_AUTH)) return 0;
    if (claim->flags & IPV67_ASN_FLAG_RELAY) return 0;
    if (claim->flags & IPV67_ASN_FLAG_QUARANTINE) return 0;
    if (claim->specificity < IPV67_ASN_OWNERSHIP_SPECIFICITY) return 0;
    if (!ipv67_identity_key_present(claim->source_key)) return 0;
    return 1;
}

static int ipv67_asn_claim_grants_route(const ipv67_asn_claim_t *claim) {
    if (!claim || !claim->valid) return 0;
    if (!(claim->flags & IPV67_ASN_FLAG_AUTH)) return 0;
    if (claim->flags & IPV67_ASN_FLAG_QUARANTINE) return 0;
    if (claim->specificity < IPV67_ASN_OWNERSHIP_SPECIFICITY) return 0;
    if (!ipv67_identity_key_present(claim->source_key)) return 0;
    return 1;
}

static int ipv67_find_asn_replace_slot(const ipv67_asn_claim_t *claim) {
    uint32_t now;
    uint32_t age;
    uint32_t best_age;
    int best_idx;
    int best_score;
    int claim_score;
    int score;
    int i;

    if (!claim || !ipv67_current || !ipv67_asns) return -1;
    now = net_get_ticks();
    best_idx = -1;
    best_score = -1;
    best_age = 0;
    claim_score = ipv67_asn_claim_weakness(claim, claim->specificity);
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asns[i].valid) return i;
        if (ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL) continue;
        score = ipv67_asn_claim_weakness(&ipv67_asns[i], claim->specificity);
        if (score < claim_score) continue;
        if (score == claim_score && ipv67_asns[i].specificity >= claim->specificity) continue;
        age = now - ipv67_asns[i].age_ticks;
        if (best_idx < 0 || score > best_score || (score == best_score && age > best_age)) {
            best_idx = i;
            best_score = score;
            best_age = age;
        }
    }
    return best_idx;
}

int ipv67_asn_claim_is_broad(const ipv67_asn_claim_t *claim) {
    if (!claim) return 0;
    return claim->specificity < IPV67_ASN_OWNERSHIP_SPECIFICITY;
}

static int ipv67_addr_in_asn_claim(const ipv67_addr_t *addr, const ipv67_asn_claim_t *claim) {
    if (!addr || !claim || !claim->valid) return 0;
    if (ipv67_addr_cmp_order(addr, &claim->start) < 0) return 0;
    if (ipv67_addr_cmp_order(addr, &claim->end) > 0) return 0;
    return 1;
}

int ipv67_asn_claim_allows_key_addr(const uint8_t *key, const ipv67_addr_t *addr) {
    int i;

    if (!ipv67_current || !ipv67_asns || !ipv67_identity_key_present(key) || !addr) return 0;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asn_claim_grants_ownership(&ipv67_asns[i])) continue;
        if (crypto_constant_compare(key, ipv67_asns[i].source_key, IPV67_IDENTITY_SIZE) != 0) continue;
        if (ipv67_addr_in_asn_claim(addr, &ipv67_asns[i])) return 1;
    }
    return 0;
}

int ipv67_asn_claim_routes_key_addr(const uint8_t *key, const ipv67_addr_t *addr) {
    int i;

    if (!ipv67_current || !ipv67_asns || !ipv67_identity_key_present(key) || !addr) return 0;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asn_claim_grants_route(&ipv67_asns[i])) continue;
        if (crypto_constant_compare(key, ipv67_asns[i].source_key, IPV67_IDENTITY_SIZE) != 0) continue;
        if (ipv67_addr_in_asn_claim(addr, &ipv67_asns[i])) return 1;
    }
    return 0;
}

static ipv67_peer_t *ipv67_peer_for_asn_claim(const ipv67_asn_claim_t *claim) {
    int i;

    if (!claim || !ipv67_current || !ipv67_peers) return NULL;
    if (!ipv67_identity_key_present(claim->source_key)) return NULL;
    for (i = 0; i < ipv67_current->peer_cap; i++) {
        if (!ipv67_peers[i].active) continue;
        if (!ipv67_peers[i].authenticated || !ipv67_peers[i].session_established) continue;
        if (!ipv67_identity_key_present(ipv67_peers[i].public_key)) continue;
        if (crypto_constant_compare(claim->source_key, ipv67_peers[i].public_key, IPV67_IDENTITY_SIZE) != 0) continue;
        if (!ipv67_peers[i].addr.zone1[0]) continue;
        if (!ipv67_addr_in_asn_claim(&ipv67_peers[i].addr, claim)) continue;
        return &ipv67_peers[i];
    }
    return NULL;
}

int ipv67_route_from_asn(const ipv67_addr_t *dst) {
    ipv67_asn_claim_t *best;
    ipv67_peer_t *peer;
    int i;
    int best_score;
    int score;

    if (!dst || !ipv67_current || !ipv67_asns) return 0;
    best = NULL;
    best_score = -1;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asns[i].valid) continue;
        if (ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL) continue;
        if (!ipv67_asn_claim_grants_ownership(&ipv67_asns[i])) continue;
        if (!ipv67_addr_in_asn_claim(dst, &ipv67_asns[i])) continue;
        peer = ipv67_peer_for_asn_claim(&ipv67_asns[i]);
        if (!peer) continue;
        score = ipv67_asns[i].specificity;
        if (ipv67_asns[i].flags & IPV67_ASN_FLAG_AUTH) score += 256;
        if (!(ipv67_asns[i].flags & IPV67_ASN_FLAG_BROAD)) score += 64;
        if (score > best_score) {
            best = &ipv67_asns[i];
            best_score = score;
        }
    }
    if (!best) return 0;
    peer = ipv67_peer_for_asn_claim(best);
    if (!peer) return 0;
    if (peer->family == IPV67_PEER_IPV6) ipv67_update_route_ex(dst, IPV67_PEER_IPV6, 0, &peer->ipv6, peer->port, 1, 2, 0, peer->public_key);
    else ipv67_update_route_ex(dst, IPV67_PEER_IPV4, peer->ipv4, NULL, peer->port, 1, 2, 0, peer->public_key);
    return 1;
}

void ipv67_recompute_asn_conflicts(void) {
    int i;
    int j;
    int count;

    if (!ipv67_current || !ipv67_asns) return;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asns[i].valid) continue;
        ipv67_asns[i].flags &= (uint8_t)~(IPV67_ASN_FLAG_CONFLICT | IPV67_ASN_FLAG_QUARANTINE);
        count = 0;
        for (j = 0; j < ipv67_current->asn_cap; j++) {
            if (i == j || !ipv67_asns[j].valid) continue;
            if (ipv67_asn_claim_overlaps(&ipv67_asns[i], &ipv67_asns[j])) count++;
        }
        if (count > 255) count = 255;
        ipv67_asns[i].conflict_count = (uint8_t)count;
        if (count > 0) {
            ipv67_asns[i].flags |= IPV67_ASN_FLAG_CONFLICT;
            if (!(ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL)) ipv67_asns[i].flags |= IPV67_ASN_FLAG_QUARANTINE;
        }
    }
}

int ipv67_set_asn_claim(const ipv67_asn_claim_t *claim) {
    ipv67_asn_claim_t next;
    int i;
    int free_idx;
    int local_asn;
    uint8_t max_specificity;

    if (!claim || claim->asn == 0) return IPV67_ERR_INVAL;
    local_asn = ipv67_local_asn_from_identity();
    if (local_asn < 0 || claim->asn != (uint32_t)local_asn) return IPV67_ERR_INVAL;
    if (ipv67_addr_cmp_order(&claim->start, &claim->end) > 0) return IPV67_ERR_INVAL;
    memcpy(&next, claim, sizeof(next));
    max_specificity = ipv67_asn_range_specificity(&next.start, &next.end);
    if (next.specificity == 0 || next.specificity > max_specificity) next.specificity = max_specificity;
    next.valid = 1;
    next.flags |= IPV67_ASN_FLAG_LOCAL;
    next.flags &= (uint8_t)~IPV67_ASN_FLAG_BROAD;
    if (ipv67_asn_claim_is_broad(&next)) next.flags |= IPV67_ASN_FLAG_BROAD;
    if (ipv67_identity_key_set) {
        memcpy(next.source_key, ipv67_identity_public, IPV67_IDENTITY_SIZE);
        ipv67_make_alias_from_key(ipv67_identity_public, next.source);
        next.flags |= IPV67_ASN_FLAG_AUTH;
    } else {
        memcpy(next.source, "local", 6);
    }
    next.age_ticks = net_get_ticks();
    free_idx = -1;
    if (ipv67_ensure_asn_cap(ipv67_current->asn_count + 1) < 0) return IPV67_ERR_NOMEM;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (ipv67_asns[i].valid && ipv67_asn_claim_same_range(&ipv67_asns[i], &next) && (ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL)) {
            memcpy(&ipv67_asns[i], &next, sizeof(next));
            ipv67_recompute_asn_conflicts();
            return IPV67_ERR_OK;
        }
        if (ipv67_asns[i].valid && (ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL) && ipv67_asn_claim_overlaps(&ipv67_asns[i], &next)) return IPV67_ERR_INVAL;
        if (free_idx < 0 && !ipv67_asns[i].valid) free_idx = i;
    }
    if (free_idx < 0) return IPV67_ERR_NOMEM;
    memcpy(&ipv67_asns[free_idx], &next, sizeof(next));
    ipv67_current->asn_count++;
    ipv67_recompute_asn_conflicts();
    return IPV67_ERR_OK;
}

int ipv67_get_local_asn(void) {
    return ipv67_local_asn_from_identity();
}

int ipv67_remove_asn_claim(uint32_t asn, const ipv67_addr_t *start, const ipv67_addr_t *end) {
    int i;
    int local_asn;

    if (!start || !end) return IPV67_ERR_INVAL;
    local_asn = ipv67_local_asn_from_identity();
    if (local_asn < 0 || asn != (uint32_t)local_asn) return IPV67_ERR_INVAL;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asns[i].valid) continue;
        if (!(ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL)) continue;
        if (ipv67_asns[i].asn == asn && ipv67_addr_eq(&ipv67_asns[i].start, start) && ipv67_addr_eq(&ipv67_asns[i].end, end)) {
            memset(&ipv67_asns[i], 0, sizeof(ipv67_asn_claim_t));
            if (ipv67_current->asn_count > 0) ipv67_current->asn_count--;
            ipv67_recompute_asn_conflicts();
            ipv67_release_empty_tables();
            return IPV67_ERR_OK;
        }
    }
    return IPV67_ERR_NOPEER;
}

int ipv67_asn_count_get(void) {
    int i;
    int count;

    if (!ipv67_current || !ipv67_asns) return 0;
    count = 0;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (ipv67_asns[i].valid) count++;
    }
    ipv67_current->asn_count = count;
    return count;
}

int ipv67_get_asns(ipv67_asn_claim_t *out, int max) {
    int i;
    int count;

    if (!out || max <= 0 || !ipv67_current || !ipv67_asns) return 0;
    count = 0;
    for (i = 0; i < ipv67_current->asn_cap && count < max; i++) {
        if (ipv67_asns[i].valid) {
            memcpy(&out[count], &ipv67_asns[i], sizeof(ipv67_asn_claim_t));
            count++;
        }
    }
    return count;
}

int ipv67_get_asn_at(int index, ipv67_asn_claim_t *out) {
    int i;
    int count;

    if (!out || index < 0) return IPV67_ERR_INVAL;
    if (!ipv67_current || !ipv67_asns) return IPV67_ERR_NOPEER;
    count = 0;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asns[i].valid) continue;
        if (count == index) {
            memcpy(out, &ipv67_asns[i], sizeof(ipv67_asn_claim_t));
            return IPV67_ERR_OK;
        }
        count++;
    }
    return IPV67_ERR_NOPEER;
}

void ipv67_remove_asn_claims_for_peer(const ipv67_peer_t *peer) {
    int i;
    int changed;

    if (!peer || !ipv67_current || !ipv67_asns || ipv67_current->asn_cap <= 0) return;
    if (!ipv67_identity_key_present(peer->public_key)) return;
    changed = 0;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (!ipv67_asns[i].valid) continue;
        if (ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL) continue;
        if (!ipv67_identity_key_present(ipv67_asns[i].source_key)) continue;
        if (crypto_constant_compare(peer->public_key, ipv67_asns[i].source_key, IPV67_IDENTITY_SIZE) != 0) continue;
        memset(&ipv67_asns[i], 0, sizeof(ipv67_asn_claim_t));
        if (ipv67_current->asn_count > 0) ipv67_current->asn_count--;
        changed = 1;
    }
    if (changed) {
        ipv67_recompute_asn_conflicts();
        ipv67_release_empty_tables();
    }
}

uint16_t ipv67_build_asn_adv(uint8_t *buf, uint16_t max_len) {
    ipv67_asn_adv_entry_t entry;
    ipv67_peer_t *claim_peer;
    uint16_t off;
    int i;
    int advertise;

    if (!buf || max_len < sizeof(entry)) return 0;
    off = 0;
    for (i = 0; i < ipv67_current->asn_cap; i++) {
        if (off + sizeof(entry) > max_len) break;
        if (!ipv67_asns[i].valid) continue;
        advertise = 0;
        if (ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL) advertise = 1;
        else if ((ipv67_asns[i].flags & IPV67_ASN_FLAG_AUTH) && ipv67_identity_key_present(ipv67_asns[i].source_key)) {
            claim_peer = ipv67_peer_for_asn_claim(&ipv67_asns[i]);
            if (claim_peer) advertise = 1;
        }
        if (!advertise) continue;
        memset(&entry, 0, sizeof(entry));
        entry.asn = ipv67_asns[i].asn;
        entry.specificity = ipv67_asns[i].specificity;
        entry.flags = ipv67_asns[i].flags;
        memcpy(entry.country, ipv67_asns[i].country, IPV67_ASN_COUNTRY_SIZE);
        memcpy(entry.source, ipv67_asns[i].source, IPV67_ASN_SOURCE_SIZE);
        memcpy(entry.name, ipv67_asns[i].name, IPV67_ASN_NAME_SIZE);
        memcpy(entry.label, ipv67_asns[i].label, IPV67_ASN_LABEL_SIZE);
        memcpy(entry.source_key, ipv67_asns[i].source_key, IPV67_IDENTITY_SIZE);
        ipv67_addr_format(&ipv67_asns[i].start, entry.start, sizeof(entry.start));
        ipv67_addr_format(&ipv67_asns[i].end, entry.end, sizeof(entry.end));
        memcpy(buf + off, &entry, sizeof(entry));
        off += sizeof(entry);
    }
    return off;
}

void ipv67_apply_asn_adv(const uint8_t *payload, uint16_t plen, const uint8_t *source_key, const char *source, int authenticated) {
    ipv67_asn_adv_entry_t entry;
    ipv67_asn_claim_t claim;
    uint16_t off;
    uint8_t max_specificity;
    int i;
    int free_idx;
    int key_matches;
    int replace_idx;
    int derived_asn;
    int entry_key_present;

    if (!payload || !ipv67_current) return;
    if (!authenticated) return;
    off = 0;
    while (off + sizeof(entry) <= plen) {
        memcpy(&entry, payload + off, sizeof(entry));
        entry.start[IPV67_ADDR_STR_MAX - 1] = '\0';
        entry.end[IPV67_ADDR_STR_MAX - 1] = '\0';
        memset(&claim, 0, sizeof(claim));
        claim.asn = entry.asn;
        claim.specificity = entry.specificity;
        claim.flags = entry.flags & (uint8_t)~(IPV67_ASN_FLAG_LOCAL | IPV67_ASN_FLAG_AUTH | IPV67_ASN_FLAG_RELAY);
        memcpy(claim.country, entry.country, IPV67_ASN_COUNTRY_SIZE);
        memcpy(claim.source, entry.source, IPV67_ASN_SOURCE_SIZE);
        memcpy(claim.name, entry.name, IPV67_ASN_NAME_SIZE);
        memcpy(claim.label, entry.label, IPV67_ASN_LABEL_SIZE);
        claim.country[IPV67_ASN_COUNTRY_SIZE - 1] = '\0';
        claim.source[IPV67_ASN_SOURCE_SIZE - 1] = '\0';
        claim.name[IPV67_ASN_NAME_SIZE - 1] = '\0';
        claim.label[IPV67_ASN_LABEL_SIZE - 1] = '\0';
        entry_key_present = ipv67_identity_key_present(entry.source_key);
        if (entry_key_present) memcpy(claim.source_key, entry.source_key, IPV67_IDENTITY_SIZE);
        else if (ipv67_identity_key_present(source_key)) memcpy(claim.source_key, source_key, IPV67_IDENTITY_SIZE);
        key_matches = 0;
        if (authenticated && ipv67_identity_key_present(source_key)) {
            if (entry_key_present && crypto_constant_compare(source_key, entry.source_key, IPV67_IDENTITY_SIZE) == 0) key_matches = 1;
            else if (!entry_key_present) key_matches = 1;
        }
        derived_asn = ipv67_asn_from_public_key(claim.source_key);
        if (!key_matches &&
            authenticated &&
            entry_key_present &&
            (entry.flags & IPV67_ASN_FLAG_AUTH) &&
            derived_asn > 0 &&
            claim.asn == (uint32_t)derived_asn) {
            claim.flags |= IPV67_ASN_FLAG_RELAY;
            key_matches = 1;
        }
        if (!key_matches) {
            off += sizeof(entry);
            continue;
        }
        if (key_matches) claim.flags |= IPV67_ASN_FLAG_AUTH;
        if (!entry_key_present && source && source[0]) {
            memset(claim.source, 0, IPV67_ASN_SOURCE_SIZE);
            memcpy(claim.source, source, strlen(source) < IPV67_ASN_SOURCE_SIZE - 1 ? strlen(source) : IPV67_ASN_SOURCE_SIZE - 1);
        }
        if (derived_asn > 0 &&
            claim.asn == (uint32_t)derived_asn &&
            ipv67_addr_parse(entry.start, &claim.start) == IPV67_ERR_OK &&
            ipv67_addr_parse(entry.end, &claim.end) == IPV67_ERR_OK &&
            claim.asn != 0 &&
            ipv67_addr_cmp_order(&claim.start, &claim.end) <= 0) {
            max_specificity = ipv67_asn_range_specificity(&claim.start, &claim.end);
            if (claim.specificity == 0 || claim.specificity > max_specificity) claim.specificity = max_specificity;
            claim.flags &= (uint8_t)~IPV67_ASN_FLAG_BROAD;
            if (ipv67_asn_claim_is_broad(&claim)) claim.flags |= IPV67_ASN_FLAG_BROAD;
            claim.valid = 1;
            claim.age_ticks = net_get_ticks();
            free_idx = -1;
            if (ipv67_ensure_asn_cap(ipv67_current->asn_count + 1) >= 0) {
                for (i = 0; i < ipv67_current->asn_cap; i++) {
                    if (ipv67_asns[i].valid && !(ipv67_asns[i].flags & IPV67_ASN_FLAG_LOCAL) && ipv67_asn_claim_same_range(&ipv67_asns[i], &claim)) {
                        memcpy(&ipv67_asns[i], &claim, sizeof(claim));
                        free_idx = -2;
                        break;
                    }
                    if (free_idx < 0 && !ipv67_asns[i].valid) free_idx = i;
                }
                if (free_idx >= 0) {
                    memcpy(&ipv67_asns[free_idx], &claim, sizeof(claim));
                    ipv67_current->asn_count++;
                } else if (free_idx == -1) {
                    replace_idx = ipv67_find_asn_replace_slot(&claim);
                    if (replace_idx >= 0) memcpy(&ipv67_asns[replace_idx], &claim, sizeof(claim));
                }
                IPV67_STATS_INC(asn_claims_rx);
            }
        }
        off += sizeof(entry);
    }
    ipv67_recompute_asn_conflicts();
}

int ipv67_advertise_asns_to_peer(const ipv67_peer_t *peer) {
    ipv67_header_t hdr;
    ipv67_peer_t peer_copy;
    uint8_t payload_stack[512];
    uint8_t *payload;
    uint16_t plen;
    uint64_t payload_size;
    char self_str[IPV67_ADDR_STR_MAX];
    int ret;

    if (!peer || !ipv67_self_set) return IPV67_ERR_INVAL;
    if (!peer->authenticated || !peer->session_established || !ipv67_identity_key_present(peer->public_key)) return IPV67_ERR_INVAL;
    if (ipv67_asn_count_get() <= 0) return IPV67_ERR_OK;
    memcpy(&peer_copy, peer, sizeof(peer_copy));
    payload_size = sizeof(ipv67_asn_adv_entry_t) * (uint64_t)ipv67_asn_count;
    if (payload_size > sizeof(payload_stack)) payload_size = sizeof(payload_stack);
    payload = payload_stack;
    plen = ipv67_build_asn_adv(payload, (uint16_t)payload_size);
    if (plen == 0) {
        return IPV67_ERR_OK;
    }
    ipv67_addr_format(&ipv67_self, self_str, sizeof(self_str));
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = IPV67_MAGIC;
    hdr.version = IPV67_PROTO_VERSION;
    hdr.type = IPV67_TYPE_ASN_ADV;
    hdr.payload_len = plen;
    hdr.hop_limit = IPV67_MAX_HOPS;
    memcpy(hdr.src, self_str, strlen(self_str) + 1);
    if (peer_copy.addr.zone1[0]) ipv67_addr_format(&peer_copy.addr, hdr.dst, sizeof(hdr.dst));
    else memcpy(hdr.dst, self_str, strlen(self_str) + 1);
    ipv67_prepare_header(&hdr);
    ret = ipv67_sign_header_key(&hdr, payload, plen, ipv67_signing_key_for_peer(&peer_copy));
    if (ret < 0) return ret;
    ret = ipv67_send_to_peer(&peer_copy, &hdr, payload, plen);
    if (ret >= 0) IPV67_STATS_ADD(asn_claims_tx, plen / sizeof(ipv67_asn_adv_entry_t));
    return ret;
}
