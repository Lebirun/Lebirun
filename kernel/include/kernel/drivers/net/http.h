#ifndef HTTP_H
#define HTTP_H

#include <kernel/drivers/net/net_types.h>

#define HTTP_STATUS_OK           200
#define HTTP_STATUS_MOVED        301
#define HTTP_STATUS_FOUND        302
#define HTTP_STATUS_NOT_FOUND    404
#define HTTP_STATUS_SERVER_ERROR 500

typedef struct {
    int status_code;
    uint32_t content_length;
    char content_type[64];
    uint8_t *body;
    uint32_t body_len;
} http_response_t;

int http_get(const char *host, uint16_t port, const char *path, http_response_t *response, uint32_t timeout_ms);
int http_get_ip(ipv4_addr_t ip, uint16_t port, const char *host, const char *path, http_response_t *response, uint32_t timeout_ms);
void http_response_free(http_response_t *response);
int http_download(const char *url, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);

#endif
