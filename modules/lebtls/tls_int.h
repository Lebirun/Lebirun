#ifndef LEBTLS_INT_H
#define LEBTLS_INT_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/drivers/net/tls.h>

#define LEBTLS_CA_PATH "/etc/ssl/certs/ca-certificates.crt"

#define LEBTLS_RT_HANDSHAKE 22
#define LEBTLS_RT_APP_DATA 23
#define LEBTLS_RT_ALERT 21

#define LEBTLS_SUITE_AES_128_GCM_SHA256 0x1301

#define LEBTLS_HS_CLIENT_HELLO 1

struct tls_conn {
    tcp_socket_t *tcp;
    char *host;
    uint8_t *transcript;
    uint64_t transcript_len;
    uint64_t transcript_cap;
    uint8_t *stash;
    uint64_t stash_len;
    uint64_t stash_cap;
    uint8_t hs_key_tx[16];
    uint8_t hs_key_rx[16];
    uint8_t hs_iv_tx[12];
    uint8_t hs_iv_rx[12];
    uint8_t app_key_tx[16];
    uint8_t app_key_rx[16];
    uint8_t app_iv_tx[12];
    uint8_t app_iv_rx[12];
    uint64_t hs_read_seq;
    uint64_t hs_write_seq;
    uint64_t app_read_seq;
    uint64_t app_write_seq;
    uint8_t eph_priv[32];
    int handshake_done;
};

int lebtls_transcript_append(tls_conn_t *conn, const uint8_t *data, uint64_t len);
void lebtls_conn_free(tls_conn_t *conn);

int lebtls_rec_send(tcp_socket_t *tcp, uint8_t type, const uint8_t *data, uint64_t len);
int lebtls_rec_recv(tcp_socket_t *tcp, uint8_t *type_out, uint8_t **data_out,
                    uint64_t *len_out, uint64_t timeout_ms);

int lebtls_hkdf_extract(const uint8_t *salt, size_t salt_len,
                        const uint8_t *ikm, size_t ikm_len, uint8_t out[32]);
int lebtls_hkdf_expand(const uint8_t secret[32], const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len);
int lebtls_hkdf_selftest(void);
int lebtls_hkdf_label(const uint8_t secret[32], const char *label,
                      const uint8_t *ctx, size_t ctx_len,
                      uint8_t *out, size_t out_len);

int lebtls_handshake(tls_conn_t *conn);
int lebtls_skip_post_handshake(const uint8_t *pt, uint64_t pt_len);

int lebtls_aead_seal(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t in_len, uint8_t *out);
int lebtls_aead_open(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t in_len, uint8_t *out);
int lebtls_aead_selftest(void);

int lebtls_x25519_pub(const uint8_t priv[32], uint8_t pub[32]);
int lebtls_x25519_shared(const uint8_t priv[32], const uint8_t peer[32],
                         uint8_t out[32]);
int lebtls_x25519_selftest(void);

int lebtls_rsa_pss_verify_msg(const uint8_t *n, size_t n_len,
                              const uint8_t *e, size_t e_len,
                              const uint8_t *msg, size_t msg_len,
                              const uint8_t *sig, size_t sig_len);
int lebtls_verify_server_certs(const uint8_t *cert_msg, size_t cert_msg_len,
                               const char *host, int *type_out,
                               int *ec_bits_out,
                               uint8_t **n_out, size_t *n_len_out,
                               uint8_t **e_out, size_t *e_len_out,
                               uint8_t qx_out[48], uint8_t qy_out[48]);

void bn_from_bin(uint32_t *r, const uint8_t *bin, size_t len, size_t limbs);
int bn_cmp(const uint32_t *a, const uint32_t *b, size_t limbs);
void bn_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, size_t n);
int bn_mod(uint32_t *r, const uint32_t *a, const uint32_t *m, size_t n,
           uint32_t *wa, uint32_t *wm);

int lebtls_ecdsa_verify(const uint8_t qx[32], const uint8_t qy[32],
                        const uint8_t *hash, size_t hash_len,
                        const uint8_t r_bin[32], const uint8_t s_bin[32]);
int lebtls_ecdsa_sig_parse(const uint8_t *sig, size_t sig_len,
                           uint8_t r_out[48], uint8_t s_out[48],
                           size_t coord);
int lebtls_p256_selftest(void);
int lebtls_ecdsa384_verify(const uint8_t qx[48], const uint8_t qy[48],
                           const uint8_t *hash, size_t hash_len,
                           const uint8_t r_bin[48], const uint8_t s_bin[48]);
int lebtls_p384_selftest(void);

uint8_t *lebtls_load_ca(uint64_t *len_out);

int lebtls_traffic_seal(uint64_t *seq_io, const uint8_t *key, const uint8_t *iv,
                        uint8_t outer_type, const uint8_t *in, uint64_t in_len,
                        uint8_t **rec_out, uint64_t *rec_len_out);
int lebtls_traffic_open(uint64_t *seq_io, const uint8_t *key, const uint8_t *iv,
                        const uint8_t *rec, uint64_t rec_len,
                        uint8_t **pt_out, uint64_t *pt_len_out,
                        uint8_t *inner_type_out);

#endif
