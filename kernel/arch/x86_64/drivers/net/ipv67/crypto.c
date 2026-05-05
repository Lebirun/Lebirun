#include "internal.h"

void ipv67_make_alias_from_key(const uint8_t *key, char alias[IPV67_ALIAS_SIZE]) {
    uint8_t hash[32];
    int i;

    memset(alias, 0, IPV67_ALIAS_SIZE);
    if (!key) return;
    sha256_hash(key, IPV67_IDENTITY_SIZE, hash);
    for (i = 0; i < 7 && (i * 2 + 1) < IPV67_ALIAS_SIZE - 1; i++) {
        alias[i * 2] = "0123456789abcdef"[(hash[i] >> 4) & 0xf];
        alias[i * 2 + 1] = "0123456789abcdef"[hash[i] & 0xf];
    }
    alias[14] = '\0';
}

void ipv67_derive_public_identity(const uint8_t *key, uint8_t *public_key) {
    if (!key || !public_key) return;
    sha256_hash(key, IPV67_IDENTITY_SIZE, public_key);
}

void ipv67_derive_session_key(ipv67_peer_t *peer) {
    uint8_t material[IPV67_IDENTITY_SIZE * 2 + sizeof(uint64_t) * 2];
    uint64_t first_challenge;
    uint64_t second_challenge;
    const uint8_t *first_key;
    const uint8_t *second_key;

    if (!peer || !peer->public_key[0]) return;
    memset(material, 0, sizeof(material));
    if (memcmp(ipv67_identity_public, peer->public_key, IPV67_IDENTITY_SIZE) <= 0) {
        first_key = ipv67_identity_public;
        second_key = peer->public_key;
    } else {
        first_key = peer->public_key;
        second_key = ipv67_identity_public;
    }
    memcpy(material, first_key, IPV67_IDENTITY_SIZE);
    memcpy(material + IPV67_IDENTITY_SIZE, second_key, IPV67_IDENTITY_SIZE);
    if (peer->local_challenge < peer->remote_challenge) {
        first_challenge = peer->local_challenge;
        second_challenge = peer->remote_challenge;
    } else {
        first_challenge = peer->remote_challenge;
        second_challenge = peer->local_challenge;
    }
    memcpy(material + IPV67_IDENTITY_SIZE * 2, &first_challenge, sizeof(first_challenge));
    memcpy(material + IPV67_IDENTITY_SIZE * 2 + sizeof(first_challenge), &second_challenge, sizeof(second_challenge));
    sha256_hash(material, sizeof(material), peer->session_key);
    peer->session_established = 1;
    peer->authenticated = 1;
}

const uint8_t *ipv67_signing_key_for_peer(const ipv67_peer_t *peer) {
    if (peer && peer->session_established) return peer->session_key;
    if (ipv67_auth_key_set) return ipv67_auth_key;
    return NULL;
}

int ipv67_is_bootstrap_type(uint8_t type) {
    if (type == IPV67_TYPE_PEER_REQ || type == IPV67_TYPE_PEER_ACK || type == IPV67_TYPE_ROUTE_ADV) return 1;
    if (type == IPV67_TYPE_AUTH_HELLO || type == IPV67_TYPE_AUTH_REPLY || type == IPV67_TYPE_AUTH_DONE) return 1;
    if (type == IPV67_TYPE_PING || type == IPV67_TYPE_PONG) return 1;
    return 0;
}

void ipv67_make_alias(const ipv67_addr_t *addr, char alias[IPV67_ALIAS_SIZE]) {
    uint8_t hash[32];
    char text[IPV67_ADDR_STR_MAX];
    int i;

    memset(alias, 0, IPV67_ALIAS_SIZE);
    if (!addr) return;
    if (ipv67_addr_format(addr, text, sizeof(text)) < 0) return;
    sha256_hash((const uint8_t *)text, strlen(text), hash);
    for (i = 0; i < 7 && (i * 2 + 1) < IPV67_ALIAS_SIZE - 1; i++) {
        alias[i * 2] = "0123456789abcdef"[(hash[i] >> 4) & 0xf];
        alias[i * 2 + 1] = "0123456789abcdef"[hash[i] & 0xf];
    }
    alias[14] = '\0';
}

void ipv67_prepare_header(ipv67_header_t *hdr) {
    uint64_t random_bits;

    if (!hdr) return;
    random_bits = rng_get_u64();
    hdr->sequence = ++ipv67_next_sequence;
    hdr->nonce = random_bits ^ pit_get_ticks() ^ net_get_ticks();
    hdr->packet_id = rng_get_u64() ^ (hdr->sequence << 1) ^ hdr->nonce;
    if (ipv67_identity_key_set) ipv67_make_alias_from_key(ipv67_identity_public, hdr->alias);
    else if (ipv67_self_set) ipv67_make_alias(&ipv67_self, hdr->alias);
}

void ipv67_sign_header_key(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, const uint8_t *key) {
    uint8_t stack_buf[sizeof(ipv67_header_t) + 512];
    uint8_t *buf;
    uint64_t total;
    int heap;

    if (!hdr || !key) return;
    total = sizeof(ipv67_header_t) + plen;
    heap = 0;
    if (total <= sizeof(stack_buf)) {
        buf = stack_buf;
    } else {
        buf = (uint8_t *)kmalloc(total);
        if (!buf) return;
        heap = 1;
    }
    memcpy(buf, hdr, sizeof(ipv67_header_t));
    memset(((ipv67_header_t *)buf)->signature, 0, IPV67_SIG_SIZE);
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);
    hmac_sha256(key, IPV67_AUTH_KEY_SIZE, buf, total, hdr->signature);
    if (heap) kfree(buf);
}

void ipv67_sign_header(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    const uint8_t *key;

    key = ipv67_signing_key_for_peer(NULL);
    ipv67_sign_header_key(hdr, payload, plen, key);
}

static int ipv67_verify_header_key(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, const uint8_t *key) {
    uint8_t stack_buf[sizeof(ipv67_header_t) + 512];
    uint8_t expected[IPV67_SIG_SIZE];
    uint8_t *buf;
    uint64_t total;
    int heap;

    if (!hdr) return 0;
    if (!key) return 1;
    total = sizeof(ipv67_header_t) + plen;
    heap = 0;
    if (total <= sizeof(stack_buf)) {
        buf = stack_buf;
    } else {
        buf = (uint8_t *)kmalloc(total);
        if (!buf) return 0;
        heap = 1;
    }
    memcpy(buf, hdr, sizeof(ipv67_header_t));
    memset(((ipv67_header_t *)buf)->signature, 0, IPV67_SIG_SIZE);
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);
    hmac_sha256(key, IPV67_AUTH_KEY_SIZE, buf, total, expected);
    if (heap) kfree(buf);
    return crypto_constant_compare(expected, hdr->signature, IPV67_SIG_SIZE) == 0;
}

int ipv67_verify_header(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, ipv67_peer_t *peer) {
    if (!hdr) return 0;
    if (ipv67_is_bootstrap_type(hdr->type) && ipv67_verify_header_key(hdr, payload, plen, ipv67_bootstrap_key)) return 1;
    if (!ipv67_auth_key_set) return 1;
    if (peer && peer->session_established && ipv67_verify_header_key(hdr, payload, plen, peer->session_key)) return 1;
    return ipv67_verify_header_key(hdr, payload, plen, ipv67_auth_key);
}

static int ipv67_ensure_replay_cap(int needed) {
    uint64_t *next;
    int next_cap;
    int i;

    if (!ipv67_current) return IPV67_ERR_INVAL;
    if (needed <= ipv67_replay_cap) return IPV67_ERR_OK;
    next_cap = ipv67_replay_cap ? ipv67_replay_cap * 2 : 4;
    while (next_cap < needed) next_cap *= 2;
    next = (uint64_t *)kmalloc(sizeof(uint64_t) * next_cap);
    if (!next) return IPV67_ERR_NOMEM;
    memset(next, 0, sizeof(uint64_t) * next_cap);
    for (i = 0; i < ipv67_replay_cap; i++) {
        next[i] = ipv67_replay_ids[i];
    }
    if (ipv67_replay_ids) kfree(ipv67_replay_ids);
    ipv67_current->replay_ids = next;
    ipv67_current->replay_cap = next_cap;
    if (ipv67_replay_pos >= ipv67_replay_cap) ipv67_replay_pos = 0;
    return IPV67_ERR_OK;
}

static int ipv67_global_replay_seen(uint64_t packet_id) {
    int needed;
    int i;

    if (packet_id == 0) return 1;
    needed = ipv67_peer_count_val * 4 + 8;
    if (needed < 8) needed = 8;
    if (ipv67_ensure_replay_cap(needed) < 0) return 1;
    for (i = 0; i < ipv67_replay_cap; i++) {
        if (ipv67_replay_ids[i] == packet_id) return 1;
    }
    ipv67_replay_ids[ipv67_replay_pos] = packet_id;
    ipv67_replay_pos++;
    if (ipv67_replay_pos >= ipv67_replay_cap) ipv67_replay_pos = 0;
    return 0;
}

static int ipv67_peer_replay_seen(ipv67_peer_t *peer, uint64_t sequence, uint64_t packet_id) {
    uint64_t shift;

    if (ipv67_global_replay_seen(packet_id)) return 1;
    if (!peer) return 0;
    if (sequence == 0) return 1;
    if (sequence > peer->highest_sequence) {
        shift = sequence - peer->highest_sequence;
        if (shift >= IPV67_REPLAY_BITS) peer->replay_window = 1;
        else peer->replay_window = (peer->replay_window << shift) | 1;
        peer->highest_sequence = sequence;
        return 0;
    }
    shift = peer->highest_sequence - sequence;
    if (shift >= IPV67_REPLAY_BITS) return 1;
    if (peer->replay_window & (1ULL << shift)) return 1;
    peer->replay_window |= 1ULL << shift;
    return 0;
}

int ipv67_packet_replay_seen(ipv67_peer_t *peer, uint8_t type, uint64_t sequence, uint64_t packet_id) {
    uint64_t shift;

    if (!ipv67_is_bootstrap_type(type)) return ipv67_peer_replay_seen(peer, sequence, packet_id);
    if (ipv67_global_replay_seen(packet_id)) return 1;
    if (!peer || sequence == 0) return 0;
    if (sequence > peer->highest_sequence) {
        shift = sequence - peer->highest_sequence;
        if (shift >= IPV67_REPLAY_BITS) peer->replay_window = 1;
        else peer->replay_window = (peer->replay_window << shift) | 1;
        peer->highest_sequence = sequence;
    }
    return 0;
}
