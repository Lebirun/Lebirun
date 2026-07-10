#ifndef OVERLAYFS_H
#define OVERLAYFS_H

#include <stdint.h>
#include <stddef.h>
#include <lebirun/vfs.h>

#define OVERLAY_MAX_LAYERS   4
#define OVERLAY_WHITEOUT_PREFIX ".wh."

typedef struct overlay_layer {
    vfs_node_t *root;
    int writable;
} overlay_layer_t;

typedef struct overlay_context {
    overlay_layer_t lower[OVERLAY_MAX_LAYERS - 1];
    int lower_count;
    overlay_layer_t upper;
    vfs_node_t *merged_root;
} overlay_context_t;

typedef struct overlay_node {
    vfs_node_t vfs;
    vfs_node_t *lower_node;
    vfs_node_t *upper_node;
    overlay_context_t *ctx;
    uint64_t rd_last_index;
    uint64_t rd_upper_count;
    uint64_t rd_lower_index;
    uint64_t rd_visible_count;
    int rd_phase;
    int open_count;
    int parent_pinned;
    int refcount;
} overlay_node_t;

int overlayfs_init(void);
void overlayfs_vfs_register(void);
overlay_context_t *overlayfs_create(vfs_node_t *lower_root, vfs_node_t *upper_root);
vfs_node_t *overlayfs_mount(overlay_context_t *ctx, const char *mountpoint);
void overlay_flush_cache(void);
void overlay_cache_stats(uint64_t *nodes, uint64_t *capacity, uint64_t *bytes);
int overlay_same_file(vfs_node_t *first, vfs_node_t *second);

#endif
