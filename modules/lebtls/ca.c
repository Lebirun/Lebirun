#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <string.h>
#include "tls_int.h"

uint8_t *lebtls_load_ca(uint64_t *len_out) {
    vfs_node_t *node;
    uint64_t len;
    uint64_t got;
    uint64_t n;
    uint8_t *buf;

    if (!len_out)
        return NULL;
    node = vfs_namei(LEBTLS_CA_PATH);
    if (!node || node->length == 0) {
        if (node)
            vfs_release(node);
        return NULL;
    }
    len = node->length;
    buf = kmalloc((size_t)len);
    if (!buf) {
        vfs_release(node);
        return NULL;
    }
    got = 0;
    while (got < len) {
        n = vfs_read(node, got, len - got, buf + got);
        if (n == 0)
            break;
        got += n;
    }
    vfs_release(node);
    if (got != len) {
        kfree(buf);
        return NULL;
    }
    *len_out = len;
    return buf;
}
