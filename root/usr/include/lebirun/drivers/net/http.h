#ifndef HTTP_H
#define HTTP_H

#include <lebirun/drivers/net/net_types.h>

#define HTTP_STATUS_OK           200
#define HTTP_STATUS_MOVED        301
#define HTTP_STATUS_FOUND        302
#define HTTP_STATUS_TEMP_REDIR   307
#define HTTP_STATUS_PERM_REDIR   308
#define HTTP_STATUS_NOT_FOUND    404
#define HTTP_STATUS_SERVER_ERROR 500

#define HTTP_MAX_REDIRECTS_DEFAULT 5

typedef struct {
    int status_code;
    uint64_t content_length;
    char content_type[64];
    char location[512];
    uint8_t *body;
    uint64_t body_len;
    uint8_t *raw_headers;
    uint64_t raw_headers_len;
} http_response_t;

int http_get(const char *host, uint16_t port, const char *path, http_response_t *response, uint64_t timeout_ms);
int http_get_tls(const char *host, uint16_t port, const char *path, http_response_t *response, uint64_t timeout_ms, int use_tls);
int http_get_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint64_t timeout_ms);
int http_get_ip_tls(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint64_t timeout_ms, int use_tls);
int http_post_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path,
                 const char *content_type, const uint8_t *body, uint64_t body_len,
                 http_response_t *response, uint64_t timeout_ms);
int http_post(const char *host, uint16_t port, const char *path,
              const char *content_type, const uint8_t *body, uint64_t body_len,
              http_response_t *response, uint64_t timeout_ms);
void http_response_free(http_response_t *response);
int http_download(const char *url, uint8_t *buffer, uint64_t buffer_size, uint64_t *out_size, int *out_status);
int http_download_ex(const char *url, uint8_t *buffer, uint64_t buffer_size,
                     uint64_t *out_size, int *out_status, int max_redirects,
                     uint8_t *headers_buf, uint64_t headers_buf_size, uint64_t *out_headers_len);
int http_download_alloc(const char *url, uint8_t **out_body, uint64_t *out_size,
                        int *out_status, int max_redirects,
                        uint8_t *headers_buf, uint64_t headers_buf_size, uint64_t *out_headers_len);
int http_post_download(const char *url, const char *content_type,
                       const uint8_t *post_body, uint64_t post_body_len,
                       uint8_t *buffer, uint64_t buffer_size,
                       uint64_t *out_size, int *out_status);

#endif
