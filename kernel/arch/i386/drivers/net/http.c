#include <kernel/drivers/net/http.h>
#include <kernel/drivers/net/tcp.h>
#include <kernel/drivers/net/dns.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <string.h>

static int http_parse_url(const char *url, char *host, uint16_t *port, char *path) {
    const char *p = url;

    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') {
        p += 4;
        if (*p == 's') p++;
        if (*p == ':' && p[1] == '/' && p[2] == '/') {
            p += 3;
        }
    }

    int host_len = 0;
    *port = 80;

    while (*p && *p != '/' && *p != ':' && host_len < 255) {
        host[host_len++] = *p++;
    }
    host[host_len] = '\0';

    if (*p == ':') {
        p++;
        *port = 0;
        while (*p >= '0' && *p <= '9') {
            *port = *port * 10 + (*p - '0');
            p++;
        }
    }

    if (*p == '/') {
        int path_len = 0;
        while (*p && path_len < 255) {
            path[path_len++] = *p++;
        }
        path[path_len] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    return 0;
}

static int http_parse_response(uint8_t *data, uint32_t len, http_response_t *response) {
    if (len < 12) return -1;

    if (data[0] != 'H' || data[1] != 'T' || data[2] != 'T' || data[3] != 'P') {
        return -1;
    }

    uint32_t i = 9;
    response->status_code = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
        response->status_code = response->status_code * 10 + (data[i] - '0');
        i++;
    }

    uint32_t header_end = 0;
    for (i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' && data[i+2] == '\r' && data[i+3] == '\n') {
            header_end = i + 4;
            break;
        }
    }

    if (header_end == 0) return -1;

    response->content_length = 0;
    response->content_type[0] = '\0';

    for (i = 0; i < header_end; i++) {
        if (i + 15 < header_end) {
            if ((data[i] == 'C' || data[i] == 'c') &&
                (data[i+8] == 'L' || data[i+8] == 'l')) {
                uint32_t j = i;
                while (j < header_end && data[j] != ':') j++;
                if (j < header_end) {
                    j++;
                    while (j < header_end && data[j] == ' ') j++;
                    while (j < header_end && data[j] >= '0' && data[j] <= '9') {
                        response->content_length = response->content_length * 10 + (data[j] - '0');
                        j++;
                    }
                }
            }
        }
    }

    response->body = data + header_end;
    response->body_len = len - header_end;

    return 0;
}

int http_get_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint32_t timeout_ms) {
    if (!response) return -1;

    memset(response, 0, sizeof(http_response_t));

    tcp_socket_t *sock = tcp_socket_create();
    if (!sock) return -1;

    if (tcp_connect(sock, ip, port, timeout_ms) < 0) {
        tcp_socket_close(sock);
        return -1;
    }

    char request[512];
    int req_len = 0;

    const char *method = "GET ";
    for (int i = 0; method[i]; i++) request[req_len++] = method[i];
    for (int i = 0; path[i]; i++) request[req_len++] = path[i];

    const char *http_ver = " HTTP/1.0\r\nHost: ";
    for (int i = 0; http_ver[i]; i++) request[req_len++] = http_ver[i];
    for (int i = 0; host[i]; i++) request[req_len++] = host[i];

    const char *headers = "\r\nConnection: close\r\n\r\n";
    for (int i = 0; headers[i]; i++) request[req_len++] = headers[i];

    if (tcp_send(sock, (uint8_t *)request, req_len) < 0) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        return -1;
    }

    uint8_t *recv_buf = (uint8_t *)kmalloc(65536);
    if (!recv_buf) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        return -1;
    }

    uint32_t total_recv = 0;
    uint32_t start = net_get_ticks();

    while (total_recv < 65536) {
        int n = tcp_recv(sock, recv_buf + total_recv, 65536 - total_recv, 1000);
        if (n > 0) {
            total_recv += n;
        } else if (n == 0) {
            if (net_get_ticks() - start > timeout_ms) break;
            if (sock->state == TCP_STATE_CLOSE_WAIT ||
                sock->state == TCP_STATE_CLOSED) break;
        } else {
            break;
        }
    }

    tcp_disconnect(sock, 1000);
    tcp_socket_close(sock);

    if (total_recv == 0) {
        kfree(recv_buf);
        return -1;
    }

    if (http_parse_response(recv_buf, total_recv, response) < 0) {
        kfree(recv_buf);
        return -1;
    }

    response->body = (uint8_t *)kmalloc(response->body_len + 1);
    if (response->body) {
        memcpy(response->body, recv_buf + (total_recv - response->body_len), response->body_len);
        response->body[response->body_len] = '\0';
    }

    kfree(recv_buf);
    return 0;
}

int http_get(const char *host, uint16_t port, const char *path, http_response_t *response, uint32_t timeout_ms) {
    ipv4_addr_t ip;

    if (dns_resolve_timeout(host, &ip, timeout_ms) < 0) {
        return -1;
    }

    return http_get_ip(ip, port, host, path, response, timeout_ms);
}

void http_response_free(http_response_t *response) {
    if (response && response->body) {
        kfree(response->body);
        response->body = NULL;
    }
}

int http_download(const char *url, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size) {
    char host[256];
    char path[256];
    uint16_t port;

    if (http_parse_url(url, host, &port, path) < 0) {
        return -1;
    }

    http_response_t response;
    if (http_get(host, port, path, &response, 10000) < 0) {
        return -1;
    }

    if (response.status_code != 200) {
        http_response_free(&response);
        return -1;
    }

    uint32_t copy_len = response.body_len < buffer_size ? response.body_len : buffer_size;
    if (response.body) {
        memcpy(buffer, response.body, copy_len);
    }

    if (out_size) *out_size = copy_len;

    http_response_free(&response);
    return 0;
}
