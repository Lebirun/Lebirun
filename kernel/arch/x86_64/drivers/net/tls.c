#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <kernel/drivers/net/tls.h>
#include <kernel/drivers/net/tcp.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/pit.h>
#include <kernel/rtc.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

struct tls_conn {
    WOLFSSL *ssl;
    tcp_socket_t *tcp;
};

static WOLFSSL_CTX *g_ctx;

static int wolf_recv_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    tcp_socket_t *tcp;
    int n;

    (void)ssl;
    tcp = (tcp_socket_t *)ctx;
    if (task_has_pending_signals()) return WOLFSSL_CBIO_ERR_GENERAL;
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
    int ret;

    if (!g_ctx) return -1;

    ret = wolfSSL_CTX_load_verify_buffer(g_ctx, pem, (long)pem_len,
                                          SSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        printf("TLS: Failed to load CA certs: %d\n", ret);
        return -1;
    }

    printf("TLS: CA certificates loaded\n");
    return 0;
}

void tls_init(void)
{
    wolfSSL_Init();

    g_ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (!g_ctx) {
        g_ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    }
    if (!g_ctx) {
        printf("TLS: Failed to create WolfSSL context\n");
        return;
    }

    wolfSSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    wolfSSL_SetIORecv(g_ctx, wolf_recv_cb);
    wolfSSL_SetIOSend(g_ctx, wolf_send_cb);

    printf("TLS: WolfSSL initialized\n");
}

tls_conn_t *tls_connect(tcp_socket_t *tcp, const char *host)
{
    tls_conn_t *conn;
    int ret;

    if (!g_ctx) {
        printf("TLS: No WolfSSL context\n");
        return NULL;
    }

    conn = (tls_conn_t *)kmalloc(sizeof(tls_conn_t));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(*conn));
    conn->tcp = tcp;

    conn->ssl = wolfSSL_new(g_ctx);
    if (!conn->ssl) {
        printf("TLS: Failed to create SSL object\n");
        kfree(conn);
        return NULL;
    }

    wolfSSL_SetIOReadCtx(conn->ssl, tcp);
    wolfSSL_SetIOWriteCtx(conn->ssl, tcp);

    if (host) {
        wolfSSL_UseSNI(conn->ssl, WOLFSSL_SNI_HOST_NAME,
                       host, (unsigned short)strlen(host));
    }

    ret = wolfSSL_connect(conn->ssl);
    if (ret != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(conn->ssl, ret);
        printf("TLS: Handshake failed: %d\n", err);
        wolfSSL_free(conn->ssl);
        kfree(conn);
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

    (void)timeout_ms;
    if (!conn || !conn->ssl) return -1;
    r = wolfSSL_read(conn->ssl, buf, (int)len);
    if (r < 0) {
        int err = wolfSSL_get_error(conn->ssl, r);
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
    kfree(conn);
}
