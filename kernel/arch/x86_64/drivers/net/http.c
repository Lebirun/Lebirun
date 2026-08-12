#include <lebirun/drivers/net/http.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/drivers/net/tls.h>
#include <lebirun/drivers/net/dns.h>
#include <lebirun/drivers/net/net.h>
#include <lebirun/mem_map.h>
#include <lebirun/tty.h>
#include <lebirun/task.h>
#include <string.h>

#define HTTP_RECV_BUF_INIT 4096

static char *http_dup_string(const char *value) {
    size_t length;
    char *copy;

    if (!value) return NULL;
    length = strlen(value);
    copy = (char *)kmalloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

__attribute__((weak)) tls_conn_t *tls_connect(tcp_socket_t *tcp, const char *host) {
    (void)tcp;
    (void)host;
    return NULL;
}

__attribute__((weak)) int tls_send(tls_conn_t *conn, const uint8_t *data, uint64_t len) {
    (void)conn;
    (void)data;
    (void)len;
    return -1;
}

__attribute__((weak)) int tls_recv(tls_conn_t *conn, uint8_t *buf, uint64_t len, uint64_t timeout_ms) {
    (void)conn;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -1;
}

__attribute__((weak)) void tls_close(tls_conn_t *conn) {
    (void)conn;
}

static int http_parse_url(const char *url, char **host_out, uint16_t *port,
                          char **path_out, int *is_https) {
    const char *p;
    const char *host_start;
    size_t host_len;
    size_t path_len;
    char *host;
    char *path;
    uint32_t parsed_port;
    int https;

    if (!url || !host_out || !path_out || !port) return -1;
    *host_out = NULL;
    *path_out = NULL;

    p = url;
    https = 0;

    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') {
        p += 4;
        if (*p == 's') {
            https = 1;
            p++;
        }
        if (*p == ':' && p[1] == '/' && p[2] == '/') {
            p += 3;
        }
    }

    host_start = p;
    *port = https ? 443 : 80;
    if (is_https) *is_https = https;

    while (*p && *p != '/' && *p != ':') p++;
    host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len == SIZE_MAX) return -1;
    host = (char *)kmalloc(host_len + 1);
    if (!host) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (*p == ':') {
        p++;
        parsed_port = 0;
        if (*p < '0' || *p > '9') {
            kfree(host);
            return -1;
        }
        while (*p >= '0' && *p <= '9') {
            parsed_port = parsed_port * 10 + (uint32_t)(*p - '0');
            if (parsed_port > 65535) {
                kfree(host);
                return -1;
            }
            p++;
        }
        *port = (uint16_t)parsed_port;
    }

    if (*p == '/') {
        path_len = strlen(p);
        if (path_len == SIZE_MAX) {
            kfree(host);
            return -1;
        }
        path = (char *)kmalloc(path_len + 1);
        if (!path) {
            kfree(host);
            return -1;
        }
        memcpy(path, p, path_len);
        path[path_len] = '\0';
    } else {
        if (*p != '\0') {
            kfree(host);
            return -1;
        }
        path = (char *)kmalloc(2);
        if (!path) {
            kfree(host);
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
    }

    *host_out = host;
    *path_out = path;
    return 0;
}

static int http_parse_response(uint8_t *data, uint64_t len, http_response_t *response) {
    uint64_t i;
    uint64_t header_end;
    uint64_t j;
    uint64_t loc_len;

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
        if (i + 16 < header_end &&
            (data[i]    == 'C' || data[i]    == 'c') &&
            (data[i+1]  == 'o' || data[i+1]  == 'O') &&
            (data[i+2]  == 'n' || data[i+2]  == 'N') &&
            (data[i+3]  == 't' || data[i+3]  == 'T') &&
            (data[i+4]  == 'e' || data[i+4]  == 'E') &&
            (data[i+5]  == 'n' || data[i+5]  == 'N') &&
            (data[i+6]  == 't' || data[i+6]  == 'T') &&
            (data[i+7]  == '-') &&
            (data[i+8]  == 'L' || data[i+8]  == 'l') &&
            (data[i+9]  == 'e' || data[i+9]  == 'E') &&
            (data[i+10] == 'n' || data[i+10] == 'N') &&
            (data[i+11] == 'g' || data[i+11] == 'G') &&
            (data[i+12] == 't' || data[i+12] == 'T') &&
            (data[i+13] == 'h' || data[i+13] == 'H') &&
            data[i+14] == ':') {
            j = i + 15;
            while (j < header_end && data[j] == ' ') j++;
            response->content_length = 0;
            while (j < header_end && data[j] >= '0' && data[j] <= '9') {
                if (response->content_length >
                    (UINT64_MAX - (uint64_t)(data[j] - '0')) / 10)
                    return -1;
                response->content_length = response->content_length * 10 + (data[j] - '0');
                j++;
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

int http_get_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint64_t timeout_ms) {
    return http_get_ip_tls(ip, port, host, path, response, timeout_ms, 0);
}

int http_get_ip_tls(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint64_t timeout_ms, int use_tls) {
    tcp_socket_t *sock;
    tls_conn_t *tls;
    char *request;
    uint64_t req_len;
    uint64_t request_len;
    uint64_t method_len;
    uint64_t version_len;
    uint64_t headers_len;
    uint64_t host_len;
    uint64_t path_len;
    const char *method;
    const char *http_ver;
    const char *headers;
    uint8_t *recv_buf;
    uint64_t total_recv;
    uint64_t buf_cap;
    uint64_t start;
    int n;
    int sig_ret;
    uint64_t header_end_pos;
    uint64_t expected_total;
    uint64_t si;
    uint64_t new_cap;
    uint8_t *new_buf;
    http_response_t *tmp_resp;
    uint8_t *hdr_copy;
    int receive_error;

    if (!response) return -1;

    memset(response, 0, sizeof(http_response_t));

    method = "GET ";
    http_ver = " HTTP/1.0\r\nHost: ";
    headers = "\r\nConnection: close\r\n\r\n";
    method_len = strlen(method);
    version_len = strlen(http_ver);
    headers_len = strlen(headers);
    host_len = strlen(host);
    path_len = strlen(path);
    if (method_len > UINT64_MAX - path_len ||
        method_len + path_len > UINT64_MAX - version_len ||
        method_len + path_len + version_len > UINT64_MAX - host_len ||
        method_len + path_len + version_len + host_len >
        UINT64_MAX - headers_len - 1) return -1;
    request_len = method_len + path_len + version_len + host_len +
                  headers_len;
    request = (char *)kmalloc(request_len + 1);
    if (!request) return -1;

    tls = NULL;
    sock = tcp_socket_create();
    if (!sock) { kfree(request); return -1; }

    if (tcp_connect(sock, ip, port, timeout_ms) < 0) {
        sig_ret = task_has_pending_signals() ? -4 : -1;
        tcp_socket_close(sock);
        kfree(request);
        return sig_ret;
    }

    if (use_tls) {
        tls = tls_connect(sock, host);
        if (!tls) {
            sig_ret = task_has_pending_signals() ? -4 : -1;
            tcp_disconnect(sock, 1000);
            tcp_socket_close(sock);
            kfree(request);
            return sig_ret;
        }
    }

    req_len = 0;

    memcpy(request + req_len, method, method_len);
    req_len += method_len;
    memcpy(request + req_len, path, path_len);
    req_len += path_len;
    memcpy(request + req_len, http_ver, version_len);
    req_len += version_len;
    memcpy(request + req_len, host, host_len);
    req_len += host_len;
    memcpy(request + req_len, headers, headers_len);
    req_len += headers_len;
    request[req_len] = '\0';

    if (use_tls) {
        if (tls_send(tls, (uint8_t *)request, req_len) < 0) {
            tls_close(tls);
            tcp_disconnect(sock, 1000);
            tcp_socket_close(sock);
            kfree(request);
            return -1;
        }
    } else {
        if (tcp_send(sock, (uint8_t *)request, req_len) < 0) {
            tcp_disconnect(sock, 1000);
            tcp_socket_close(sock);
            kfree(request);
            return -1;
        }
    }

    kfree(request);

    recv_buf = (uint8_t *)kmalloc(HTTP_RECV_BUF_INIT);
    if (!recv_buf) {
        if (tls) tls_close(tls);
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        return -1;
    }

    total_recv = 0;
    buf_cap = HTTP_RECV_BUF_INIT;
    start = net_get_ticks();
    expected_total = 0;
    receive_error = 0;

    for (;;) {
        if (task_has_pending_signals()) {
            kfree(recv_buf);
            if (tls) tls_close(tls);
            tcp_disconnect(sock, 500);
            tcp_socket_close(sock);
            return -4;
        }

        if (expected_total == 0 && total_recv > UINT64_MAX - 4096) {
            receive_error = 1;
            break;
        }
        if (expected_total == 0 && total_recv + 4096 > buf_cap) {
            if (buf_cap > UINT64_MAX / 2) {
                receive_error = 1;
                break;
            }
            new_cap = buf_cap * 2;
            if (expected_total > 0 && new_cap < expected_total)
                new_cap = expected_total + 256;
            new_buf = (uint8_t *)krealloc(recv_buf, new_cap);
            if (!new_buf) {
                receive_error = 1;
                break;
            }
            recv_buf = new_buf;
            buf_cap = new_cap;
        }
        if (use_tls) {
            n = tls_recv(tls, recv_buf + total_recv, buf_cap - total_recv, 1000);
        } else {
            n = tcp_recv(sock, recv_buf + total_recv, buf_cap - total_recv, 1000);
        }
        if (n > 0) {
            total_recv += n;
            start = net_get_ticks();

            if (task_has_pending_signals()) {
                kfree(recv_buf);
                if (tls) tls_close(tls);
                tcp_disconnect(sock, 500);
                tcp_socket_close(sock);
                return -4;
            }

            if (expected_total == 0) {
                header_end_pos = 0;
                for (si = 0; si + 3 < total_recv; si++) {
                    if (recv_buf[si] == '\r' && recv_buf[si+1] == '\n' &&
                        recv_buf[si+2] == '\r' && recv_buf[si+3] == '\n') {
                        header_end_pos = si + 4;
                        break;
                    }
                }
                if (header_end_pos > 0) {
                    tmp_resp = (http_response_t *)kmalloc(sizeof(http_response_t));
                    if (tmp_resp) {
                        memset(tmp_resp, 0, sizeof(*tmp_resp));
                        if (http_parse_response(recv_buf, total_recv, tmp_resp) == 0 &&
                            tmp_resp->content_length > 0) {
                            if (tmp_resp->content_length > UINT64_MAX - header_end_pos) {
                                kfree(tmp_resp);
                                receive_error = 1;
                                break;
                            }
                            expected_total = header_end_pos + tmp_resp->content_length;
                            if (expected_total > buf_cap) {
                                new_buf = (uint8_t *)krealloc(recv_buf, expected_total);
                                if (new_buf) {
                                    recv_buf = new_buf;
                                    buf_cap = expected_total;
                                } else {
                                    kfree(tmp_resp);
                                    receive_error = 1;
                                    break;
                                }
                            }
                        }
                        kfree(tmp_resp);
                    }
                }
            }

            if (expected_total > 0 && total_recv >= expected_total) break;
        } else if (n == 0) {
            if (sock->state == TCP_STATE_CLOSE_WAIT ||
                sock->state == TCP_STATE_CLOSED) {
                break;
            }
            if (net_get_ticks() - start > timeout_ms) break;
        } else {
            break;
        }
    }

    if (tls) tls_close(tls);
    tcp_disconnect(sock, 1000);
    tcp_socket_close(sock);

    if (receive_error) {
        kfree(recv_buf);
        return -1;
    }

    if (task_has_pending_signals()) {
        kfree(recv_buf);
        return -4;
    }

    if (total_recv == 0) {
        kfree(recv_buf);
        return -1;
    }

    if (http_parse_response(recv_buf, total_recv, response) < 0) {
        kfree(recv_buf);
        return -1;
    }

    response->body = (uint8_t *)kmalloc(response->body_len + 1);
    if (!response->body) {
        memset(response, 0, sizeof(http_response_t));
        kfree(recv_buf);
        return -1;
    }
    memcpy(response->body, recv_buf + (total_recv - response->body_len), response->body_len);
    response->body[response->body_len] = '\0';

    if (response->raw_headers_len > 0) {
        hdr_copy = (uint8_t *)kmalloc(response->raw_headers_len);
        if (hdr_copy) {
            memcpy(hdr_copy, response->raw_headers, response->raw_headers_len);
        }
        if (!hdr_copy) {
            kfree(response->body);
            memset(response, 0, sizeof(http_response_t));
            kfree(recv_buf);
            return -1;
        }
        response->raw_headers = hdr_copy;
    } else {
        response->raw_headers = NULL;
    }

    kfree(recv_buf);
    return 0;
}

int http_get(const char *host, uint16_t port, const char *path, http_response_t *response, uint64_t timeout_ms) {
    ipv4_addr_t ip;

    if (dns_resolve_timeout(host, &ip, timeout_ms) < 0) {
        return -1;
    }

    return http_get_ip(ip, port, host, path, response, timeout_ms);
}

int http_get_tls(const char *host, uint16_t port, const char *path, http_response_t *response, uint64_t timeout_ms, int use_tls) {
    ipv4_addr_t ip;

    if (dns_resolve_timeout(host, &ip, timeout_ms) < 0) {
        return task_has_pending_signals() ? -4 : -1;
    }

    return http_get_ip_tls(ip, port, host, path, response, timeout_ms, use_tls);
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

static int http_name_equal(const uint8_t *value, size_t length,
                           const char *name) {
    size_t i;
    uint8_t a;
    uint8_t b;

    if (strlen(name) != length) return 0;
    for (i = 0; i < length; i++) {
        a = value[i];
        b = (uint8_t)name[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (uint8_t)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

char *http_response_header_dup(const http_response_t *response,
                               const char *name) {
    const uint8_t *headers;
    uint64_t length;
    uint64_t line;
    uint64_t colon;
    uint64_t end;
    uint64_t value;
    uint64_t value_length;
    char *location;

    if (!response || !response->raw_headers || !name || !name[0]) return NULL;
    headers = response->raw_headers;
    length = response->raw_headers_len;
    line = 0;
    while (line < length) {
        end = line;
        while (end < length && headers[end] != '\r' && headers[end] != '\n') end++;
        colon = line;
        while (colon < end && headers[colon] != ':') colon++;
        if (colon < end && http_name_equal(headers + line,
                                            (size_t)(colon - line),
                                            name)) {
            value = colon + 1;
            while (value < end &&
                   (headers[value] == ' ' || headers[value] == '\t')) value++;
            while (end > value &&
                   (headers[end - 1] == ' ' || headers[end - 1] == '\t')) end--;
            value_length = end - value;
            location = (char *)kmalloc((size_t)value_length + 1);
            if (!location) return NULL;
            memcpy(location, headers + value, (size_t)value_length);
            location[value_length] = '\0';
            return location;
        }
        line = end;
        while (line < length &&
               (headers[line] == '\r' || headers[line] == '\n')) line++;
    }
    return NULL;
}

static char *http_redirect_url(const char *location, int is_https,
                               const char *host) {
    const char *scheme;
    size_t scheme_len;
    size_t host_len;
    size_t location_len;
    size_t total;
    char *url;

    if (!location || !location[0]) return NULL;
    if (location[0] != '/') return http_dup_string(location);
    scheme = is_https ? "https://" : "http://";
    scheme_len = strlen(scheme);
    host_len = strlen(host);
    location_len = strlen(location);
    if (scheme_len > SIZE_MAX - host_len ||
        scheme_len + host_len > SIZE_MAX - location_len - 1) return NULL;
    total = scheme_len + host_len + location_len;
    url = (char *)kmalloc(total + 1);
    if (!url) return NULL;
    memcpy(url, scheme, scheme_len);
    memcpy(url + scheme_len, host, host_len);
    memcpy(url + scheme_len + host_len, location, location_len + 1);
    return url;
}

int http_download(const char *url, uint8_t *buffer, uint64_t buffer_size, uint64_t *out_size, int *out_status) {
    return http_download_ex(url, buffer, buffer_size, out_size, out_status,
                            HTTP_MAX_REDIRECTS_DEFAULT, NULL, NULL);
}

int http_download_ex(const char *url, uint8_t *buffer, uint64_t buffer_size,
                     uint64_t *out_size, int *out_status, int max_redirects,
                     uint8_t **out_headers, uint64_t *out_headers_len) {
    char *host;
    char *path;
    char *current_url;
    http_response_t *response;
    uint16_t port;
    int attempt;
    int max_attempts;
    int ret;
    uint64_t copy_len;
    int redir;
    int is_https;
    int saved_ret;
    char *location;
    char *next_url;

    host = NULL;
    path = NULL;
    current_url = http_dup_string(url);
    if (!current_url) return -1;
    response = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!response) { kfree(host); kfree(path); kfree(current_url); return -1; }
    if (out_headers) *out_headers = NULL;
    if (out_headers_len) *out_headers_len = 0;

    redir = 0;
    for (;;) {
        if (host) kfree(host);
        if (path) kfree(path);
        host = NULL;
        path = NULL;
        is_https = 0;
        if (http_parse_url(current_url, &host, &port, &path, &is_https) < 0) {
            kfree(host); kfree(path); kfree(current_url); kfree(response);
            return -1;
        }

        max_attempts = 2;
        ret = -1;
        for (attempt = 0; attempt < max_attempts; attempt++) {
            ret = http_get_tls(host, port, path, response, 60000, is_https);
            if (ret == 0 || ret == -4) {
                break;
            }
            if (attempt + 1 < max_attempts) {
                sleep_ms(500);
            }
        }

        if (ret < 0) {
            saved_ret = ret;
            kfree(host); kfree(path); kfree(current_url); kfree(response);
            return saved_ret;
        }

        location = http_response_header_dup(response, "location");
        if (http_is_redirect(response->status_code) && location &&
            redir < max_redirects) {
            next_url = http_redirect_url(location, is_https, host);
            kfree(location);
            if (!next_url) {
                http_response_free(response);
                kfree(host); kfree(path); kfree(current_url); kfree(response);
                return -1;
            }
            kfree(current_url);
            current_url = next_url;
            http_response_free(response);
            redir++;
            continue;
        }
        if (location) kfree(location);

        if (out_status) *out_status = response->status_code;

        if (out_headers && response->raw_headers &&
            response->raw_headers_len > 0) {
            *out_headers = response->raw_headers;
            if (out_headers_len) *out_headers_len = response->raw_headers_len;
            response->raw_headers = NULL;
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
}

int http_download_alloc(const char *url, uint8_t **out_body, uint64_t *out_size,
                        int *out_status, int max_redirects,
                        uint8_t **out_headers, uint64_t *out_headers_len) {
    char *host;
    char *path;
    char *current_url;
    http_response_t *response;
    uint16_t port;
    int attempt;
    int max_attempts;
    int ret;
    int redir;
    int is_https;
    int saved_ret;
    char *location;
    char *next_url;

    if (!out_body) return -1;
    *out_body = NULL;
    if (out_headers) *out_headers = NULL;
    if (out_headers_len) *out_headers_len = 0;

    host = NULL;
    path = NULL;
    current_url = http_dup_string(url);
    if (!current_url) return -1;
    response = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!response) { kfree(host); kfree(path); kfree(current_url); return -1; }

    redir = 0;
    for (;;) {
        if (host) kfree(host);
        if (path) kfree(path);
        host = NULL;
        path = NULL;
        is_https = 0;
        if (http_parse_url(current_url, &host, &port, &path, &is_https) < 0) {
            kfree(host); kfree(path); kfree(current_url); kfree(response);
            return -1;
        }

        max_attempts = 2;
        ret = -1;
        for (attempt = 0; attempt < max_attempts; attempt++) {
            ret = http_get_tls(host, port, path, response, 60000, is_https);
            if (ret == 0 || ret == -4) break;
            if (attempt + 1 < max_attempts) sleep_ms(500);
        }

        if (ret < 0) {
            saved_ret = ret;
            kfree(host); kfree(path); kfree(current_url); kfree(response);
            return saved_ret;
        }

        location = http_response_header_dup(response, "location");
        if (http_is_redirect(response->status_code) && location &&
            redir < max_redirects) {
            next_url = http_redirect_url(location, is_https, host);
            kfree(location);
            if (!next_url) {
                http_response_free(response);
                kfree(host); kfree(path); kfree(current_url); kfree(response);
                return -1;
            }
            kfree(current_url);
            current_url = next_url;
            http_response_free(response);
            redir++;
            continue;
        }
        if (location) kfree(location);

        if (out_status) *out_status = response->status_code;

        if (out_headers && response->raw_headers &&
            response->raw_headers_len > 0) {
            *out_headers = response->raw_headers;
            if (out_headers_len) *out_headers_len = response->raw_headers_len;
            response->raw_headers = NULL;
        }

        *out_body = response->body;
        if (out_size) *out_size = response->body_len;
        response->body = NULL;

        http_response_free(response);
        kfree(host); kfree(path); kfree(current_url); kfree(response);
        return 0;
    }
}

int http_post_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path,
                 const char *content_type, const uint8_t *body, uint64_t body_len,
                 http_response_t *response, uint64_t timeout_ms) {
    tcp_socket_t *sock;
    char *request;
    uint64_t req_len;
    uint64_t request_len;
    uint64_t path_len;
    uint64_t host_len;
    uint64_t content_type_len;
    uint64_t prefix_len;
    uint64_t host_header_len;
    uint64_t type_header_len;
    uint64_t length_header_len;
    uint64_t suffix_len;
    const char *actual_content_type;
    const char *prefix;
    const char *host_header;
    const char *type_header;
    const char *length_header;
    const char *suffix;
    char len_str[32];
    char rev[32];
    int li;
    int ri;
    uint64_t tmp;
    uint8_t *recv_buf;
    uint64_t total_recv;
    uint64_t start;
    int n;
    uint64_t buf_cap;
    uint64_t header_end_pos;
    uint64_t expected_total;
    uint64_t si;
    uint64_t new_cap;
    uint8_t *new_buf;
    http_response_t *tmp_resp;
    uint8_t *hdr_copy;
    int receive_error;

    if (!response) return -1;

    memset(response, 0, sizeof(http_response_t));

    prefix = "POST ";
    host_header = " HTTP/1.0\r\nHost: ";
    type_header = "\r\nContent-Type: ";
    length_header = "\r\nContent-Length: ";
    suffix = "\r\nConnection: close\r\n\r\n";
    actual_content_type = content_type ? content_type :
                          "application/x-www-form-urlencoded";
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
        while (ri > 0) len_str[li++] = rev[--ri];
    }
    len_str[li] = '\0';
    prefix_len = strlen(prefix);
    path_len = strlen(path);
    host_header_len = strlen(host_header);
    host_len = strlen(host);
    type_header_len = strlen(type_header);
    content_type_len = strlen(actual_content_type);
    length_header_len = strlen(length_header);
    suffix_len = strlen(suffix);
    if (prefix_len > UINT64_MAX - path_len ||
        prefix_len + path_len > UINT64_MAX - host_header_len ||
        prefix_len + path_len + host_header_len > UINT64_MAX - host_len ||
        prefix_len + path_len + host_header_len + host_len >
        UINT64_MAX - type_header_len ||
        prefix_len + path_len + host_header_len + host_len + type_header_len >
        UINT64_MAX - content_type_len ||
        prefix_len + path_len + host_header_len + host_len + type_header_len +
        content_type_len > UINT64_MAX - length_header_len ||
        prefix_len + path_len + host_header_len + host_len + type_header_len +
        content_type_len + length_header_len > UINT64_MAX - (uint64_t)li ||
        prefix_len + path_len + host_header_len + host_len + type_header_len +
        content_type_len + length_header_len + (uint64_t)li >
        UINT64_MAX - suffix_len - 1) return -1;
    request_len = prefix_len + path_len + host_header_len + host_len +
                  type_header_len + content_type_len + length_header_len +
                  (uint64_t)li + suffix_len;
    request = (char *)kmalloc(request_len + 1);
    if (!request) return -1;

    sock = tcp_socket_create();
    if (!sock) { kfree(request); return -1; }

    if (tcp_connect(sock, ip, port, timeout_ms) < 0) {
        tcp_socket_close(sock);
        kfree(request);
        return -1;
    }

    req_len = 0;
    memcpy(request + req_len, prefix, prefix_len);
    req_len += prefix_len;
    memcpy(request + req_len, path, path_len);
    req_len += path_len;
    memcpy(request + req_len, host_header, host_header_len);
    req_len += host_header_len;
    memcpy(request + req_len, host, host_len);
    req_len += host_len;
    memcpy(request + req_len, type_header, type_header_len);
    req_len += type_header_len;
    memcpy(request + req_len, actual_content_type, content_type_len);
    req_len += content_type_len;
    memcpy(request + req_len, length_header, length_header_len);
    req_len += length_header_len;
    memcpy(request + req_len, len_str, (uint64_t)li);
    req_len += (uint64_t)li;
    memcpy(request + req_len, suffix, suffix_len);
    req_len += suffix_len;
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

    recv_buf = (uint8_t *)kmalloc(HTTP_RECV_BUF_INIT);
    if (!recv_buf) {
        tcp_disconnect(sock, 1000);
        tcp_socket_close(sock);
        return -1;
    }

    total_recv = 0;
    start = net_get_ticks();
    buf_cap = HTTP_RECV_BUF_INIT;
    expected_total = 0;
    receive_error = 0;

    for (;;) {
        if (task_has_pending_signals()) break;

        if (expected_total == 0 && total_recv > UINT64_MAX - 4096) {
            receive_error = 1;
            break;
        }
        if (expected_total == 0 && total_recv + 4096 > buf_cap) {
            if (buf_cap > UINT64_MAX / 2) {
                receive_error = 1;
                break;
            }
            new_cap = buf_cap * 2;
            new_buf = (uint8_t *)krealloc(recv_buf, new_cap);
            if (!new_buf) {
                receive_error = 1;
                break;
            }
            recv_buf = new_buf;
            buf_cap = new_cap;
        }
        n = tcp_recv(sock, recv_buf + total_recv, buf_cap - total_recv, 1000);
        if (n > 0) {
            total_recv += n;
            start = net_get_ticks();

            header_end_pos = 0;
            for (si = 0; si + 3 < total_recv; si++) {
                if (recv_buf[si] == '\r' && recv_buf[si+1] == '\n' &&
                    recv_buf[si+2] == '\r' && recv_buf[si+3] == '\n') {
                    header_end_pos = si + 4;
                    break;
                }
            }
            if (header_end_pos > 0) {
                tmp_resp = (http_response_t *)kmalloc(sizeof(http_response_t));
                if (tmp_resp) {
                    memset(tmp_resp, 0, sizeof(*tmp_resp));
                    if (http_parse_response(recv_buf, total_recv, tmp_resp) == 0 &&
                        tmp_resp->content_length > 0) {
                        if (tmp_resp->content_length > UINT64_MAX - header_end_pos) {
                            kfree(tmp_resp);
                            receive_error = 1;
                            break;
                        }
                        expected_total = header_end_pos + tmp_resp->content_length;
                        if (expected_total > buf_cap) {
                            new_buf = (uint8_t *)krealloc(recv_buf, expected_total);
                            if (new_buf) {
                                recv_buf = new_buf;
                                buf_cap = expected_total;
                            } else {
                                kfree(tmp_resp);
                                receive_error = 1;
                                break;
                            }
                        }
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

    if (receive_error) {
        kfree(recv_buf);
        return -1;
    }

    if (total_recv == 0) {
        kfree(recv_buf);
        return -1;
    }

    if (http_parse_response(recv_buf, total_recv, response) < 0) {
        kfree(recv_buf);
        return -1;
    }

    response->body = (uint8_t *)kmalloc(response->body_len + 1);
    if (!response->body) {
        memset(response, 0, sizeof(http_response_t));
        kfree(recv_buf);
        return -1;
    }
    memcpy(response->body, recv_buf + (total_recv - response->body_len), response->body_len);
    response->body[response->body_len] = '\0';

    if (response->raw_headers_len > 0) {
        hdr_copy = (uint8_t *)kmalloc(response->raw_headers_len);
        if (hdr_copy) {
            memcpy(hdr_copy, response->raw_headers, response->raw_headers_len);
        }
        if (!hdr_copy) {
            kfree(response->body);
            memset(response, 0, sizeof(http_response_t));
            kfree(recv_buf);
            return -1;
        }
        response->raw_headers = hdr_copy;
    } else {
        response->raw_headers = NULL;
    }

    kfree(recv_buf);
    return 0;
}

int http_post(const char *host, uint16_t port, const char *path,
              const char *content_type, const uint8_t *body, uint64_t body_len,
              http_response_t *response, uint64_t timeout_ms) {
    ipv4_addr_t ip;

    if (dns_resolve_timeout(host, &ip, timeout_ms) < 0) {
        return -1;
    }

    return http_post_ip(ip, port, host, path, content_type, body, body_len, response, timeout_ms);
}

int http_post_download(const char *url, const char *content_type,
                       const uint8_t *post_body, uint64_t post_body_len,
                       uint8_t *buffer, uint64_t buffer_size,
                       uint64_t *out_size, int *out_status) {
    uint8_t *body;
    uint64_t body_size;
    uint64_t copy_len;
    int result;

    body = NULL;
    body_size = 0;
    result = http_post_download_alloc(url, content_type, post_body,
                                      post_body_len, &body, &body_size,
                                      out_status);
    if (result != 0) return result;
    copy_len = body_size < buffer_size ? body_size : buffer_size;
    if (body && copy_len > 0) memcpy(buffer, body, copy_len);
    if (body) kfree(body);
    if (out_size) *out_size = copy_len;
    return 0;
}

int http_post_download_alloc(const char *url, const char *content_type,
                             const uint8_t *post_body,
                             uint64_t post_body_len,
                             uint8_t **out_body, uint64_t *out_size,
                             int *out_status) {
    char *host;
    char *path;
    uint16_t port;
    http_response_t *response;
    int attempt;
    int ok;

    if (!out_body) return -1;
    *out_body = NULL;
    if (out_size) *out_size = 0;
    host = NULL;
    path = NULL;
    response = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!response) { kfree(host); kfree(path); return -1; }

    if (http_parse_url(url, &host, &port, &path, NULL) < 0) {
        kfree(host); kfree(path); kfree(response);
        return -1;
    }

    ok = 0;
    for (attempt = 0; attempt < 2; attempt++) {
        if (http_post(host, port, path, content_type, post_body,
                      post_body_len, response, 15000) == 0) {
            ok = 1;
            break;
        }
        if (attempt + 1 < 2) sleep_ms(500);
    }
    if (!ok) {
        kfree(host); kfree(path); kfree(response);
        return -1;
    }

    if (out_status) *out_status = response->status_code;
    *out_body = response->body;
    if (out_size) *out_size = response->body_len;
    response->body = NULL;
    http_response_free(response);
    kfree(host); kfree(path); kfree(response);
    return 0;
}
