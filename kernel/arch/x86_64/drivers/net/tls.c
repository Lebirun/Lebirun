#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <lebirun/drivers/net/tls.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <lebirun/pit.h>
#include <lebirun/rtc.h>
#include <lebirun/task.h>
#include <lebirun/tty.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

struct tls_conn {
    WOLFSSL *ssl;
    WOLFSSL_CTX *ctx;
    tcp_socket_t *tcp;
};

static int tls_active;

static int wolf_recv_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx);
static int wolf_send_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx);

static WOLFSSL_CTX *tls_create_ctx(void)
{
    WOLFSSL_CTX *ctx;
    vfs_node_t *ca_node;
    uint8_t *ca_buf;
    uint32_t rd;
    int ret;

    ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (!ctx) {
        ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    }
    if (!ctx) {
        printf("TLS: Failed to create WolfSSL context\n");
        return NULL;
    }

    wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    wolfSSL_SetIORecv(ctx, wolf_recv_cb);
    wolfSSL_SetIOSend(ctx, wolf_send_cb);

    ca_node = vfs_namei("/etc/ssl/certs/ca-certificates.crt");
    if (!ca_node || ca_node->length == 0) {
        printf("TLS: CA bundle missing\n");
        wolfSSL_CTX_free(ctx);
        return NULL;
    }

    ca_buf = (uint8_t *)kmalloc(ca_node->length);
    if (!ca_buf) {
        wolfSSL_CTX_free(ctx);
        return NULL;
    }

    rd = vfs_read(ca_node, 0, ca_node->length, ca_buf);
    if (rd == 0) {
        printf("TLS: Failed to read CA bundle\n");
        kfree(ca_buf);
        wolfSSL_CTX_free(ctx);
        return NULL;
    }

    ret = wolfSSL_CTX_load_verify_buffer(ctx, ca_buf, (long)rd,
                                         SSL_FILETYPE_PEM);
    kfree(ca_buf);
    if (ret != WOLFSSL_SUCCESS) {
        printf("TLS: Failed to load CA bundle: %d\n", ret);
        wolfSSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

static void tls_release_global(void)
{
    if (tls_active > 0) tls_active--;
    if (tls_active == 0) wolfSSL_Cleanup();
}

static int wolf_recv_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    tcp_socket_t *tcp;
    int n;

    (void)ssl;
    tcp = (tcp_socket_t *)ctx;
    if (task_has_pending_signals()) return WOLFSSL_CBIO_ERR_GENERAL;
    netif_poll_all();
    n = tcp_recv(tcp, (uint8_t *)buf, (uint64_t)sz, 15000);
    if (n < 0) return WOLFSSL_CBIO_ERR_GENERAL;
    if (n == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return n;
}

static int wolf_send_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    tcp_socket_t *tcp;
    int r;

    (void)ssl;
    tcp = (tcp_socket_t *)ctx;
    r = tcp_send(tcp, (uint8_t *)buf, (uint64_t)sz);
    if (r < 0) return WOLFSSL_CBIO_ERR_GENERAL;
    return sz;
}

int tls_load_ca_certs(const uint8_t *pem, size_t pem_len)
{
    WOLFSSL_CTX *ctx;
    int ret;

    wolfSSL_Init();
    ctx = tls_create_ctx();
    if (!ctx) {
        wolfSSL_Cleanup();
        return -1;
    }

    ret = wolfSSL_CTX_load_verify_buffer(ctx, pem, (long)pem_len,
                                          SSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("TLS: Failed to load CA certs: %d\n", ret);
        wolfSSL_CTX_free(ctx);
        wolfSSL_Cleanup();
        return -1;
    }

    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    printf("TLS: CA certificates loaded\n");
    return 0;
}

void tls_init(void)
{
}

tls_conn_t *tls_connect(tcp_socket_t *tcp, const char *host)
{
    tls_conn_t *conn;
    int ret;
    int err;

    wolfSSL_Init();
    tls_active++;

    conn = (tls_conn_t *)kmalloc(sizeof(tls_conn_t));
    if (!conn) {
        tls_release_global();
        return NULL;
    }
    memset(conn, 0, sizeof(*conn));
    conn->tcp = tcp;

    conn->ctx = tls_create_ctx();
    if (!conn->ctx) {
        kfree(conn);
        tls_release_global();
        return NULL;
    }

    conn->ssl = wolfSSL_new(conn->ctx);
    if (!conn->ssl) {
        printf("TLS: Failed to create SSL object\n");
        wolfSSL_CTX_free(conn->ctx);
        kfree(conn);
        tls_release_global();
        return NULL;
    }

    wolfSSL_SetIOReadCtx(conn->ssl, tcp);
    wolfSSL_SetIOWriteCtx(conn->ssl, tcp);

    if (host) {
        wolfSSL_UseSNI(conn->ssl, WOLFSSL_SNI_HOST_NAME,
                       host, (unsigned short)strlen(host));
        if (wolfSSL_check_domain_name(conn->ssl, host) != WOLFSSL_SUCCESS) {
            printf("TLS: Failed to set hostname verification\n");
            wolfSSL_free(conn->ssl);
            wolfSSL_CTX_free(conn->ctx);
            kfree(conn);
            tls_release_global();
            return NULL;
        }
    }

    ret = wolfSSL_connect(conn->ssl);
    if (ret != WOLFSSL_SUCCESS) {
        err = wolfSSL_get_error(conn->ssl, ret);
        printf("TLS: Handshake failed: %d\n", err);
        wolfSSL_free(conn->ssl);
        wolfSSL_CTX_free(conn->ctx);
        kfree(conn);
        tls_release_global();
        return NULL;
    }

    return conn;
}

int tls_send(tls_conn_t *conn, const uint8_t *data, uint64_t len)
{
    int r;

    if (!conn || !conn->ssl) return -1;
    r = wolfSSL_write(conn->ssl, data, (int)len);
    if (r < 0) return -1;
    return r;
}

int tls_recv(tls_conn_t *conn, uint8_t *buf, uint64_t len, uint64_t timeout_ms)
{
    int r;
    int err;

    (void)timeout_ms;
    if (!conn || !conn->ssl) return -1;
    r = wolfSSL_read(conn->ssl, buf, (int)len);
    if (r < 0) {
        err = wolfSSL_get_error(conn->ssl, r);
        if (err == WOLFSSL_ERROR_WANT_READ) return 0;
        return -1;
    }
    return r;
}

void tls_close(tls_conn_t *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        wolfSSL_shutdown(conn->ssl);
        wolfSSL_free(conn->ssl);
    }
    if (conn->ctx) wolfSSL_CTX_free(conn->ctx);
    kfree(conn);
    tls_release_global();
}
