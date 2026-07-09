#include "internal.h"

typedef int64_t ipv67_gf[16];

static const ipv67_gf ipv67_x25519_121665 = {0xdb41, 1};
static const uint8_t ipv67_x25519_base[32] = {9};

static void ipv67_x25519_carry(ipv67_gf o) {
    int i;
    int64_t c;

    for (i = 0; i < 16; i++) {
        o[i] += 1LL << 16;
        c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1;
        else o[0] += 38 * (c - 1);
        o[i] -= c << 16;
    }
}

static void ipv67_x25519_select(ipv67_gf p, ipv67_gf q, int b) {
    int i;
    int64_t t;
    int64_t c;

    c = ~(int64_t)(b - 1);
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void ipv67_x25519_unpack(ipv67_gf o, const uint8_t n[32]) {
    int i;

    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void ipv67_x25519_pack(uint8_t o[32], ipv67_gf n) {
    ipv67_gf m;
    ipv67_gf t;
    int i;
    int j;
    int b;

    for (i = 0; i < 16; i++) t[i] = n[i];
    ipv67_x25519_carry(t);
    ipv67_x25519_carry(t);
    ipv67_x25519_carry(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        ipv67_x25519_select(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xff);
    }
}

static void ipv67_x25519_add(ipv67_gf o, const ipv67_gf a, const ipv67_gf b) {
    int i;

    for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void ipv67_x25519_sub(ipv67_gf o, const ipv67_gf a, const ipv67_gf b) {
    int i;

    for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void ipv67_x25519_mul(ipv67_gf o, const ipv67_gf a, const ipv67_gf b) {
    int64_t t[31];
    int i;
    int j;

    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    ipv67_x25519_carry(o);
    ipv67_x25519_carry(o);
}

static void ipv67_x25519_square(ipv67_gf o, const ipv67_gf a) {
    ipv67_x25519_mul(o, a, a);
}

static void ipv67_x25519_invert(ipv67_gf o, const ipv67_gf i) {
    ipv67_gf c;
    int a;

    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        ipv67_x25519_square(c, c);
        if (a != 2 && a != 4) ipv67_x25519_mul(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void ipv67_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    ipv67_gf x;
    ipv67_gf a;
    ipv67_gf b;
    ipv67_gf c;
    ipv67_gf d;
    ipv67_gf e;
    ipv67_gf f;
    int i;
    int r;

    for (i = 0; i < 32; i++) z[i] = scalar[i];
    z[0] &= 248;
    z[31] &= 127;
    z[31] |= 64;
    ipv67_x25519_unpack(x, point);
    for (i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = 0;
        a[i] = 0;
        c[i] = 0;
    }
    a[0] = 1;
    d[0] = 1;
    for (i = 254; i >= 0; i--) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        ipv67_x25519_select(a, b, r);
        ipv67_x25519_select(c, d, r);
        ipv67_x25519_add(e, a, c);
        ipv67_x25519_sub(a, a, c);
        ipv67_x25519_add(c, b, d);
        ipv67_x25519_sub(b, b, d);
        ipv67_x25519_square(d, e);
        ipv67_x25519_square(f, a);
        ipv67_x25519_mul(a, c, a);
        ipv67_x25519_mul(c, b, e);
        ipv67_x25519_add(e, a, c);
        ipv67_x25519_sub(a, a, c);
        ipv67_x25519_square(b, a);
        ipv67_x25519_sub(c, d, f);
        ipv67_x25519_mul(a, c, ipv67_x25519_121665);
        ipv67_x25519_add(a, a, d);
        ipv67_x25519_mul(c, c, a);
        ipv67_x25519_mul(a, d, f);
        ipv67_x25519_mul(d, b, x);
        ipv67_x25519_square(b, e);
        ipv67_x25519_select(a, b, r);
        ipv67_x25519_select(c, d, r);
    }
    ipv67_x25519_invert(c, c);
    ipv67_x25519_mul(a, a, c);
    ipv67_x25519_pack(out, a);
}

int ipv67_identity_selftest(void) {
    uint8_t a_priv[32];
    uint8_t b_priv[32];
    uint8_t a_pub[32];
    uint8_t b_pub[32];
    uint8_t a_shared[32];
    uint8_t b_shared[32];
    int i;

    for (i = 0; i < 32; i++) {
        a_priv[i] = (uint8_t)(i + 1);
        b_priv[i] = (uint8_t)(0xf0 - i);
    }
    ipv67_x25519(a_pub, a_priv, ipv67_x25519_base);
    ipv67_x25519(b_pub, b_priv, ipv67_x25519_base);
    if (!ipv67_identity_key_present(a_pub)) return 0;
    if (!ipv67_identity_key_present(b_pub)) return 0;
    ipv67_x25519(a_shared, a_priv, b_pub);
    ipv67_x25519(b_shared, b_priv, a_pub);
    if (!ipv67_identity_key_present(a_shared)) return 0;
    return crypto_constant_compare(a_shared, b_shared, 32) == 0;
}

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
    ipv67_x25519(public_key, key, ipv67_x25519_base);
}

void ipv67_derive_identity_secret(const uint8_t *public_key, uint8_t out[IPV67_SESSION_KEY_SIZE]) {
    uint8_t shared[IPV67_SESSION_KEY_SIZE];
    uint8_t material[IPV67_SESSION_KEY_SIZE + IPV67_IDENTITY_SIZE * 2];
    const uint8_t *first_key;
    const uint8_t *second_key;

    if (!out) return;
    memset(out, 0, IPV67_SESSION_KEY_SIZE);
    if (!public_key || !ipv67_identity_key_set || !ipv67_identity_key_present(public_key)) return;
    ipv67_x25519(shared, ipv67_identity_key, public_key);
    if (!ipv67_identity_key_present(shared)) return;
    if (memcmp(ipv67_identity_public, public_key, IPV67_IDENTITY_SIZE) <= 0) {
        first_key = ipv67_identity_public;
        second_key = public_key;
    } else {
        first_key = public_key;
        second_key = ipv67_identity_public;
    }
    memcpy(material, shared, IPV67_SESSION_KEY_SIZE);
    memcpy(material + IPV67_SESSION_KEY_SIZE, first_key, IPV67_IDENTITY_SIZE);
    memcpy(material + IPV67_SESSION_KEY_SIZE + IPV67_IDENTITY_SIZE, second_key, IPV67_IDENTITY_SIZE);
    sha256_hash(material, sizeof(material), out);
}

int ipv67_identity_key_present(const uint8_t *key) {
    int i;

    if (!key) return 0;
    for (i = 0; i < IPV67_IDENTITY_SIZE; i++) {
        if (key[i] != 0) return 1;
    }
    return 0;
}

int ipv67_asn_from_public_key(const uint8_t *key) {
    uint8_t hash[32];
    uint32_t value;

    if (!ipv67_identity_key_present(key)) return IPV67_ERR_INVAL;
    sha256_hash(key, IPV67_IDENTITY_SIZE, hash);
    value = ((uint32_t)hash[0] << 24) |
            ((uint32_t)hash[1] << 16) |
            ((uint32_t)hash[2] << 8) |
            (uint32_t)hash[3];
    return (int)(100000u + (value % 900000u));
}

int ipv67_local_asn_from_identity(void) {
    if (!ipv67_identity_key_set) return IPV67_ERR_INVAL;
    return ipv67_asn_from_public_key(ipv67_identity_public);
}

void ipv67_derive_session_key(ipv67_peer_t *peer) {
    uint8_t material[IPV67_IDENTITY_SIZE * 2 + sizeof(uint64_t) * 2 + IPV67_SESSION_KEY_SIZE];
    uint8_t identity_secret[IPV67_SESSION_KEY_SIZE];
    uint64_t first_challenge;
    uint64_t second_challenge;
    const uint8_t *first_key;
    const uint8_t *second_key;
    uint8_t session_secret[IPV67_SESSION_KEY_SIZE];

    if (!peer || !ipv67_identity_key_present(peer->public_key)) return;
    if (!ipv67_identity_key_set) return;
    if (peer->local_challenge == 0 || peer->remote_challenge == 0) return;
    ipv67_derive_identity_secret(peer->public_key, identity_secret);
    if (!ipv67_identity_key_present(identity_secret)) return;
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
    memcpy(material + IPV67_IDENTITY_SIZE * 2 + sizeof(first_challenge) + sizeof(second_challenge), identity_secret, IPV67_SESSION_KEY_SIZE);
    if (ipv67_auth_key_set) hmac_sha256(ipv67_auth_key, IPV67_AUTH_KEY_SIZE, identity_secret, IPV67_SESSION_KEY_SIZE, session_secret);
    else sha256_hash(identity_secret, IPV67_SESSION_KEY_SIZE, session_secret);
    hmac_sha256(session_secret, IPV67_SESSION_KEY_SIZE, material, sizeof(material), peer->session_key);
    peer->session_established = 1;
    peer->authenticated = 1;
}

const uint8_t *ipv67_signing_key_for_peer(const ipv67_peer_t *peer) {
    if (peer && peer->session_established) return peer->session_key;
    if (ipv67_auth_key_set) return ipv67_auth_key;
    return ipv67_bootstrap_key;
}

int ipv67_is_bootstrap_type(uint8_t type) {
    if (type == IPV67_TYPE_HELLO) return 1;
    if (type == IPV67_TYPE_PEER_REQ || type == IPV67_TYPE_PEER_ACK) return 1;
    if (type == IPV67_TYPE_AUTH_HELLO || type == IPV67_TYPE_AUTH_REPLY || type == IPV67_TYPE_AUTH_DONE) return 1;
    return 0;
}

int ipv67_type_known(uint8_t type) {
    if (type == IPV67_TYPE_DATA) return 1;
    if (type == IPV67_TYPE_HELLO) return 1;
    if (type == IPV67_TYPE_ROUTE) return 1;
    if (type == IPV67_TYPE_PING) return 1;
    if (type == IPV67_TYPE_PONG) return 1;
    if (type == IPV67_TYPE_PEER_REQ) return 1;
    if (type == IPV67_TYPE_PEER_ACK) return 1;
    if (type == IPV67_TYPE_ROUTE_ADV) return 1;
    if (type == IPV67_TYPE_AUTH_HELLO) return 1;
    if (type == IPV67_TYPE_AUTH_REPLY) return 1;
    if (type == IPV67_TYPE_AUTH_DONE) return 1;
    if (type == IPV67_TYPE_ASN_ADV) return 1;
    if (type == IPV67_TYPE_PUNCH) return 1;
    return 0;
}

int ipv67_type_allows_unauthenticated(uint8_t type) {
    if (type == IPV67_TYPE_HELLO) return 1;
    if (type == IPV67_TYPE_PEER_REQ || type == IPV67_TYPE_PEER_ACK) return 1;
    if (type == IPV67_TYPE_AUTH_HELLO || type == IPV67_TYPE_AUTH_REPLY || type == IPV67_TYPE_AUTH_DONE) return 1;
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

int ipv67_sign_header_key(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, const uint8_t *key) {
    uint8_t buf[sizeof(ipv67_header_t) + 512];
    uint64_t total;

    if (!hdr || !key) return IPV67_ERR_INVAL;
    total = sizeof(ipv67_header_t) + plen;
    if (total > sizeof(buf)) return IPV67_ERR_TOOLONG;
    memcpy(buf, hdr, sizeof(ipv67_header_t));
    memset(((ipv67_header_t *)buf)->signature, 0, IPV67_SIG_SIZE);
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);
    hmac_sha256(key, IPV67_AUTH_KEY_SIZE, buf, total, hdr->signature);
    return IPV67_ERR_OK;
}

int ipv67_sign_header(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen) {
    const uint8_t *key;

    key = ipv67_signing_key_for_peer(NULL);
    return ipv67_sign_header_key(hdr, payload, plen, key);
}

static int ipv67_verify_header_key(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, const uint8_t *key) {
    uint8_t buf[sizeof(ipv67_header_t) + 512];
    uint8_t expected[IPV67_SIG_SIZE];
    uint64_t total;

    if (!hdr) return 0;
    if (!key) return 0;
    total = sizeof(ipv67_header_t) + plen;
    if (total > sizeof(buf)) return 0;
    memcpy(buf, hdr, sizeof(ipv67_header_t));
    memset(((ipv67_header_t *)buf)->signature, 0, IPV67_SIG_SIZE);
    if (plen > 0 && payload) memcpy(buf + sizeof(ipv67_header_t), payload, plen);
    hmac_sha256(key, IPV67_AUTH_KEY_SIZE, buf, total, expected);
    return crypto_constant_compare(expected, hdr->signature, IPV67_SIG_SIZE) == 0;
}

int ipv67_verify_header(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, ipv67_peer_t *peer) {
    if (!hdr) return 0;
    if (ipv67_is_bootstrap_type(hdr->type) && ipv67_verify_header_key(hdr, payload, plen, ipv67_bootstrap_key)) return 1;
    if (peer && peer->session_established && (!ipv67_auth_required || peer->authenticated) && ipv67_verify_header_key(hdr, payload, plen, peer->session_key)) return 1;
    if (!ipv67_auth_key_set) return 0;
    return ipv67_verify_header_key(hdr, payload, plen, ipv67_auth_key);
}

int ipv67_header_has_peer_auth(ipv67_header_t *hdr, const uint8_t *payload, uint16_t plen, ipv67_peer_t *peer) {
    if (!hdr || !peer) return 0;
    if (peer->session_established && peer->authenticated && ipv67_verify_header_key(hdr, payload, plen, peer->session_key)) return 1;
    return 0;
}

static int ipv67_global_replay_seen(uint64_t packet_id) {
    int i;

    if (packet_id == 0) return 1;
    for (i = 0; i < IPV67_REPLAY_GLOBAL_SLOTS; i++) {
        if (ipv67_replay_ids[i] == packet_id) return 1;
    }
    ipv67_replay_ids[ipv67_replay_pos] = packet_id;
    ipv67_replay_pos++;
    if (ipv67_replay_pos >= IPV67_REPLAY_GLOBAL_SLOTS) ipv67_replay_pos = 0;
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
    } else if (sequence > 0 && peer->highest_sequence - sequence < IPV67_REPLAY_BITS) {
        shift = peer->highest_sequence - sequence;
        peer->replay_window |= 1ULL << shift;
    }
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
