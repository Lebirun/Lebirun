#include <kernel/drivers/net/http.h>
#include <kernel/drivers/net/tcp.h>
#include <kernel/drivers/net/dns.h>
#include <kernel/drivers/net/net.h>
#include <kernel/mem_map.h>
#include <kernel/tty.h>
#include <kernel/task.h>
#include <string.h>

static int http_parse_url(const char *url, char *host, uint16_t *port, char *path) {
    const char *p;
    int host_len;
    int path_len;

    p = url;

    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') {
        p += 4;
        if (*p == 's') p++;
        if (*p == ':' && p[1] == '/' && p[2] == '/') {
            p += 3;
        }
    }

    host_len = 0;
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
        path_len = 0;
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
    uint32_t i;
    uint32_t header_end;

    if (len < 12) return -1;

    if (data[0] != 'H' || data[1] != 'T' || data[2] != 'T' || data[3] != 'P') {
        return -1;
    }

    i = 9;
    response->status_code = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
        response->status_code = response->status_code * 10 + (data[i] - '0');
        i++;
    }

    header_end = 0;
    for (i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' && data[i+2] == '\r' && data[i+3] == '\n') {
            header_end = i + 4;
            break;
        }
    }

    if (header_end == 0) return -1;

    response->content_length = 0;
    response->content_type[0] = '\0';
    response->location[0] = '\0';
    response->raw_headers = data;
    response->raw_headers_len = header_end;

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
        if (i + 9 < header_end &&
            (data[i] == 'L' || data[i] == 'l') &&
            (data[i+1] == 'o' || data[i+1] == 'O') &&
            (data[i+2] == 'c' || data[i+2] == 'C') &&
            (data[i+3] == 'a' || data[i+3] == 'A') &&
            (data[i+4] == 't' || data[i+4] == 'T') &&
            (data[i+5] == 'i' || data[i+5] == 'I') &&
            (data[i+6] == 'o' || data[i+6] == 'O') &&
            (data[i+7] == 'n' || data[i+7] == 'N') &&
            data[i+8] == ':') {
            uint32_t j;
            uint32_t loc_len;

            j = i + 9;
            while (j < header_end && data[j] == ' ') j++;
            loc_len = 0;
            while (j < header_end && data[j] != '\r' && data[j] != '\n' && loc_len < 511) {
                response->location[loc_len++] = (char)data[j];
                j++;
            }
            response->location[loc_len] = '\0';
        }
    }

    response->body = data + header_end;
    response->body_len = len - header_end;

    return 0;
}

int http_get_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint32_t timeout_ms) {
    tcp_socket_t *sock;
    char *request;
    int req_len;
    const char *method;
    const char *http_ver;
    const char *headers;
    uint8_t *recv_buf;
    uint32_t total_recv;
    uint32_t buf_cap;
    uint32_t start;
    int n;
    int i;

    if (!response) return -1;

    memset(response, 0, sizeof(http_response_t));

    request = (char *)kmalloc(512);
    if (!request) return -1;

    sock = tcp_socket_create();
    if (!sock) { kfree(request); return -1; }

    if (tcp_connect(sock, ip, port, timeout_ms) < 0) {
        tcp_socket_close(sock);
        kfree(request);
        return -1;
    }

    req_len = 0;

    method = "GET ";
    for (i = 0; method[i] && req_len < 511; i++) request[req_len++] = method[i];
    for (i = 0; path[i] && req_len < 511; i++) request[req_len++] = path[i];

    http_ver = " HTTP/1.0\r\nHost: ";
    for (i = 0; http_ver[i] && req_len < 511; i++) request[req_len++] = http_ver[i];
    for (i = 0; host[i] && req_len < 511; i++) request[req_len++] = host[i];

    headers = "\r\nConnection: close\r\n\r\n";
    for (i = 0; headers[i] && req_len < 511; i++) request[req_len++] = headers[i];
    request[req_len] = '\0';

    if (tcp_send(sock, (uint8_t *)request, req_len) < 0) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        kfree(request);
        return -1;
    }

    kfree(request);

    recv_buf = (uint8_t *)kmalloc(16384);
    if (!recv_buf) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        return -1;
    }

    total_recv = 0;
    buf_cap = 16384;
    start = net_get_ticks();

    for (;;) {
        uint32_t header_end_pos;
        uint32_t expected_total;

        if (total_recv + 4096 > buf_cap) {
            uint32_t new_cap = buf_cap * 2;
            uint8_t *new_buf = (uint8_t *)krealloc(recv_buf, new_cap);
            if (!new_buf) break;
            recv_buf = new_buf;
            buf_cap = new_cap;
        }
        n = tcp_recv(sock, recv_buf + total_recv, buf_cap - total_recv, 1000);
        if (n > 0) {
            total_recv += n;
            start = net_get_ticks();

            header_end_pos = 0;
            {
                uint32_t si;
                for (si = 0; si + 3 < total_recv; si++) {
                    if (recv_buf[si] == '\r' && recv_buf[si+1] == '\n' &&
                        recv_buf[si+2] == '\r' && recv_buf[si+3] == '\n') {
                        header_end_pos = si + 4;
                        break;
                    }
                }
            }
            if (header_end_pos > 0) {
                http_response_t *tmp_resp;
                tmp_resp = (http_response_t *)kmalloc(sizeof(http_response_t));
                if (tmp_resp) {
                    memset(tmp_resp, 0, sizeof(*tmp_resp));
                    if (http_parse_response(recv_buf, total_recv, tmp_resp) == 0 &&
                        tmp_resp->content_length > 0) {
                        expected_total = header_end_pos + tmp_resp->content_length;
                        kfree(tmp_resp);
                        if (total_recv >= expected_total) break;
                    } else {
                        kfree(tmp_resp);
                    }
                }
            }
            if (sock->state == TCP_STATE_CLOSE_WAIT ||
                sock->state == TCP_STATE_CLOSED) {
                break;
            }
        } else if (n == 0) {
            if (sock->state == TCP_STATE_CLOSE_WAIT ||
                sock->state == TCP_STATE_CLOSED) break;
            if (net_get_ticks() - start > timeout_ms) break;
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

    if (response->raw_headers_len > 0) {
        uint8_t *hdr_copy = (uint8_t *)kmalloc(response->raw_headers_len);
        if (hdr_copy) {
            memcpy(hdr_copy, response->raw_headers, response->raw_headers_len);
        }
        response->raw_headers = hdr_copy;
    } else {
        response->raw_headers = NULL;
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
    if (!response) return;
    if (response->body) {
        kfree(response->body);
        response->body = NULL;
    }
    if (response->raw_headers) {
        kfree(response->raw_headers);
        response->raw_headers = NULL;
    }
}

static int http_is_redirect(int status_code) {
    return status_code == 301 || status_code == 302 ||
           status_code == 307 || status_code == 308;
}

int http_download(const char *url, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size, int *out_status) {
    return http_download_ex(url, buffer, buffer_size, out_size, out_status,
                            HTTP_MAX_REDIRECTS_DEFAULT, NULL, 0, NULL);
}

int http_download_ex(const char *url, uint8_t *buffer, uint32_t buffer_size,
                     uint32_t *out_size, int *out_status, int max_redirects,
                     uint8_t *headers_buf, uint32_t headers_buf_size, uint32_t *out_headers_len) {
    char *host;
    char *path;
    char *current_url;
    http_response_t *response;
    uint16_t port;
    int attempt;
    int max_attempts;
    int ret;
    uint32_t copy_len;
    int redir;
    uint32_t url_len;
    uint32_t hdr_copy;

    host = (char *)kmalloc(256);
    if (!host) return -1;
    path = (char *)kmalloc(256);
    if (!path) { kfree(host); return -1; }
    current_url = (char *)kmalloc(512);
    if (!current_url) { kfree(host); kfree(path); return -1; }
    response = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!response) { kfree(host); kfree(path); kfree(current_url); return -1; }

    url_len = 0;
    while (url[url_len] && url_len < 511) {
        current_url[url_len] = url[url_len];
        url_len++;
    }
    current_url[url_len] = '\0';

    for (redir = 0; redir <= max_redirects; redir++) {
        if (http_parse_url(current_url, host, &port, path) < 0) {
            kfree(host); kfree(path); kfree(current_url); kfree(response);
            return -1;
        }

        max_attempts = 2;
        ret = -1;
        for (attempt = 0; attempt < max_attempts; attempt++) {
            ret = http_get(host, port, path, response, 15000);
            if (ret == 0) {
                break;
            }
            if (attempt + 1 < max_attempts) {
                sleep_ms(500);
            }
        }

        if (ret < 0) {
            kfree(host); kfree(path); kfree(current_url); kfree(response);
            return -1;
        }

        if (http_is_redirect(response->status_code) && response->location[0] && redir < max_redirects) {
            if (response->location[0] == '/') {
                uint32_t off = 0;
                uint32_t hi;
                const char *scheme = "http://";
                for (hi = 0; scheme[hi]; hi++)
                    current_url[off++] = scheme[hi];
                for (hi = 0; host[hi] && off < 510; hi++)
                    current_url[off++] = host[hi];
                for (hi = 0; response->location[hi] && off < 511; hi++)
                    current_url[off++] = response->location[hi];
                current_url[off] = '\0';
            } else {
                uint32_t li = 0;
                while (response->location[li] && li < 511) {
                    current_url[li] = response->location[li];
                    li++;
                }
                current_url[li] = '\0';
            }
            http_response_free(response);
            continue;
        }

        if (out_status) *out_status = response->status_code;

        if (headers_buf && headers_buf_size > 0 && response->raw_headers && response->raw_headers_len > 0) {
            hdr_copy = response->raw_headers_len < headers_buf_size ? response->raw_headers_len : headers_buf_size;
            memcpy(headers_buf, response->raw_headers, hdr_copy);
            if (out_headers_len) *out_headers_len = hdr_copy;
        } else if (out_headers_len) {
            *out_headers_len = 0;
        }

        copy_len = response->body_len < buffer_size ? response->body_len : buffer_size;
        if (response->body) {
            memcpy(buffer, response->body, copy_len);
        }

        if (out_size) *out_size = copy_len;

        http_response_free(response);
        kfree(host); kfree(path); kfree(current_url); kfree(response);
        return 0;
    }

    kfree(host); kfree(path); kfree(current_url); kfree(response);
    return -1;
}

int http_post_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path,
                 const char *content_type, const uint8_t *body, uint32_t body_len,
                 http_response_t *response, uint32_t timeout_ms) {
    tcp_socket_t *sock;
    char *request;
    int req_len;
    char len_str[16];
    char rev[16];
    int li;
    int ri;
    uint32_t tmp;
    uint8_t *recv_buf;
    uint32_t total_recv;
    uint32_t start;
    int n;
    int i;

    if (!response) return -1;

    memset(response, 0, sizeof(http_response_t));

    request = (char *)kmalloc(512);
    if (!request) return -1;

    sock = tcp_socket_create();
    if (!sock) { kfree(request); return -1; }

    if (tcp_connect(sock, ip, port, timeout_ms) < 0) {
        tcp_socket_close(sock);
        kfree(request);
        return -1;
    }

    req_len = 0;

    for (i = 0; "POST "[i] && req_len < 511; i++) request[req_len++] = "POST "[i];
    for (i = 0; path[i] && req_len < 511; i++) request[req_len++] = path[i];
    for (i = 0; " HTTP/1.0\r\nHost: "[i] && req_len < 511; i++) request[req_len++] = " HTTP/1.0\r\nHost: "[i];
    for (i = 0; host[i] && req_len < 511; i++) request[req_len++] = host[i];
    for (i = 0; "\r\nContent-Type: "[i] && req_len < 511; i++) request[req_len++] = "\r\nContent-Type: "[i];

    if (content_type) {
        for (i = 0; content_type[i] && req_len < 511; i++) request[req_len++] = content_type[i];
    } else {
        for (i = 0; "application/x-www-form-urlencoded"[i] && req_len < 511; i++)
            request[req_len++] = "application/x-www-form-urlencoded"[i];
    }

    for (i = 0; "\r\nContent-Length: "[i] && req_len < 511; i++) request[req_len++] = "\r\nContent-Length: "[i];

    li = 0;
    tmp = body_len;
    if (tmp == 0) {
        len_str[li++] = '0';
    } else {
        ri = 0;
        while (tmp > 0) {
            rev[ri++] = '0' + (char)(tmp % 10);
            tmp /= 10;
        }
        while (ri > 0) {
            len_str[li++] = rev[--ri];
        }
    }
    len_str[li] = '\0';

    for (i = 0; len_str[i] && req_len < 511; i++) request[req_len++] = len_str[i];
    for (i = 0; "\r\nConnection: close\r\n\r\n"[i] && req_len < 511; i++)
        request[req_len++] = "\r\nConnection: close\r\n\r\n"[i];
    request[req_len] = '\0';

    if (tcp_send(sock, (uint8_t *)request, req_len) < 0) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        kfree(request);
        return -1;
    }

    kfree(request);

    if (body && body_len > 0) {
        if (tcp_send(sock, (uint8_t *)body, body_len) < 0) {
            tcp_disconnect(sock, 1000);
            tcp_socket_close(sock);
            return -1;
        }
    }

    recv_buf = (uint8_t *)kmalloc(16384);
    if (!recv_buf) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        return -1;
    }

    total_recv = 0;
    start = net_get_ticks();
    {
    uint32_t buf_cap = 16384;

    for (;;) {
        uint32_t header_end_pos;
        uint32_t expected_total;

        if (total_recv + 4096 > buf_cap) {
            uint32_t new_cap = buf_cap * 2;
            uint8_t *new_buf = (uint8_t *)krealloc(recv_buf, new_cap);
            if (!new_buf) break;
            recv_buf = new_buf;
            buf_cap = new_cap;
        }
        n = tcp_recv(sock, recv_buf + total_recv, buf_cap - total_recv, 1000);
        if (n > 0) {
            total_recv += n;
            start = net_get_ticks();

            header_end_pos = 0;
            {
                uint32_t si;
                for (si = 0; si + 3 < total_recv; si++) {
                    if (recv_buf[si] == '\r' && recv_buf[si+1] == '\n' &&
                        recv_buf[si+2] == '\r' && recv_buf[si+3] == '\n') {
                        header_end_pos = si + 4;
                        break;
                    }
                }
            }
            if (header_end_pos > 0) {
                http_response_t *tmp_resp;
                tmp_resp = (http_response_t *)kmalloc(sizeof(http_response_t));
                if (tmp_resp) {
                    memset(tmp_resp, 0, sizeof(*tmp_resp));
                    if (http_parse_response(recv_buf, total_recv, tmp_resp) == 0 &&
                        tmp_resp->content_length > 0) {
                        expected_total = header_end_pos + tmp_resp->content_length;
                        kfree(tmp_resp);
                        if (total_recv >= expected_total) break;
                    } else {
                        kfree(tmp_resp);
                    }
                }
            }
            if (sock->state == TCP_STATE_CLOSE_WAIT ||
                sock->state == TCP_STATE_CLOSED) {
                break;
            }
        } else if (n == 0) {
            if (sock->state == TCP_STATE_CLOSE_WAIT ||
                sock->state == TCP_STATE_CLOSED) break;
            if (net_get_ticks() - start > timeout_ms) break;
        } else {
            break;
        }
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

    if (response->raw_headers_len > 0) {
        uint8_t *hdr_copy = (uint8_t *)kmalloc(response->raw_headers_len);
        if (hdr_copy) {
            memcpy(hdr_copy, response->raw_headers, response->raw_headers_len);
        }
        response->raw_headers = hdr_copy;
    } else {
        response->raw_headers = NULL;
    }

    kfree(recv_buf);
    return 0;
}

int http_post(const char *host, uint16_t port, const char *path,
              const char *content_type, const uint8_t *body, uint32_t body_len,
              http_response_t *response, uint32_t timeout_ms) {
    ipv4_addr_t ip;

    if (dns_resolve_timeout(host, &ip, timeout_ms) < 0) {
        return -1;
    }

    return http_post_ip(ip, port, host, path, content_type, body, body_len, response, timeout_ms);
}

int http_post_download(const char *url, const char *content_type,
                       const uint8_t *post_body, uint32_t post_body_len,
                       uint8_t *buffer, uint32_t buffer_size,
                       uint32_t *out_size, int *out_status) {
    char *host;
    char *path;
    uint16_t port;
    http_response_t *response;
    uint32_t copy_len;

    host = (char *)kmalloc(256);
    if (!host) return -1;
    path = (char *)kmalloc(256);
    if (!path) { kfree(host); return -1; }
    response = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!response) { kfree(host); kfree(path); return -1; }

    if (http_parse_url(url, host, &port, path) < 0) {
        kfree(host); kfree(path); kfree(response);
        return -1;
    }

    {
        int attempt;
        int max_attempts = 2;
        int ok = 0;
        for (attempt = 0; attempt < max_attempts; attempt++) {
            if (http_post(host, port, path, content_type, post_body, post_body_len, response, 15000) == 0) {
                ok = 1;
                break;
            }
            if (attempt + 1 < max_attempts) {
                sleep_ms(500);
            }
        }
        if (!ok) {
            kfree(host); kfree(path); kfree(response);
            return -1;
        }
    }

    if (out_status) *out_status = response->status_code;

    copy_len = response->body_len < buffer_size ? response->body_len : buffer_size;
    if (response->body) {
        memcpy(buffer, response->body, copy_len);
    }

    if (out_size) *out_size = copy_len;

    http_response_free(response);
    kfree(host); kfree(path); kfree(response);
    return 0;
}
