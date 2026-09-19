#include "syscall_defs.h"
#include <lebirun/task.h>
#include <lebirun/pit.h>
#include <lebirun/drivers/net/tcp.h>
#include <lebirun/drivers/net/udp.h>
#include <lebirun/drivers/net/net.h>

#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    AF_UNIX
#define AF_INET     2
#define AF_INET6    10

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

#define IPPROTO_IP   0
#define IPPROTO_IPV6 41
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define IP_TTL        2
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define TCP_NODELAY   1
#define TCP_CORK      3
#define TCP_FASTOPEN  23
#define TCP_USER_TIMEOUT 18
#define IPV6_V6ONLY   26
#define SO_TIMESTAMP  29
#define MSG_ZEROCOPY  0x4000000

#define SOL_SOCKET   1

#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_PASSCRED     16
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30
#define SO_PEERCRED     17
#define SO_REUSEPORT    15

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000
#define MSG_WAITALL   0x100
#define MSG_TRUNC     0x20
#define MSG_CTRUNC    0x08

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2

#define SOCKET_INIT_COUNT 1
#define SOCKET_BUF_SIZE 4096
#define BACKLOG_INIT_SIZE 8
#define UNIX_PATH_MAX 108

typedef unsigned int socklen_t;
typedef long ssize_t;

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[UNIX_PATH_MAX];
};

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    int msg_iovlen;
    int msg_iov_padding;
    void *msg_control;
    socklen_t msg_controllen;
    int msg_control_padding;
    int msg_flags;
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct ucred {
    pid_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct linger {
    int l_onoff;
    int l_linger;
};

typedef enum {
    SOCKSTATE_CLOSED = 0,
    SOCKSTATE_BOUND,
    SOCKSTATE_LISTENING,
    SOCKSTATE_CONNECTING,
    SOCKSTATE_CONNECTED
} sock_state_t;

struct cmsghdr {
    socklen_t cmsg_len;
    int cmsg_padding;
    int cmsg_level;
    int cmsg_type;
};

typedef struct pending_conn {
    uint64_t remote_addr;
    uint16_t remote_port;
    int valid;
    int peer_idx;
} pending_conn_t;

typedef struct {
    int in_use;
    int domain;
    int type;
    int protocol;
    uint64_t local_addr;
    uint16_t local_port;
    pid_t owner_pid;
    uint64_t remote_addr;
    uint16_t remote_port;
    uint8_t *recv_buf;
    uint32_t recv_capacity;
    volatile uint32_t recv_lock;
    uint64_t recv_head;
    uint64_t recv_tail;
    int nonblocking;
    int descriptor_refs;
    int error;
    int shut_rd;
    int shut_wr;
    int so_reuseaddr;
    int so_reuseport;
    int so_keepalive;
    int so_broadcast;
    int so_sndbuf;
    int so_rcvbuf;
    int so_timestamp;
    int tcp_cork;
    int tcp_fastopen;
    int tcp_user_timeout;
    int msg_zerocopy;
    struct timeval so_rcvtimeo;
    struct timeval so_sndtimeo;
    struct linger so_linger;
    int so_passcred;
    int tcp_nodelay;
    int ip_ttl;
    int ipv6_v6only;
    int backlog_size;
    int backlog_count;
    int backlog_capacity;
    int peer_socket;
    pid_t peer_pid;
    uint32_t peer_uid;
    uint32_t peer_gid;
    int peer_write_closed;
    tcp_socket_t *tcp;
    udp_socket_t *udp;
    udp_mcast_t *ip_mcast;
    sock_state_t state;
    pending_conn_t *backlog;
    char *sun_path;
    task_fd_t *pending_fds;
    int pending_fd_count;
    int pending_fd_capacity;
} socket_t;

static socket_t *sockets = NULL;
static int socket_capacity = 0;
static uint16_t next_ephemeral_port = 49152;
static spinlock_t socket_table_lock;

static socket_t *get_socket(int fd);
static void free_socket(int idx, int graceful);
int is_socket_fd(int fd);
extern int task_has_pending_signals(void);

static int socket_descriptor_index(task_fd_t *descriptor) {
    uintptr_t encoded;

    if (!descriptor || !descriptor->in_use ||
        descriptor->type != FD_TYPE_SOCKET || !descriptor->private_data)
        return -1;
    encoded = (uintptr_t)descriptor->private_data;
    if (encoded == 0 || encoded - 1 > 0x7FFFFFFFUL) return -1;
    return (int)(encoded - 1);
}

static int socket_fd_index(task_t *task, int fd) {
    if (!task || !task->fds || fd < 0 || fd >= task->fds_capacity)
        return -1;
    return socket_descriptor_index(&task->fds[fd]);
}

static int socket_fd_alloc_from(int idx, int flags, int start) {
    task_fd_t *descriptor;
    int fd;
    int i;

    if (!current_task || start < 0 || idx < 0 || idx >= socket_capacity ||
        !sockets[idx].in_use)
        return -EMFILE;
    fd = -1;
    for (i = start; i < current_task->fds_capacity; i++) {
        if (!current_task->fds[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        if (task_fd_ensure_capacity(current_task,
                                    start >= current_task->fds_capacity ?
                                    start : current_task->fds_capacity) != 0)
            return -EMFILE;
        for (i = start; i < current_task->fds_capacity; i++) {
            if (!current_task->fds[i].in_use) {
                fd = i;
                break;
            }
        }
    }
    if (fd < 0) return -EMFILE;
    descriptor = &current_task->fds[fd];
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->in_use = 1;
    descriptor->ref_count = 1;
    descriptor->type = FD_TYPE_SOCKET;
    descriptor->flags = (uint64_t)flags;
    descriptor->private_data = (void *)(uintptr_t)(idx + 1);
    sockets[idx].descriptor_refs++;
    return fd;
}

static int socket_fd_alloc(int idx, int flags) {
    return socket_fd_alloc_from(idx, flags, 3);
}

static void socket_release_index(int idx, int graceful) {
    if (idx < 0 || idx >= socket_capacity || !sockets[idx].in_use) return;
    if (sockets[idx].descriptor_refs > 0) sockets[idx].descriptor_refs--;
    if (sockets[idx].descriptor_refs == 0) free_socket(idx, graceful);
}

static int socket_grow(void) {
    socket_t *new_arr;
    int new_cap;

    new_arr = krealloc_grow_array(sockets, socket_capacity, &new_cap,
                                  SOCKET_INIT_COUNT, sizeof(*new_arr));
    if (!new_arr) return -1;
    sockets = new_arr;
    socket_capacity = new_cap;
    return 0;
}

static int alloc_socket(void) {
    int i;

    for (i = 0; i < socket_capacity; i++) {
        if (!sockets[i].in_use) goto found;
    }
    if (socket_grow() < 0) return -1;
    i = socket_capacity / 2;
found:
    memset(&sockets[i], 0, sizeof(socket_t));
    sockets[i].in_use = 1;
    sockets[i].owner_pid = current_task ? current_task->pid : 0;
    sockets[i].so_sndbuf = SOCKET_BUF_SIZE;
    sockets[i].so_rcvbuf = SOCKET_BUF_SIZE;
    sockets[i].ip_ttl = 64;
    sockets[i].ipv6_v6only = 0;
    sockets[i].peer_socket = -1;
    return i;
}

static void socket_release_pending_fd(task_fd_t *fd) {
    pipe_t *pipe;
    int endpoint;
    int socket_idx;

    if (!fd || !fd->in_use) return;
    if (fd->type == FD_TYPE_FILE && fd->node) {
        vfs_close((vfs_node_t *)fd->node);
    } else if (FD_TYPE_IS_PTY(fd->type) && fd->private_data) {
        endpoint = (int)(uintptr_t)fd->private_data;
        if (fd->type == FD_TYPE_PTY_MASTER)
            pty_close_master(endpoint);
        else
            pty_close_slave(endpoint);
    } else if (FD_TYPE_IS_PIPE(fd->type) && fd->private_data) {
        pipe = (pipe_t *)fd->private_data;
        if (pipe_release_reference(pipe, fd->type)) {
            pipe_destroy_if_unused(pipe);
        }
    } else if (fd->type == FD_TYPE_SOCKET && fd->private_data) {
        socket_idx = (int)((uintptr_t)fd->private_data - 1);
        socket_release_index(socket_idx, 0);
    }
    memset(fd, 0, sizeof(task_fd_t));
}

static int socket_ensure_pending_fd_capacity(socket_t *sock, int needed) {
    task_fd_t *new_fds;
    int new_capacity;

    if (!sock || needed < 0 ||
        (size_t)needed > SIZE_MAX / sizeof(task_fd_t)) return -ENOMEM;
    if (needed <= sock->pending_fd_capacity) return 0;
    new_capacity = needed;
    new_fds = (task_fd_t *)kmalloc((size_t)new_capacity * sizeof(task_fd_t));
    if (!new_fds) return -ENOMEM;
    memset(new_fds, 0, (size_t)new_capacity * sizeof(task_fd_t));
    if (sock->pending_fds && sock->pending_fd_count > 0) {
        memcpy(new_fds, sock->pending_fds,
               (size_t)sock->pending_fd_count * sizeof(task_fd_t));
    }
    kfree(sock->pending_fds);
    sock->pending_fds = new_fds;
    sock->pending_fd_capacity = new_capacity;
    return 0;
}

static int socket_send_rights(socket_t *sock, const struct msghdr *msg,
                              const struct cmsghdr *cmsg) {
    socket_t *peer;
    task_fd_t *src_tfd;
    pipe_t *passed_pipe;
    int *fd_arr;
    int nfds_to_pass;
    int i;
    int src_fd;
    size_t fd_bytes;
    const uint8_t *control_data;

    if (cmsg->cmsg_len < sizeof(struct cmsghdr) ||
        cmsg->cmsg_len > msg->msg_controllen)
        return -EINVAL;
    fd_bytes = cmsg->cmsg_len - sizeof(struct cmsghdr);
    if (fd_bytes % sizeof(int) != 0 ||
        fd_bytes / sizeof(int) > 0x7FFFFFFFUL)
        return -EINVAL;
    nfds_to_pass = (int)(fd_bytes / sizeof(int));
    if (sock->peer_socket < 0 || sock->peer_socket >= socket_capacity)
        return -ENOTCONN;
    peer = &sockets[sock->peer_socket];
    if (!peer->in_use) return -ENOTCONN;
    if (nfds_to_pass > 0x7FFFFFFF - peer->pending_fd_count) return -ENOMEM;
    if (nfds_to_pass == 0) return 0;
    fd_arr = (int *)kmalloc(fd_bytes);
    if (!fd_arr) return -ENOMEM;
    control_data = (const uint8_t *)msg->msg_control +
                   sizeof(struct cmsghdr);
    if (copy_from_user(fd_arr, control_data, fd_bytes) < 0) {
        kfree(fd_arr);
        return -EFAULT;
    }
    for (i = 0; i < nfds_to_pass; i++) {
        src_fd = fd_arr[i];
        if (!current_task || src_fd < 0 ||
            src_fd >= current_task->fds_capacity ||
            !current_task->fds[src_fd].in_use) {
            kfree(fd_arr);
            return -EBADF;
        }
    }
    if (socket_ensure_pending_fd_capacity(
            peer, peer->pending_fd_count + nfds_to_pass) < 0) {
        kfree(fd_arr);
        return -ENOMEM;
    }
    for (i = 0; i < nfds_to_pass; i++) {
        src_fd = fd_arr[i];
        src_tfd = &current_task->fds[src_fd];
        memcpy(&peer->pending_fds[peer->pending_fd_count], src_tfd,
               sizeof(task_fd_t));
        if (src_tfd->type == FD_TYPE_FILE && src_tfd->node) {
            vfs_open((vfs_node_t *)src_tfd->node, 0);
            task_fd_position_share(
                src_tfd, &peer->pending_fds[peer->pending_fd_count]);
        }
        if (FD_TYPE_IS_PTY(src_tfd->type) && src_tfd->private_data) {
            pty_retain_endpoint((int)(uintptr_t)src_tfd->private_data);
        }
        if (src_tfd->private_data && FD_TYPE_IS_PIPE(src_tfd->type)) {
            passed_pipe = (pipe_t *)src_tfd->private_data;
            pipe_retain_reference(passed_pipe, src_tfd->type);
        }
        if (src_tfd->type == FD_TYPE_SOCKET && src_tfd->private_data) {
            src_fd = (int)((uintptr_t)src_tfd->private_data - 1);
            if (src_fd >= 0 && src_fd < socket_capacity &&
                sockets[src_fd].in_use)
                sockets[src_fd].descriptor_refs++;
        }
        peer->pending_fd_count++;
    }
    kfree(fd_arr);
    return 0;
}

static void free_socket(int idx, int graceful) {
    int i;
    int peer_idx;
    socket_t *sock;
    socket_t *peer;

    if (idx < 0 || idx >= socket_capacity || !sockets[idx].in_use) return;
    sock = &sockets[idx];
    for (i = 0; i < sock->pending_fd_count; i++)
        socket_release_pending_fd(&sock->pending_fds[i]);
    sock->pending_fd_count = 0;
    if (sock->domain == AF_UNIX) {
        for (i = 0; i < sock->backlog_capacity; i++) {
            if (!sock->backlog[i].valid) continue;
            peer_idx = sock->backlog[i].peer_idx;
            if (peer_idx != idx) free_socket(peer_idx, 0);
        }
    }
    if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        peer = &sockets[sock->peer_socket];
        if (peer->in_use && peer->peer_socket == idx) {
            peer->peer_socket = -1;
        }
    }
    if (sock->tcp) {
        if (graceful) tcp_disconnect(sock->tcp, 1000);
        tcp_socket_close(sock->tcp);
    }
    if (sock->state == SOCKSTATE_LISTENING && sock->domain == AF_INET &&
        sock->type == SOCK_STREAM)
        tcp_unlisten(sock->local_port);
    if (sock->udp) udp_socket_close(sock->udp);
    while (sock->ip_mcast) {
        udp_mcast_t *dead = sock->ip_mcast;
        sock->ip_mcast = dead->next;
        kfree(dead);
    }
    kfree(sock->recv_buf);
    kfree(sock->backlog);
    kfree(sock->sun_path);
    kfree(sock->pending_fds);
    memset(sock, 0, sizeof(*sock));
}

static void socket_reclaim_storage(void) {
    int new_capacity;
    socket_t *new_sockets;

    new_capacity = socket_capacity;
    while (new_capacity > 0 && !sockets[new_capacity - 1].in_use)
        new_capacity--;
    if (new_capacity == socket_capacity) return;
    if (new_capacity == 0) {
        kfree(sockets);
        sockets = NULL;
        socket_capacity = 0;
        return;
    }
    new_sockets = (socket_t *)krealloc(
        sockets, (size_t)new_capacity * sizeof(socket_t));
    if (!new_sockets) return;
    sockets = new_sockets;
    socket_capacity = new_capacity;
}

void socket_close_task(task_t *task) {
    int i;
    int idx;

    if (!task || !task->fds) return;
    spin_lock(&socket_table_lock);
    for (i = 0; i < task->fds_capacity; i++) {
        if (!task->fds[i].in_use || task->fds[i].type != FD_TYPE_SOCKET)
            continue;
        idx = socket_fd_index(task, i);
        memset(&task->fds[i], 0, sizeof(task_fd_t));
        socket_release_index(idx, 0);
    }
    socket_reclaim_storage();
    spin_unlock(&socket_table_lock);
    descriptor_ready_notify();
}

void socket_close_cloexec(task_t *task) {
    int i;
    int idx;

    if (!task || !task->fds) return;
    spin_lock(&socket_table_lock);
    for (i = 3; i < task->fds_capacity; i++) {
        if (!task->fds[i].in_use || task->fds[i].type != FD_TYPE_SOCKET ||
            !(task->fds[i].flags & 1))
            continue;
        idx = socket_fd_index(task, i);
        memset(&task->fds[i], 0, sizeof(task_fd_t));
        socket_release_index(idx, 0);
    }
    socket_reclaim_storage();
    spin_unlock(&socket_table_lock);
    descriptor_ready_notify();
}

static ipv4_addr_t socket_ipv4_from_addr(uint32_t addr)
{
    return u32_to_ipv4(ntohl(addr));
}

static uint32_t socket_addr_from_ipv4(ipv4_addr_t ip)
{
    return htonl(ipv4_to_u32(ip));
}

static socket_t *get_socket(int fd) {
    int idx;

    idx = socket_fd_index(current_task, fd);
    if (idx < 0 || idx >= socket_capacity) return NULL;
    if (!sockets[idx].in_use) return NULL;
    return &sockets[idx];
}

void socket_retain_task_fd(task_fd_t *descriptor) {
    int idx;

    spin_lock(&socket_table_lock);
    idx = socket_descriptor_index(descriptor);
    if (idx >= 0 && idx < socket_capacity && sockets[idx].in_use)
        sockets[idx].descriptor_refs++;
    spin_unlock(&socket_table_lock);
}

void socket_release_task_fd(task_fd_t *descriptor) {
    int idx;

    if (!descriptor) return;
    spin_lock(&socket_table_lock);
    idx = socket_descriptor_index(descriptor);
    memset(descriptor, 0, sizeof(*descriptor));
    socket_release_index(idx, 1);
    socket_reclaim_storage();
    spin_unlock(&socket_table_lock);
    descriptor_ready_notify();
}

static uint16_t alloc_ephemeral_port(void) {
    uint16_t port;

    port = next_ephemeral_port++;
    if (port == UINT16_MAX) next_ephemeral_port = 49152;
    return port;
}

static size_t recv_buf_used(socket_t *sock) {
    return sock->recv_tail - sock->recv_head;
}

static void socket_recv_lock(socket_t *sock) {
    while (__sync_lock_test_and_set(&sock->recv_lock, 1)) {
        while (sock->recv_lock) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

static void socket_recv_unlock(socket_t *sock) {
    __sync_lock_release(&sock->recv_lock);
}

static size_t recv_buf_free(socket_t *sock) {
    size_t used;

    used = recv_buf_used(sock);
    if (used >= (size_t)sock->so_rcvbuf) return 0;
    return (size_t)sock->so_rcvbuf - used;
}

static int socket_reserve_buffer(uint8_t **buffer, uint32_t *capacity,
                                 uint64_t *head, uint64_t *tail,
                                 size_t additional, uint32_t limit) {
    uint64_t used;
    uint64_t required;
    uint32_t new_capacity;
    uint8_t *new_buffer;
    uint64_t i;

    used = *tail - *head;
    if (used > limit) return -ENOMEM;
    if (additional > limit - used) return -ENOMEM;
    required = used + additional;
    if (required <= *capacity) return 0;
    new_capacity = (uint32_t)required;
    new_buffer = (uint8_t *)kmalloc(new_capacity);
    if (!new_buffer) return -ENOMEM;
    for (i = 0; i < used; i++) {
        new_buffer[i] = (*buffer)[(*head + i) % *capacity];
    }
    kfree(*buffer);
    *buffer = new_buffer;
    *capacity = new_capacity;
    *head = 0;
    *tail = used;
    return 0;
}

static int socket_ensure_recv_buf(socket_t *sock, size_t additional) {
    return socket_reserve_buffer(&sock->recv_buf, &sock->recv_capacity,
                                 &sock->recv_head, &sock->recv_tail,
                                 additional, (uint32_t)sock->so_rcvbuf);
}

static void socket_release_empty_recv_buffer(socket_t *sock) {
    uint64_t used;

    if (!sock) return;
    used = sock->recv_tail - sock->recv_head;
    if (used != 0) return;
    kfree(sock->recv_buf);
    sock->recv_buf = NULL;
    sock->recv_capacity = 0;
    sock->recv_head = 0;
    sock->recv_tail = 0;
}

static int recv_buf_write(socket_t *sock, const void *data, size_t len) {
    size_t free;
    size_t to_write;
    const uint8_t *src;
    size_t i;

    socket_recv_lock(sock);
    free = recv_buf_free(sock);
    to_write = (len < free) ? len : free;
    if (to_write > 0 && socket_ensure_recv_buf(sock, to_write) < 0) {
        socket_recv_unlock(sock);
        return -ENOMEM;
    }
    src = (const uint8_t *)data;
    for (i = 0; i < to_write; i++) {
        sock->recv_buf[sock->recv_tail % sock->recv_capacity] = src[i];
        sock->recv_tail++;
    }
    socket_recv_unlock(sock);
    return (int)to_write;
}

static uint64_t socket_rcv_timeout_ms(socket_t *sock, int nonblocking,
                                          int flags, uint64_t default_ms) {
    uint64_t timeout_ms;

    if (nonblocking || (flags & MSG_DONTWAIT)) return 0;
    if (sock->so_rcvtimeo.tv_sec || sock->so_rcvtimeo.tv_usec) {
        timeout_ms = (uint64_t)sock->so_rcvtimeo.tv_sec * 1000 +
                     (uint64_t)sock->so_rcvtimeo.tv_usec / 1000;
        return timeout_ms;
    }
    return default_ms;
}

static uint64_t socket_timeval_to_ticks(const struct timeval *tv) {
    uint64_t ms;

    if (!tv->tv_sec && !tv->tv_usec) return UINT64_MAX;
    if (tv->tv_sec > (long)(INT64_MAX / 1000)) return (uint64_t)INT64_MAX;
    ms = (uint64_t)tv->tv_sec * 1000 + (uint64_t)tv->tv_usec / 1000;
    if (ms > (uint64_t)INT64_MAX) return (uint64_t)INT64_MAX;
    return pit_ms_to_ticks(ms);
}

static int socket_wait_for_data(socket_t **sock_ptr, int fd, int flags) {
    socket_t *sock;
    size_t used;
    uint64_t ready_generation;
    uint64_t wait_ticks;
    uint64_t deadline;
    int timed;
    int have_deadline;

    have_deadline = 0;
    deadline = 0;
    for (;;) {
        ready_generation = descriptor_ready_generation();
        spin_lock(&socket_table_lock);
        sock = get_socket(fd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        socket_recv_lock(sock);
        used = recv_buf_used(sock);
        socket_recv_unlock(sock);
        if (used != 0) {
            *sock_ptr = sock;
            return 1;
        }
        if (sock->peer_socket < 0 ||
            sock->peer_write_closed ||
            sock->shut_rd) {
            spin_unlock(&socket_table_lock);
            return 0;
        }
        if (sock->nonblocking || (flags & MSG_DONTWAIT)) {
            spin_unlock(&socket_table_lock);
            return -EAGAIN;
        }
        timed = sock->so_rcvtimeo.tv_sec || sock->so_rcvtimeo.tv_usec;
        if (timed && !have_deadline) {
            wait_ticks = socket_timeval_to_ticks(&sock->so_rcvtimeo);
            deadline = pit_get_ticks() + wait_ticks;
            have_deadline = 1;
        }
        if (timed) {
            wait_ticks = deadline - pit_get_ticks();
            if ((int64_t)wait_ticks <= 0) {
                spin_unlock(&socket_table_lock);
                return -EAGAIN;
            }
        } else {
            wait_ticks = UINT64_MAX;
        }
        spin_unlock(&socket_table_lock);
        descriptor_ready_wait(ready_generation, wait_ticks);
        if (task_has_pending_signals()) return -EINTR;
    }
}

static size_t recv_buf_read(socket_t *sock, void *data, size_t len, int peek) {
    size_t used;
    size_t to_read;
    uint8_t *dst;
    uint64_t head;
    size_t i;

    socket_recv_lock(sock);
    used = recv_buf_used(sock);
    to_read = (len < used) ? len : used;
    if (to_read != 0 && (!sock->recv_buf || sock->recv_capacity == 0)) {
        socket_recv_unlock(sock);
        return 0;
    }
    dst = (uint8_t *)data;
    head = sock->recv_head;
    for (i = 0; i < to_read; i++) {
        dst[i] = sock->recv_buf[head % sock->recv_capacity];
        head++;
    }
    if (!peek) {
        sock->recv_head = head;
        socket_release_empty_recv_buffer(sock);
    }
    socket_recv_unlock(sock);
    return to_read;
}

static int sys_socket(int domain, const char *type_ptr, int protocol) {
    int type;
    int flags;
    int idx;
    int fd;
    int fd_flags;

    type = (int)(uintptr_t)type_ptr;
    flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    
    if (domain != AF_INET && domain != AF_UNIX && domain != AF_INET6 && domain != 17) {
        return -EAFNOSUPPORT;
    }
    if (domain == 17 && type != SOCK_RAW && type != SOCK_DGRAM) {
        return -ESOCKTNOSUPPORT;
    }
    
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW && type != SOCK_SEQPACKET) {
        return -ESOCKTNOSUPPORT;
    }
    
    spin_lock(&socket_table_lock);
    idx = alloc_socket();
    if (idx < 0) {
        spin_unlock(&socket_table_lock);
        return -EMFILE;
    }
    
    sockets[idx].domain = domain;
    sockets[idx].type = type;
    sockets[idx].protocol = protocol;
    sockets[idx].state = SOCKSTATE_CLOSED;
    sockets[idx].nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
    fd_flags = (flags & SOCK_CLOEXEC ? 1 : 0) |
               (flags & SOCK_NONBLOCK ? 0x800 : 0);
    fd = socket_fd_alloc(idx, fd_flags);
    if (fd < 0) {
        free_socket(idx, 0);
        socket_reclaim_storage();
        spin_unlock(&socket_table_lock);
        return fd;
    }
    spin_unlock(&socket_table_lock);
    return fd;
}

static int sys_socketpair(int domain, const char *type_ptr, int protocol,
                          int *sv) {
    int type;
    int flags;
    int sv_values[2];
    int idx1;
    int idx2;
    int fd_flags;

    (void)protocol;
    type = (int)(uintptr_t)type_ptr;
    flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (!sv) return -EFAULT;

    if (domain != AF_UNIX) {
        return -EAFNOSUPPORT;
    }
    
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) {
        return -ESOCKTNOSUPPORT;
    }
    
    spin_lock(&socket_table_lock);
    idx1 = alloc_socket();
    if (idx1 < 0) {
        spin_unlock(&socket_table_lock);
        return -EMFILE;
    }
    
    idx2 = alloc_socket();
    if (idx2 < 0) {
        free_socket(idx1, 0);
        socket_reclaim_storage();
        spin_unlock(&socket_table_lock);
        return -EMFILE;
    }
    
    sockets[idx1].domain = domain;
    sockets[idx1].type = type;
    sockets[idx1].state = SOCKSTATE_CONNECTED;
    sockets[idx1].nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
    sockets[idx1].peer_socket = idx2;
    sockets[idx1].peer_pid = current_task ? current_task->pid : 0;
    sockets[idx1].peer_uid = current_task ? current_task->euid : 0;
    sockets[idx1].peer_gid = current_task ? current_task->egid : 0;
    
    sockets[idx2].domain = domain;
    sockets[idx2].type = type;
    sockets[idx2].state = SOCKSTATE_CONNECTED;
    sockets[idx2].nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
    sockets[idx2].peer_socket = idx1;
    sockets[idx2].peer_pid = current_task ? current_task->pid : 0;
    sockets[idx2].peer_uid = current_task ? current_task->euid : 0;
    sockets[idx2].peer_gid = current_task ? current_task->egid : 0;
    
    fd_flags = (flags & SOCK_CLOEXEC ? 1 : 0) |
               (flags & SOCK_NONBLOCK ? 0x800 : 0);
    sv_values[0] = socket_fd_alloc(idx1, fd_flags);
    if (sv_values[0] < 0) {
        free_socket(idx1, 0);
        free_socket(idx2, 0);
        socket_reclaim_storage();
        spin_unlock(&socket_table_lock);
        return sv_values[0];
    }
    sv_values[1] = socket_fd_alloc(idx2, fd_flags);
    if (sv_values[1] < 0) {
        task_fd_free(current_task, sv_values[0]);
        socket_release_index(idx1, 0);
        free_socket(idx2, 0);
        socket_reclaim_storage();
        spin_unlock(&socket_table_lock);
        return sv_values[1];
    }
    if (copy_to_user(sv, sv_values, sizeof(sv_values)) < 0) {
        task_fd_free(current_task, sv_values[0]);
        task_fd_free(current_task, sv_values[1]);
        socket_release_index(idx1, 0);
        socket_release_index(idx2, 0);
        socket_reclaim_storage();
        spin_unlock(&socket_table_lock);
        return -EFAULT;
    }
    spin_unlock(&socket_table_lock);

    return 0;
}

static int find_unix_listener(const char *path) {
    int i;
    for (i = 0; i < socket_capacity; i++) {
        if (sockets[i].in_use && sockets[i].domain == AF_UNIX &&
            sockets[i].state == SOCKSTATE_LISTENING &&
            sockets[i].sun_path && strcmp(sockets[i].sun_path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int socket_set_sun_path(socket_t *sock, const char *path) {
    size_t length;
    char *new_path;

    length = 0;
    while (path[length] && length < UNIX_PATH_MAX - 1) length++;
    new_path = (char *)kmalloc(length + 1);
    if (!new_path) return -ENOMEM;
    memcpy(new_path, path, length);
    new_path[length] = '\0';
    kfree(sock->sun_path);
    sock->sun_path = new_path;
    return 0;
}

static int socket_set_sun_path_abs(socket_t *sock, const char *raw, size_t raw_len) {
    char *new_path;
    size_t n;
    if (raw_len <= 2) return -EINVAL;
    n = raw_len - 2;
    if (n >= UNIX_PATH_MAX) n = UNIX_PATH_MAX - 1;
    new_path = (char *)kmalloc(n + 2);
    if (!new_path) return -ENOMEM;
    new_path[0] = '@';
    memcpy(new_path + 1, raw + 1, n - 1);
    new_path[n] = '\0';
    kfree(sock->sun_path);
    sock->sun_path = new_path;
    return 0;
}



static int socket_create_path_node(const char *path) {
    char parent_path[UNIX_PATH_MAX];
    const char *name;
    const char *slash;
    size_t parent_length;
    vfs_node_t *existing;
    vfs_node_t *parent;
    int result;

    if (!path || path[0] != '/') return -EINVAL;
    existing = vfs_namei(path);
    if (existing) {
        vfs_release(existing);
        return -EADDRINUSE;
    }
    slash = strrchr(path, '/');
    if (!slash || !slash[1]) return -EINVAL;
    name = slash + 1;
    parent_length = (size_t)(slash - path);
    if (parent_length == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        if (parent_length >= sizeof(parent_path)) return -ENAMETOOLONG;
        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';
    }
    parent = vfs_namei(parent_path);
    if (!parent) return -ENOENT;
    result = vfs_mknod(parent, name, S_IFSOCK | 0777);
    if (result < 0)
        result = vfs_create(parent, name, VFS_SOCKET | 0777);
    vfs_release(parent);
    if (result < 0) return -EACCES;
    return 0;
}

static int socket_path_node_exists(const char *path) {
    vfs_node_t *node;
    int exists;

    node = vfs_namei(path);
    if (!node) return 0;
    exists = VFS_GET_TYPE(node->flags) == VFS_SOCKET;
    vfs_release(node);
    return exists;
}

static void socket_forget_unlinked_path(const char *path) {
    int i;

    if (socket_path_node_exists(path)) return;
    for (i = 0; i < socket_capacity; i++) {
        if (!sockets[i].in_use || !sockets[i].sun_path) continue;
        if (sockets[i].state != SOCKSTATE_BOUND &&
            sockets[i].state != SOCKSTATE_LISTENING) continue;
        if (strcmp(sockets[i].sun_path, path) != 0) continue;
        kfree(sockets[i].sun_path);
        sockets[i].sun_path = NULL;
    }
}

static int sys_bind(int sockfd, const char *addr_ptr, int addrlen) {
    struct sockaddr_in *addr;
    struct sockaddr_in6 *addr6;
    struct sockaddr_un *uaddr;
    socket_t *sock;
    int path_result;
    int result;

    result = 0;
    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        result = -EBADF;
        goto out;
    }
    
    if (sock->state != SOCKSTATE_CLOSED) {
        result = -EINVAL;
        goto out;
    }
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        if (!uaddr || addrlen < 3) {
            result = -EINVAL;
            goto out;
        }
        if (uaddr->sun_family != AF_UNIX) {
            result = -EAFNOSUPPORT;
            goto out;
        }
        if (addrlen >= 3 && uaddr->sun_path[0] == '\0') {
            size_t raw_len = (size_t)addrlen > UNIX_PATH_MAX + 2 ?
                             UNIX_PATH_MAX + 2 : (size_t)addrlen;
            if (socket_set_sun_path_abs(sock, (const char *)uaddr, raw_len) < 0) {
                result = -ENOMEM;
                goto out;
            }
            sock->state = SOCKSTATE_BOUND;
            goto out;
        }
        socket_forget_unlinked_path(uaddr->sun_path);
        if (socket_set_sun_path(sock, uaddr->sun_path) < 0) {
            result = -ENOMEM;
            goto out;
        }
        path_result = socket_create_path_node(sock->sun_path);
        if (path_result < 0) {
            kfree(sock->sun_path);
            sock->sun_path = NULL;
            result = path_result;
            goto out;
        }
        sock->state = SOCKSTATE_BOUND;
        goto out;
    }
    
    if (sock->domain == AF_INET6) {
        addr6 = (struct sockaddr_in6 *)(uintptr_t)addr_ptr;
        if (!addr6 || addrlen < (int)sizeof(struct sockaddr_in6)) {
            result = -EINVAL;
            goto out;
        }
        if (addr6->sin6_family != AF_INET6) {
            result = -EAFNOSUPPORT;
            goto out;
        }
        result = -EOPNOTSUPP;
        goto out;
    }

    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    if (!addr || addrlen < (int)sizeof(struct sockaddr_in)) {
        result = -EINVAL;
        goto out;
    }
    
    if (sock->domain == AF_INET && addr->sin_family != AF_INET) {
        result = -EAFNOSUPPORT;
        goto out;
    }
    
    sock->local_addr = addr->sin_addr.s_addr;
    sock->local_port = ntohs(addr->sin_port);
    
    if (sock->type == SOCK_DGRAM) {
        sock->udp = udp_socket_create(sock->local_port);
        if (!sock->udp) {
            result = -EADDRINUSE;
            goto out;
        }
        sock->local_port = sock->udp->local_port;
    } else if (sock->local_port == 0) {
        sock->local_port = alloc_ephemeral_port();
    }
    
    sock->state = SOCKSTATE_BOUND;
out:
    spin_unlock(&socket_table_lock);
    return result;
}

static int sys_connect(int sockfd, const char *addr_ptr, int addrlen) {
    struct sockaddr_in *addr;
    struct sockaddr_in6 *addr6;
    struct sockaddr_un *uaddr;
    int listener_idx;
    int peer_idx;
    int socket_idx;
    int i;
    socket_t *sock;
    task_t *listener_task;
    tcp_socket_t *tcp;
    uint16_t remote_port;
    pid_t listener_pid;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    socket_idx = socket_fd_index(current_task, sockfd);
    
    if (sock->state == SOCKSTATE_CONNECTED) {
        spin_unlock(&socket_table_lock);
        return -EISCONN;
    }
    
    if (sock->state == SOCKSTATE_LISTENING) {
        spin_unlock(&socket_table_lock);
        return -EINVAL;
    }
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        if (!uaddr || addrlen < 3) {
            spin_unlock(&socket_table_lock);
            return -EINVAL;
        }
        spin_unlock(&socket_table_lock);
        if (!socket_path_node_exists(uaddr->sun_path))
            return -ECONNREFUSED;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock || sock->state == SOCKSTATE_CONNECTED ||
            sock->state == SOCKSTATE_LISTENING) {
            spin_unlock(&socket_table_lock);
            return -ECONNREFUSED;
        }
        listener_idx = find_unix_listener(uaddr->sun_path);
        if (listener_idx < 0) {
            spin_unlock(&socket_table_lock);
            return -ECONNREFUSED;
        }
        
        if (!sockets[listener_idx].backlog) {
            int first = sockets[listener_idx].backlog_size;
            pending_conn_t *fb;

            if (first > BACKLOG_INIT_SIZE) first = BACKLOG_INIT_SIZE;
            if (first < 1) first = 1;
            fb = (pending_conn_t *)kmalloc((size_t)first * sizeof(pending_conn_t));
            if (!fb) {
                spin_unlock(&socket_table_lock);
                return -ECONNREFUSED;
            }
            memset(fb, 0, (size_t)first * sizeof(pending_conn_t));
            sockets[listener_idx].backlog = fb;
            sockets[listener_idx].backlog_size = first;
            sockets[listener_idx].backlog_capacity = first;
        }
        if (sockets[listener_idx].backlog_count >= sockets[listener_idx].backlog_size) {
            pending_conn_t *nb;
            size_t ncap = (size_t)sockets[listener_idx].backlog_size * 2 + 1;
            if (ncap > SIZE_MAX / sizeof(pending_conn_t)) {
                spin_unlock(&socket_table_lock);
                return -ECONNREFUSED;
            }
            nb = (pending_conn_t *)krealloc(sockets[listener_idx].backlog,
                                            ncap * sizeof(pending_conn_t));
            if (!nb) {
                spin_unlock(&socket_table_lock);
                return -ECONNREFUSED;
            }
            memset(nb + sockets[listener_idx].backlog_size, 0,
                   (ncap - (size_t)sockets[listener_idx].backlog_size) * sizeof(pending_conn_t));
            sockets[listener_idx].backlog = nb;
            sockets[listener_idx].backlog_size = (int)ncap;
            sockets[listener_idx].backlog_capacity = (int)ncap;
        }
        
        peer_idx = alloc_socket();
        if (peer_idx < 0) {
            spin_unlock(&socket_table_lock);
            return -ENOMEM;
        }
        sock = &sockets[socket_idx];
        
        sockets[peer_idx].domain = AF_UNIX;
        sockets[peer_idx].type = sockets[listener_idx].type;
        sockets[peer_idx].owner_pid = sockets[listener_idx].owner_pid;
        sockets[peer_idx].state = SOCKSTATE_CONNECTED;
        sockets[peer_idx].peer_socket = socket_idx;
        sockets[peer_idx].peer_pid = sock->owner_pid;
        sockets[peer_idx].peer_uid = current_task ? current_task->euid : 0;
        sockets[peer_idx].peer_gid = current_task ? current_task->egid : 0;
        if (socket_set_sun_path(&sockets[peer_idx], uaddr->sun_path) < 0) {
            free_socket(peer_idx, 0);
            socket_reclaim_storage();
            spin_unlock(&socket_table_lock);
            return -ENOMEM;
        }
        if (socket_set_sun_path(sock, uaddr->sun_path) < 0) {
            free_socket(peer_idx, 0);
            socket_reclaim_storage();
            spin_unlock(&socket_table_lock);
            return -ENOMEM;
        }
        
        for (i = 0; i < sockets[listener_idx].backlog_size; i++) {
            if (!sockets[listener_idx].backlog[i].valid) {
                sockets[listener_idx].backlog[i].valid = 1;
                sockets[listener_idx].backlog[i].peer_idx = peer_idx;
                sockets[listener_idx].backlog_count++;
                break;
            }
        }
        
        sock->peer_socket = peer_idx;
        listener_pid = sockets[listener_idx].owner_pid;
        sock->peer_pid = listener_pid;
        sock->state = SOCKSTATE_CONNECTED;
        spin_unlock(&socket_table_lock);
        listener_task = task_find(listener_pid);
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (sock && sock->peer_socket == peer_idx) {
            sock->peer_uid = listener_task ? listener_task->euid : 0;
            sock->peer_gid = listener_task ? listener_task->egid : 0;
        }
        spin_unlock(&socket_table_lock);
        descriptor_ready_notify();
        return 0;
    }
    if (sock->domain == AF_INET6) {
        uint8_t *b;
        addr6 = (struct sockaddr_in6 *)(uintptr_t)addr_ptr;
        if (!addr6 || addrlen < (int)sizeof(struct sockaddr_in6)) {
            spin_unlock(&socket_table_lock);
            return -EINVAL;
        }
        if (addr6->sin6_family != AF_INET6) {
            spin_unlock(&socket_table_lock);
            return -EAFNOSUPPORT;
        }
        b = addr6->sin6_addr.s6_addr;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 &&
            b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
            b[8] == 0 && b[9] == 0 && b[10] == 0xFF && b[11] == 0xFF &&
            !sock->ipv6_v6only) {
            uint32_t v4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                          ((uint32_t)b[14] << 8) | b[15];
            sock->remote_addr = v4;
            sock->remote_port = ntohs(addr6->sin6_port);
            if (sock->state == SOCKSTATE_CLOSED && sock->type != SOCK_DGRAM) {
                sock->local_port = alloc_ephemeral_port();
            }
            if (sock->type == SOCK_DGRAM) {
                if (!sock->udp) {
                    sock->udp = udp_socket_create(sock->local_port);
                    if (!sock->udp) {
                        spin_unlock(&socket_table_lock);
                        return -EADDRINUSE;
                    }
                    sock->local_port = sock->udp->local_port;
                }
                sock->state = SOCKSTATE_CONNECTED;
                spin_unlock(&socket_table_lock);
                return 0;
            }
            sock->tcp = tcp_socket_create();
            if (!sock->tcp) {
                spin_unlock(&socket_table_lock);
                return -ENOMEM;
            }
            sock->tcp->ttl = (uint8_t)sock->ip_ttl;
            sock->tcp->keepalive = (uint8_t)(sock->so_keepalive ? 1 : 0);
            sock->tcp->cork = (uint8_t)(sock->tcp_cork ? 1 : 0);
            sock->tcp->fastopen = (uint8_t)(sock->tcp_fastopen ? 1 : 0);
            sock->tcp->user_timeout = (uint64_t)sock->tcp_user_timeout;
            if (sock->nonblocking) {
                tcp_socket_t *tcpv6 = sock->tcp;
                uint16_t rport = sock->remote_port;
                uint32_t rv4 = v4;
                spin_unlock(&socket_table_lock);
                if (tcp_connect_start(tcpv6,
                                      socket_ipv4_from_addr(rv4),
                                      rport) < 0) {
                    return -ECONNREFUSED;
                }
                sock->state = SOCKSTATE_CONNECTING;
                return -EINPROGRESS;
            }
            {
                tcp_socket_t *tcpv6 = sock->tcp;
                uint16_t rport = sock->remote_port;
                uint32_t rv4 = v4;
                spin_unlock(&socket_table_lock);
                if (tcp_connect(tcpv6, socket_ipv4_from_addr(rv4),
                                rport, 10000) < 0) {
                    return -ECONNREFUSED;
                }
                sock->state = SOCKSTATE_CONNECTED;
                return 0;
            }
        }
        spin_unlock(&socket_table_lock);
        return -EOPNOTSUPP;
    }

    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    if (!addr || addrlen < (int)sizeof(struct sockaddr_in)) {
        spin_unlock(&socket_table_lock);
        return -EINVAL;
    }
    
    if (sock->domain == AF_INET && addr->sin_family != AF_INET) {
        spin_unlock(&socket_table_lock);
        return -EAFNOSUPPORT;
    }
    
    sock->remote_addr = addr->sin_addr.s_addr;
    sock->remote_port = ntohs(addr->sin_port);
    
    if (sock->state == SOCKSTATE_CLOSED && sock->type != SOCK_DGRAM) {
        sock->local_port = alloc_ephemeral_port();
    }
    if (sock->type == SOCK_DGRAM) {
        if (!sock->udp) {
            sock->udp = udp_socket_create(sock->local_port);
            if (!sock->udp) {
                spin_unlock(&socket_table_lock);
                return -EADDRINUSE;
            }
            sock->local_port = sock->udp->local_port;
        }
        sock->state = SOCKSTATE_CONNECTED;
        spin_unlock(&socket_table_lock);
        return 0;
    }
    
    sock->tcp = tcp_socket_create();
    if (!sock->tcp) {
        spin_unlock(&socket_table_lock);
        return -ENOMEM;
    }
    sock->tcp->ttl = (uint8_t)sock->ip_ttl;
    sock->tcp->keepalive = (uint8_t)(sock->so_keepalive ? 1 : 0);
    sock->tcp->cork = (uint8_t)(sock->tcp_cork ? 1 : 0);
    sock->tcp->fastopen = (uint8_t)(sock->tcp_fastopen ? 1 : 0);
    sock->tcp->user_timeout = (uint64_t)sock->tcp_user_timeout;

    if (sock->nonblocking) {
        tcp = sock->tcp;
        remote_port = sock->remote_port;
        spin_unlock(&socket_table_lock);
        if (tcp_connect_start(tcp,
                              socket_ipv4_from_addr(addr->sin_addr.s_addr),
                              remote_port) < 0) {
            spin_lock(&socket_table_lock);
            sock = get_socket(sockfd);
            if (sock && sock->tcp == tcp) {
                sock->tcp = NULL;
                sock->state = SOCKSTATE_CLOSED;
            }
            spin_unlock(&socket_table_lock);
            tcp_socket_close(tcp);
            return -ECONNREFUSED;
        }
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock || sock->tcp != tcp) {
            spin_unlock(&socket_table_lock);
            tcp_socket_close(tcp);
            return -EBADF;
        }
        sock->local_port = tcp->local_port;
        sock->local_addr = socket_addr_from_ipv4(tcp->local_ip);
        sock->state = SOCKSTATE_CONNECTING;
        spin_unlock(&socket_table_lock);
        return -EINPROGRESS;
    }

    tcp = sock->tcp;
    remote_port = sock->remote_port;
    spin_unlock(&socket_table_lock);
    if (tcp_connect(tcp, socket_ipv4_from_addr(addr->sin_addr.s_addr),
                    remote_port, 60000) < 0) {
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (sock && sock->tcp == tcp) {
            sock->tcp = NULL;
            sock->state = SOCKSTATE_CLOSED;
        }
        spin_unlock(&socket_table_lock);
        tcp_socket_close(tcp);
        return -ECONNREFUSED;
    }

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock || sock->tcp != tcp) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    sock->local_port = tcp->local_port;
    sock->local_addr = socket_addr_from_ipv4(tcp->local_ip);
    sock->state = SOCKSTATE_CONNECTED;
    spin_unlock(&socket_table_lock);
    return 0;
}

static int sys_listen(int sockfd, const char *backlog_ptr, int unused) {
    int backlog;
    socket_t *sock;

    (void)unused;
    backlog = (int)(uintptr_t)backlog_ptr;
    if (backlog < 1) backlog = 1;
    if ((size_t)backlog > SIZE_MAX / sizeof(pending_conn_t)) return -ENOMEM;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    if (sock->state != SOCKSTATE_BOUND) {
        spin_unlock(&socket_table_lock);
        return -EINVAL;
    }
    if (sock->type != SOCK_STREAM && sock->type != SOCK_SEQPACKET) {
        spin_unlock(&socket_table_lock);
        return -EOPNOTSUPP;
    }
    kfree(sock->backlog);
    sock->backlog = NULL;
    sock->backlog_capacity = 0;

    sock->backlog_size = backlog;
    sock->backlog_count = 0;
    sock->state = SOCKSTATE_LISTENING;
    if (sock->domain == AF_INET && sock->type == SOCK_STREAM) {
        if (tcp_listen(sock->local_port, backlog) < 0) {
            sock->state = SOCKSTATE_BOUND;
            spin_unlock(&socket_table_lock);
            return -EADDRINUSE;
        }
    }
    spin_unlock(&socket_table_lock);
    descriptor_ready_notify();
    return 0;
}

static int sys_accept(int sockfd, const char *addr_ptr,
                      uint64_t addrlen_ptr) {
    int idx;
    int i;
    int listener_idx;
    int conn_idx;
    int accepted_fd;
    int fd_flags;
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socklen_t *user_addrlen;
    pending_conn_t *conn;
    socket_t *sock;
    tcp_socket_t *accepted_tcp;
    uint64_t ready_generation;
    uint16_t remote_port;
    uint32_t remote_addr;

    listener_idx = -1;
    accepted_tcp = NULL;
    for (;;) {
        ready_generation = descriptor_ready_generation();
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        listener_idx = socket_fd_index(current_task, sockfd);
        if (sock->state != SOCKSTATE_LISTENING) {
            spin_unlock(&socket_table_lock);
            return -EINVAL;
        }
        if (sock->domain == AF_INET && sock->type == SOCK_STREAM) {
            if (!accepted_tcp)
                accepted_tcp = tcp_accept(sock->local_port);
            if (accepted_tcp) break;
        } else if (sock->backlog_count != 0) {
            break;
        }
        if (sock->nonblocking) {
            spin_unlock(&socket_table_lock);
            return -EAGAIN;
        }
        if (sock->domain == AF_INET) {
            spin_unlock(&socket_table_lock);
            netif_poll_all();
        } else {
            spin_unlock(&socket_table_lock);
        }
        descriptor_ready_wait(ready_generation, UINT64_MAX);
        if (task_has_pending_signals()) return -EINTR;
    }

    if (sock->domain == AF_UNIX) {
        conn = NULL;
        for (i = 0; i < sock->backlog_size; i++) {
            if (sock->backlog[i].valid) {
                conn = &sock->backlog[i];
                break;
            }
        }
        if (!conn) {
            spin_unlock(&socket_table_lock);
            return -EAGAIN;
        }
        
        idx = conn->peer_idx;
        fd_flags = sockets[idx].nonblocking ? 0x800 : 0;
        accepted_fd = socket_fd_alloc(idx, fd_flags);
        if (accepted_fd < 0) {
            spin_unlock(&socket_table_lock);
            return accepted_fd;
        }
        conn->valid = 0;
        sock->backlog_count--;
        spin_unlock(&socket_table_lock);
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        user_addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
        if (uaddr && user_addrlen && *user_addrlen >= sizeof(struct sockaddr_un)) {
            uaddr->sun_family = AF_UNIX;
            memset(uaddr->sun_path, 0, UNIX_PATH_MAX);
            *user_addrlen = sizeof(uint16_t);
        }
        
        return accepted_fd;
    }
    
    if (sock->domain == AF_INET && sock->type == SOCK_STREAM) {
        if (!accepted_tcp) {
            spin_unlock(&socket_table_lock);
            return -EAGAIN;
        }
        idx = alloc_socket();
        if (idx < 0) {
            tcp_socket_close(accepted_tcp);
            spin_unlock(&socket_table_lock);
            return -EMFILE;
        }
        sock = &sockets[listener_idx];

        sockets[idx].domain = sock->domain;
        sockets[idx].type = sock->type;
        sockets[idx].protocol = sock->protocol;
        sockets[idx].state = SOCKSTATE_CONNECTED;
        sockets[idx].local_addr = sock->local_addr;
        sockets[idx].local_port = sock->local_port;
        sockets[idx].remote_addr =
            socket_addr_from_ipv4(accepted_tcp->remote_ip);
        sockets[idx].remote_port = accepted_tcp->remote_port;
        sockets[idx].nonblocking = sock->nonblocking;
        sockets[idx].tcp = accepted_tcp;
        accepted_tcp->ttl = (uint8_t)sock->ip_ttl;
        accepted_tcp->keepalive = sock->so_keepalive ? 1 : 0;
        fd_flags = sockets[idx].nonblocking ? 0x800 : 0;
        accepted_fd = socket_fd_alloc(idx, fd_flags);
        if (accepted_fd < 0) {
            sockets[idx].tcp = NULL;
            tcp_socket_close(accepted_tcp);
            free_socket(idx, 0);
            socket_reclaim_storage();
            spin_unlock(&socket_table_lock);
            return accepted_fd;
        }

        remote_port = sockets[idx].remote_port;
        remote_addr = sockets[idx].remote_addr;
        spin_unlock(&socket_table_lock);

        addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
        user_addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;

        if (addr && user_addrlen && *user_addrlen >= sizeof(struct sockaddr_in)) {
            addr->sin_family = AF_INET;
            addr->sin_port = htons(remote_port);
            addr->sin_addr.s_addr = remote_addr;
            *user_addrlen = sizeof(struct sockaddr_in);
        }

        return accepted_fd;
    }

    conn = NULL;
    conn_idx = -1;
    for (i = 0; i < sock->backlog_size; i++) {
        if (sock->backlog[i].valid) {
            conn = &sock->backlog[i];
            conn_idx = i;
            break;
        }
    }
    
    if (!conn) {
        spin_unlock(&socket_table_lock);
        return -EAGAIN;
    }
    
    idx = alloc_socket();
    if (idx < 0) {
        spin_unlock(&socket_table_lock);
        return -EMFILE;
    }
    sock = &sockets[listener_idx];
    conn = &sock->backlog[conn_idx];

    sockets[idx].domain = sock->domain;
    sockets[idx].type = sock->type;
    sockets[idx].protocol = sock->protocol;
    sockets[idx].state = SOCKSTATE_CONNECTED;
    sockets[idx].local_addr = sock->local_addr;
    sockets[idx].local_port = sock->local_port;
    sockets[idx].remote_addr = conn->remote_addr;
    sockets[idx].remote_port = conn->remote_port;
    sockets[idx].nonblocking = sock->nonblocking;
    fd_flags = sockets[idx].nonblocking ? 0x800 : 0;
    accepted_fd = socket_fd_alloc(idx, fd_flags);
    if (accepted_fd < 0) {
        free_socket(idx, 0);
        socket_reclaim_storage();
        spin_unlock(&socket_table_lock);
        return accepted_fd;
    }
    
    conn->valid = 0;
    sock->backlog_count--;
    remote_port = sockets[idx].remote_port;
    remote_addr = sockets[idx].remote_addr;
    spin_unlock(&socket_table_lock);

    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    user_addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
    if (addr && user_addrlen && *user_addrlen >= sizeof(struct sockaddr_in)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(remote_port);
        addr->sin_addr.s_addr = remote_addr;
        *user_addrlen = sizeof(struct sockaddr_in);
    }
    
    return accepted_fd;
}

static int sys_accept4(int sockfd, const char *addr_ptr,
                       uint64_t addrlen_ptr, int flags) {
    int fd;
    socket_t *sock;
    task_fd_t *descriptor;

    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    fd = sys_accept(sockfd, addr_ptr, addrlen_ptr);
    if (fd < 0) return fd;
    spin_lock(&socket_table_lock);
    sock = get_socket(fd);
    if (sock) {
        sock->nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
        descriptor = &current_task->fds[fd];
        if (flags & SOCK_NONBLOCK)
            descriptor->flags |= 0x800;
        else
            descriptor->flags &= ~0x800UL;
        if (flags & SOCK_CLOEXEC)
            descriptor->flags |= 1;
        else
            descriptor->flags &= ~1UL;
    }
    spin_unlock(&socket_table_lock);
    return fd;
}

static int sys_getsockopt(int sockfd, const char *level_ptr, int optname,
                          uint64_t optval_ptr, uint64_t optlen_ptr,
                          int unused) {
    int level;
    void *optval;
    socklen_t *optlen;
    int value;
    struct ucred credentials;
    struct timeval tv;
    struct linger linger;
    socket_t *sock;
    (void)unused;

    level = (int)(uintptr_t)level_ptr;
    optval = (void *)(uintptr_t)optval_ptr;
    optlen = (socklen_t *)(uintptr_t)optlen_ptr;
    if (!optval || !optlen) return -EINVAL;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    if (level == IPPROTO_TCP && optname == TCP_NODELAY) {
        value = sock->tcp_nodelay;
        spin_unlock(&socket_table_lock);
        if (*optlen < sizeof(int)) return -EINVAL;
        if (copy_to_user(optval, &value, sizeof(int)) < 0) return -EFAULT;
        *optlen = sizeof(int);
        return 0;
    }
    if (level == IPPROTO_IP && optname == IP_TTL) {
        value = sock->ip_ttl;
        spin_unlock(&socket_table_lock);
        if (*optlen < sizeof(int)) return -EINVAL;
        if (copy_to_user(optval, &value, sizeof(int)) < 0) return -EFAULT;
        *optlen = sizeof(int);
        return 0;
    }
    if (level == IPPROTO_IPV6 && optname == IPV6_V6ONLY) {
        value = sock->ipv6_v6only;
        spin_unlock(&socket_table_lock);
        if (*optlen < sizeof(int)) return -EINVAL;
        if (copy_to_user(optval, &value, sizeof(int)) < 0) return -EFAULT;
        *optlen = sizeof(int);
        return 0;
    }
    if (level != SOL_SOCKET) {
        spin_unlock(&socket_table_lock);
        return -ENOPROTOOPT;
    }
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
        tv = optname == SO_RCVTIMEO ? sock->so_rcvtimeo : sock->so_sndtimeo;
        spin_unlock(&socket_table_lock);
        if (*optlen < sizeof(tv)) return -EINVAL;
        if (copy_to_user(optval, &tv, sizeof(tv)) < 0) return -EFAULT;
        *optlen = sizeof(tv);
        return 0;
    }
    if (optname == SO_LINGER) {
        linger = sock->so_linger;
        spin_unlock(&socket_table_lock);
        if (*optlen < sizeof(linger)) return -EINVAL;
        if (copy_to_user(optval, &linger, sizeof(linger)) < 0)
            return -EFAULT;
        *optlen = sizeof(linger);
        return 0;
    }
    if (*optlen < sizeof(int)) {
        spin_unlock(&socket_table_lock);
        return -EINVAL;
    }
    if (optname == SO_PEERCRED) {
        if (*optlen < sizeof(credentials)) {
            spin_unlock(&socket_table_lock);
            return -EINVAL;
        }
        credentials.pid = sock->peer_pid;
        credentials.uid = sock->peer_uid;
        credentials.gid = sock->peer_gid;
        spin_unlock(&socket_table_lock);
        memcpy(optval, &credentials, sizeof(credentials));
        *optlen = sizeof(credentials);
        return 0;
    }

    value = 0;
    switch (optname) {
        case SO_TYPE:
            value = sock->type;
            break;
        case SO_ERROR:
            value = sock->error;
            sock->error = 0;
            break;
        case SO_REUSEADDR:
            value = sock->so_reuseaddr;
            break;
        case SO_REUSEPORT:
            value = sock->so_reuseport;
            break;
        case SO_KEEPALIVE:
            value = sock->so_keepalive;
            break;
        case SO_BROADCAST:
            value = sock->so_broadcast;
            break;
        case SO_SNDBUF:
            value = sock->so_sndbuf;
            break;
        case SO_RCVBUF:
            value = sock->so_rcvbuf;
            break;
        case SO_PASSCRED:
            value = sock->so_passcred;
            break;
        case SO_TIMESTAMP:
            value = sock->so_timestamp;
            break;
        default:
            spin_unlock(&socket_table_lock);
            return -ENOPROTOOPT;
    }

    spin_unlock(&socket_table_lock);
    *(int *)optval = value;
    *optlen = sizeof(int);
    return 0;
}

static int sys_setsockopt(int sockfd, const char *level_ptr, int optname,
                          uint64_t optval_ptr, int optlen, int unused) {
    int level;
    int value;
    void *optval;
    struct timeval tv;
    struct linger linger;
    socket_t *sock;
    (void)unused;

    level = (int)(uintptr_t)level_ptr;
    optval = (void *)(uintptr_t)optval_ptr;
    if (!optval) return -EINVAL;

    if (level == IPPROTO_TCP && (optname == TCP_NODELAY || optname == TCP_CORK ||
                                 optname == TCP_FASTOPEN || optname == TCP_USER_TIMEOUT)) {
        if (optlen < (int)sizeof(int)) return -EINVAL;
        if (copy_from_user(&value, optval, sizeof(value)) < 0)
            return -EFAULT;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        if (optname == TCP_NODELAY) sock->tcp_nodelay = value ? 1 : 0;
        else if (optname == TCP_CORK) {
            sock->tcp_cork = value ? 1 : 0;
            if (sock->tcp) sock->tcp->cork = value ? 1 : 0;
        } else if (optname == TCP_FASTOPEN) {
            sock->tcp_fastopen = value ? 1 : 0;
            if (sock->tcp) sock->tcp->fastopen = value ? 1 : 0;
        } else {
            if (value < 0) value = 0;
            sock->tcp_user_timeout = value;
            if (sock->tcp) sock->tcp->user_timeout = (uint64_t)value;
        }
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (level == IPPROTO_IP && (optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP)) {
        struct in_addr maddr;
        udp_mcast_t *mc;
        udp_mcast_t **pp;
        uint8_t *b;
        if (optlen < (int)sizeof(maddr)) return -EINVAL;
        if (copy_from_user(&maddr, optval, sizeof(maddr)) < 0) return -EFAULT;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock || (sock->domain != AF_INET && sock->domain != AF_INET6)) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        b = (uint8_t *)&maddr.s_addr;
        if (optname == IP_ADD_MEMBERSHIP) {
            for (mc = sock->ip_mcast; mc; mc = mc->next) {
                if (mc->group.octets[0] == b[0] &&
                    mc->group.octets[1] == b[1] &&
                    mc->group.octets[2] == b[2] &&
                    mc->group.octets[3] == b[3]) break;
            }
            if (!mc) {
                mc = (udp_mcast_t *)kmalloc(sizeof(udp_mcast_t));
                if (!mc) { spin_unlock(&socket_table_lock); return -ENOMEM; }
                mc->group.octets[0] = b[0];
                mc->group.octets[1] = b[1];
                mc->group.octets[2] = b[2];
                mc->group.octets[3] = b[3];
                mc->next = sock->ip_mcast;
                sock->ip_mcast = mc;
            }
        } else {
            pp = &sock->ip_mcast;
            while (*pp) {
                if ((*pp)->group.octets[0] == b[0] &&
                    (*pp)->group.octets[1] == b[1] &&
                    (*pp)->group.octets[2] == b[2] &&
                    (*pp)->group.octets[3] == b[3]) {
                    udp_mcast_t *dead = *pp;
                    *pp = dead->next;
                    kfree(dead);
                    break;
                }
                pp = &(*pp)->next;
            }
        }
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (level == IPPROTO_IP && optname == IP_TTL) {
        if (optlen < (int)sizeof(int)) return -EINVAL;
        if (copy_from_user(&value, optval, sizeof(value)) < 0)
            return -EFAULT;
        if (value < 1 || value > 255) return -EINVAL;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        sock->ip_ttl = value;
        if (sock->tcp) sock->tcp->ttl = (uint8_t)value;
        if (sock->udp) sock->udp->ttl = (uint8_t)value;
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (level == IPPROTO_IPV6 && optname == IPV6_V6ONLY) {
        if (optlen < (int)sizeof(int)) return -EINVAL;
        if (copy_from_user(&value, optval, sizeof(value)) < 0)
            return -EFAULT;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        sock->ipv6_v6only = value ? 1 : 0;
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
        if (optlen < (int)sizeof(tv)) return -EINVAL;
        if (copy_from_user(&tv, optval, sizeof(tv)) < 0) return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
            return -EINVAL;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        if (optname == SO_RCVTIMEO) sock->so_rcvtimeo = tv;
        else sock->so_sndtimeo = tv;
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (optname == SO_LINGER) {
        if (optlen < (int)sizeof(linger)) return -EINVAL;
        if (copy_from_user(&linger, optval, sizeof(linger)) < 0)
            return -EFAULT;
        if (linger.l_onoff < 0 || linger.l_linger < 0) return -EINVAL;
        spin_lock(&socket_table_lock);
        sock = get_socket(sockfd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        sock->so_linger = linger;
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (optlen < (int)sizeof(int)) return -EINVAL;
    if (copy_from_user(&value, optval, sizeof(value)) < 0) return -EFAULT;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    switch (optname) {
        case SO_REUSEADDR:
            sock->so_reuseaddr = value ? 1 : 0;
            break;
        case SO_REUSEPORT:
            sock->so_reuseport = value ? 1 : 0;
            break;
        case SO_KEEPALIVE:
            sock->so_keepalive = value ? 1 : 0;
            if (sock->tcp) sock->tcp->keepalive = value ? 1 : 0;
            break;
        case SO_BROADCAST:
            sock->so_broadcast = value ? 1 : 0;
            break;
        case SO_SNDBUF:
            if (value <= 0) {
                spin_unlock(&socket_table_lock);
                return -EINVAL;
            }
            sock->so_sndbuf = value;
            break;
        case SO_RCVBUF:
            if (value <= 0) {
                spin_unlock(&socket_table_lock);
                return -EINVAL;
            }
            socket_recv_lock(sock);
            sock->so_rcvbuf = value;
            socket_recv_unlock(sock);
            break;
        case SO_PASSCRED:
            sock->so_passcred = value ? 1 : 0;
            break;
        case SO_TIMESTAMP:
            sock->so_timestamp = value ? 1 : 0;
            break;
        default:
            spin_unlock(&socket_table_lock);
            return -ENOPROTOOPT;
    }

    spin_unlock(&socket_table_lock);
    return 0;
}

static int sys_getsockname(int sockfd, const char *addr_ptr,
                           uint64_t addrlen_ptr) {
    socket_t *sock;
    const socklen_t *ulen;
    socklen_t alen;
    int domain;
    uint16_t local_port;
    uint32_t local_addr;
    char kpath[UNIX_PATH_MAX];
    size_t pathlen;
    struct sockaddr_in kaddr;
    struct sockaddr_in6 kaddr6;
    struct sockaddr_un kunix;
    socklen_t need;
    size_t copy;

    if (!addr_ptr || !addrlen_ptr) return -EINVAL;
    ulen = (const socklen_t *)(uintptr_t)addrlen_ptr;
    if (copy_from_user(&alen, ulen, sizeof(alen)) != 0) return -EFAULT;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    domain = sock->domain;
    local_port = sock->local_port;
    local_addr = sock->local_addr;
    kpath[0] = '\0';
    pathlen = 0;
    if (domain == AF_UNIX) {
        const char *path = sock->sun_path ? sock->sun_path : "";
        size_t i = 0;
        while (i < sizeof(kpath) - 1 && path[i]) {
            kpath[i] = path[i];
            i++;
        }
        kpath[i] = '\0';
        pathlen = i;
    }
    spin_unlock(&socket_table_lock);

    if (domain == AF_UNIX) {
        if (alen < sizeof(uint16_t)) return -EINVAL;
        need = (socklen_t)(sizeof(uint16_t) + pathlen + 1);
        memset(&kunix, 0, sizeof(kunix));
        kunix.sun_family = AF_UNIX;
        memcpy(kunix.sun_path, kpath, pathlen + 1);
        copy = alen < need ? alen : need;
        if (copy_to_user((void *)(uintptr_t)addr_ptr, &kunix, copy) != 0)
            return -EFAULT;
        if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &need, sizeof(need)) != 0)
            return -EFAULT;
        return 0;
    }

    if (domain == AF_INET6) {
        if (alen < sizeof(kaddr6)) return -EINVAL;
        memset(&kaddr6, 0, sizeof(kaddr6));
        kaddr6.sin6_family = AF_INET6;
        if (copy_to_user((void *)(uintptr_t)addr_ptr, &kaddr6, sizeof(kaddr6)) != 0)
            return -EFAULT;
        need = sizeof(kaddr6);
        if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &need, sizeof(need)) != 0)
            return -EFAULT;
        return 0;
    }

    if (alen < sizeof(kaddr)) return -EINVAL;
    memset(&kaddr, 0, sizeof(kaddr));
    kaddr.sin_family = AF_INET;
    kaddr.sin_port = htons(local_port);
    kaddr.sin_addr.s_addr = local_addr;
    if (copy_to_user((void *)(uintptr_t)addr_ptr, &kaddr, sizeof(kaddr)) != 0)
        return -EFAULT;
    need = sizeof(kaddr);
    if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &need, sizeof(need)) != 0)
        return -EFAULT;
    return 0;
}

static int sys_getpeername(int sockfd, const char *addr_ptr,
                           uint64_t addrlen_ptr) {
    socket_t *sock;
    const socklen_t *ulen;
    socklen_t alen;
    int domain;
    int connected;
    uint16_t remote_port;
    uint32_t remote_addr;
    struct sockaddr_in kaddr;
    struct sockaddr_in6 kaddr6;
    struct sockaddr_un kunix;
    socklen_t need;

    if (!addr_ptr || !addrlen_ptr) return -EINVAL;
    ulen = (const socklen_t *)(uintptr_t)addrlen_ptr;
    if (copy_from_user(&alen, ulen, sizeof(alen)) != 0) return -EFAULT;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    connected = sock->state == SOCKSTATE_CONNECTED;
    domain = sock->domain;
    remote_port = sock->remote_port;
    remote_addr = sock->remote_addr;
    spin_unlock(&socket_table_lock);

    if (!connected) return -ENOTCONN;

    if (domain == AF_UNIX) {
        if (alen < sizeof(uint16_t)) return -EINVAL;
        memset(&kunix, 0, sizeof(kunix));
        kunix.sun_family = AF_UNIX;
        need = sizeof(uint16_t);
        if (copy_to_user((void *)(uintptr_t)addr_ptr, &kunix, need) != 0)
            return -EFAULT;
        if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &need, sizeof(need)) != 0)
            return -EFAULT;
        return 0;
    }

    if (domain == AF_INET6) {
        if (alen < sizeof(kaddr6)) return -EINVAL;
        memset(&kaddr6, 0, sizeof(kaddr6));
        kaddr6.sin6_family = AF_INET6;
        if (copy_to_user((void *)(uintptr_t)addr_ptr, &kaddr6, sizeof(kaddr6)) != 0)
            return -EFAULT;
        need = sizeof(kaddr6);
        if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &need, sizeof(need)) != 0)
            return -EFAULT;
        return 0;
    }

    if (alen < sizeof(kaddr)) return -EINVAL;
    memset(&kaddr, 0, sizeof(kaddr));
    kaddr.sin_family = AF_INET;
    kaddr.sin_port = htons(remote_port);
    kaddr.sin_addr.s_addr = remote_addr;
    if (copy_to_user((void *)(uintptr_t)addr_ptr, &kaddr, sizeof(kaddr)) != 0)
        return -EFAULT;
    need = sizeof(kaddr);
    if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &need, sizeof(need)) != 0)
        return -EFAULT;
    return 0;
}

static int sys_sendto(int sockfd, const char *buf_ptr, int len,
                      int flags, uint64_t dest_addr_ptr, int addrlen) {
    const void *buf;
    int ret;
    socket_t *sock;
    socket_t *peer;
    struct sockaddr_in destination;
    uint32_t destination_addr;
    uint16_t destination_port;
    int domain;
    int type;
    int state;
    uint32_t remote_addr;
    uint16_t remote_port;
    tcp_socket_t *tcp;
    udp_socket_t *udp;

    (void)flags;
    if (len < 0) return -EINVAL;
    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    if (sock->type == SOCK_STREAM && sock->state != SOCKSTATE_CONNECTED) {
        if (sock->state == SOCKSTATE_CONNECTING) {
            spin_unlock(&socket_table_lock);
            return -EAGAIN;
        }
        spin_unlock(&socket_table_lock);
        return -ENOTCONN;
    }
    if (sock->shut_wr) {
        spin_unlock(&socket_table_lock);
        return -EPIPE;
    }
    buf = (const void *)(uintptr_t)buf_ptr;
    if (sock->domain == AF_UNIX) {
        if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
            peer = &sockets[sock->peer_socket];
            if (peer->in_use) {
                ret = recv_buf_write(peer, buf, len);
                spin_unlock(&socket_table_lock);
                if (ret > 0) descriptor_ready_notify();
                return ret;
            }
        }
        spin_unlock(&socket_table_lock);
        return -EPIPE;
    }
    domain = sock->domain;
    type = sock->type;
    state = sock->state;
    remote_addr = sock->remote_addr;
    remote_port = sock->remote_port;
    if (domain == AF_INET && type == SOCK_DGRAM && !sock->udp) {
        sock->udp = udp_socket_create(sock->local_port);
        if (!sock->udp) {
            spin_unlock(&socket_table_lock);
            return -EADDRINUSE;
        }
        sock->local_port = sock->udp->local_port;
    }
    tcp = sock->tcp;
    udp = sock->udp;
    if (udp) udp->ttl = (uint8_t)sock->ip_ttl;
    spin_unlock(&socket_table_lock);

    if (domain == AF_INET && type == SOCK_DGRAM) {
        if (dest_addr_ptr) {
            if (addrlen < (int)sizeof(destination) ||
                copy_from_user(&destination,
                    (const void *)(uintptr_t)dest_addr_ptr,
                    sizeof(destination)) < 0)
                return -EFAULT;
            if (destination.sin_family != AF_INET) return -EAFNOSUPPORT;
            destination_addr = destination.sin_addr.s_addr;
            destination_port = ntohs(destination.sin_port);
        } else {
            if (state != SOCKSTATE_CONNECTED) return -EDESTADDRREQ;
            destination_addr = remote_addr;
            destination_port = remote_port;
        }
        ret = udp_socket_send(udp,
                              socket_ipv4_from_addr(destination_addr),
                              destination_port, (uint8_t *)buf,
                              (uint64_t)len);
        return ret < 0 ? -EIO : len;
    }

    if (domain == AF_INET && type == SOCK_STREAM && tcp) {
        ret = tcp_send(tcp, (uint8_t *)buf, (uint64_t)len);
        if (ret < 0) return -EIO;
        return ret;
    }
    
    return -EOPNOTSUPP;
}

static int sys_sendmsg(int sockfd, const char *msg_ptr, int flags) {
    struct msghdr msg;
    struct cmsghdr cmsg;
    struct iovec iov;
    ssize_t total;
    ssize_t sent;
    socket_t *sock;
    int iov_index;
    int rights_result;

    if (!is_socket_fd(sockfd)) return -EBADF;
    if (!msg_ptr || copy_from_user(&msg, msg_ptr, sizeof(msg)) < 0)
        return -EFAULT;
    if (msg.msg_iovlen < 0) return -EINVAL;
    if (msg.msg_iovlen != 0 && !msg.msg_iov) return -EFAULT;
    if (msg.msg_iovlen > 0 &&
        ((uint64_t)(uintptr_t)msg.msg_iov > UINT64_MAX -
         (uint64_t)msg.msg_iovlen * sizeof(struct iovec) ||
         !user_access_ok(msg.msg_iov,
                         (uint64_t)msg.msg_iovlen * sizeof(struct iovec),
                         UACCESS_READ)))
        return -EFAULT;

    (void)flags;

    if (msg.msg_control && msg.msg_controllen >= sizeof(struct cmsghdr)) {
        if (copy_from_user(&cmsg, msg.msg_control, sizeof(cmsg)) < 0)
            return -EFAULT;
        if (cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_RIGHTS) {
            spin_lock(&socket_table_lock);
            sock = get_socket(sockfd);
            if (!sock) {
                spin_unlock(&socket_table_lock);
                return -EBADF;
            }
            rights_result = socket_send_rights(sock, &msg, &cmsg);
            spin_unlock(&socket_table_lock);
            if (rights_result < 0) return rights_result;
        }
    }

    total = 0;
    for (iov_index = 0; iov_index < msg.msg_iovlen; iov_index++) {
        if (copy_from_user(&iov,
                (const void *)(uintptr_t)((uint64_t)(uintptr_t)msg.msg_iov +
                (uint64_t)iov_index * sizeof(struct iovec)), sizeof(iov)) < 0)
            return -EFAULT;
        if (iov.iov_len > 0x7FFFFFFFUL) return -EINVAL;
        if (!user_access_ok(iov.iov_base, iov.iov_len, UACCESS_READ))
            return -EFAULT;
        sent = sys_sendto(sockfd, (const char *)(uintptr_t)iov.iov_base,
                          (int)iov.iov_len, flags, 0, 0);
        if (sent < 0) return sent;
        total += sent;
    }

    return total;
}

static int sys_recvfrom(int sockfd, const char *buf_ptr, int len,
                        int flags, uint64_t src_addr_ptr,
                        uint64_t addrlen_ptr) {
    void *buf;
    uint64_t timeout_ms;
    uint64_t full_dgram;
    int ret;
    int total;
    int wait_result;
    socket_t *sock;
    ipv4_addr_t source_ip;
    uint16_t source_port;
    struct sockaddr_in source;
    socklen_t source_length;
    int domain;
    int type;
    int nonblocking;
    int shut_rd;
    long timeout_sec;
    long timeout_usec;
    tcp_socket_t *tcp;
    udp_socket_t *udp;

    (void)src_addr_ptr;
    (void)addrlen_ptr;
    if (len < 0) return -EINVAL;
    
    buf = (void *)(uintptr_t)buf_ptr;
    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    domain = sock->domain;
    type = sock->type;
    nonblocking = sock->nonblocking;
    shut_rd = sock->shut_rd;
    timeout_sec = sock->so_rcvtimeo.tv_sec;
    timeout_usec = sock->so_rcvtimeo.tv_usec;
    if (domain == AF_INET && type == SOCK_DGRAM && !sock->udp) {
        sock->udp = udp_socket_create(sock->local_port);
        if (!sock->udp) {
            spin_unlock(&socket_table_lock);
            return -EADDRINUSE;
        }
        sock->local_port = sock->udp->local_port;
    }
    tcp = sock->tcp;
    udp = sock->udp;
    spin_unlock(&socket_table_lock);

    if (domain == AF_INET && type == SOCK_DGRAM) {
        if (shut_rd) return 0;
        if (nonblocking || (flags & MSG_DONTWAIT))
            timeout_ms = 0;
        else if (timeout_sec || timeout_usec)
            timeout_ms = (uint64_t)timeout_sec * 1000 +
                         (uint64_t)timeout_usec / 1000;
        else
            timeout_ms = UINT64_MAX;
        ret = udp_socket_recv(udp, (uint8_t *)buf, (uint64_t)len,
                              &source_ip, &source_port, timeout_ms,
                              (flags & MSG_PEEK) ? 1 : 0, &full_dgram);
        if (ret < 0)
            return timeout_ms == 0 ? -EAGAIN : -ETIMEDOUT;
        if ((flags & MSG_TRUNC) && full_dgram > (uint64_t)ret)
            ret = (int)full_dgram;
        if (src_addr_ptr && addrlen_ptr) {
            if (copy_from_user(&source_length,
                    (const void *)(uintptr_t)addrlen_ptr,
                    sizeof(source_length)) < 0)
                return -EFAULT;
            if (source_length >= sizeof(source)) {
                memset(&source, 0, sizeof(source));
                source.sin_family = AF_INET;
                source.sin_port = htons(source_port);
                source.sin_addr.s_addr = socket_addr_from_ipv4(source_ip);
                if (copy_to_user((void *)(uintptr_t)src_addr_ptr, &source,
                                 sizeof(source)) < 0)
                    return -EFAULT;
            }
            source_length = sizeof(source);
            if (copy_to_user((void *)(uintptr_t)addrlen_ptr,
                             &source_length, sizeof(source_length)) < 0)
                return -EFAULT;
        }
        return ret;
    }

    if (domain == AF_INET && type == SOCK_STREAM && tcp) {
        if (shut_rd) return 0;
        timeout_ms = socket_rcv_timeout_ms(sock, nonblocking, flags, 15000);
        if ((flags & MSG_WAITALL) && !nonblocking && !(flags & MSG_DONTWAIT) &&
            !(flags & MSG_PEEK)) {
            total = 0;
            while ((uint64_t)total < (uint64_t)len) {
                ret = tcp_recv(tcp, (uint8_t *)buf + total,
                               (uint64_t)len - (uint64_t)total, timeout_ms, 0);
                if (ret < 0) return total > 0 ? total : -EIO;
                if (ret == 0) {
                    if (tcp->state != TCP_STATE_CLOSE_WAIT &&
                        tcp->state != TCP_STATE_CLOSED)
                        return total > 0 ? total : -EAGAIN;
                    return total;
                }
                total += ret;
            }
            return total;
        }
        ret = tcp_recv(tcp, (uint8_t *)buf, (uint64_t)len, timeout_ms,
                       (flags & MSG_PEEK) ? 1 : 0);
        if (ret < 0) return -EIO;
        if (ret == 0) {
            if (nonblocking || (flags & MSG_DONTWAIT)) return -EAGAIN;
            if (tcp->state != TCP_STATE_CLOSE_WAIT &&
                tcp->state != TCP_STATE_CLOSED)
                return -EAGAIN;
        }
        return ret;
    }
    
    wait_result = socket_wait_for_data(&sock, sockfd, flags);
    if (wait_result <= 0) return wait_result;
    if ((flags & MSG_WAITALL) && !(flags & MSG_DONTWAIT) &&
        sock->type == SOCK_STREAM) {
        total = 0;
        for (;;) {
            ret = (int)recv_buf_read(sock, (uint8_t *)buf + total,
                                     (size_t)(len - total),
                                     flags & MSG_PEEK);
            if (ret > 0) {
                if (flags & MSG_PEEK) {
                    spin_unlock(&socket_table_lock);
                    return ret;
                }
                total += ret;
                if (total >= len) break;
            } else if (total > 0) {
                break;
            }
            spin_unlock(&socket_table_lock);
            wait_result = socket_wait_for_data(&sock, sockfd, flags);
            if (wait_result <= 0) return total > 0 ? total : wait_result;
        }
        spin_unlock(&socket_table_lock);
        if (total > 0) descriptor_ready_notify();
        return total;
    }
    ret = (int)recv_buf_read(sock, buf, len, flags & MSG_PEEK);
    spin_unlock(&socket_table_lock);
    if (ret > 0 && !(flags & MSG_PEEK)) descriptor_ready_notify();
    return ret;
}

static int sys_recvmsg(int sockfd, const char *msg_ptr, int flags) {
    struct msghdr msg;
    struct cmsghdr cmsg;
    struct iovec iov;
    ssize_t total;
    ssize_t recvd;
    socket_t *sock;
    struct ucred credentials;
    int is_dgram;
    socklen_t user_controllen;
    socklen_t cred_len;
    int nfds;
    socklen_t needed;
    int *out_fds;
    int i;
    int newfd;
    int iov_index;
    uint8_t *control_data;
    size_t needed_size;

    if (!is_socket_fd(sockfd)) return -EBADF;
    if (!msg_ptr || copy_from_user(&msg, msg_ptr, sizeof(msg)) < 0)
        return -EFAULT;
    if (msg.msg_iovlen < 0) return -EINVAL;
    if (msg.msg_iovlen != 0 && !msg.msg_iov) return -EFAULT;
    if (msg.msg_iovlen > 0 &&
        ((uint64_t)(uintptr_t)msg.msg_iov > UINT64_MAX -
         (uint64_t)msg.msg_iovlen * sizeof(struct iovec) ||
         !user_access_ok(msg.msg_iov,
                         (uint64_t)msg.msg_iovlen * sizeof(struct iovec),
                         UACCESS_READ)))
        return -EFAULT;

    (void)flags;
    msg.msg_flags = 0;
    user_controllen = msg.msg_controllen;

    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    is_dgram = sock->domain == AF_INET && sock->type == SOCK_DGRAM;
    if (sock->pending_fd_count > 0) {
        nfds = sock->pending_fd_count;
        out_fds = NULL;
        needed_size = sizeof(struct cmsghdr) +
                      (size_t)nfds * sizeof(int);
        needed = needed_size <= 0xFFFFFFFFUL ?
                 (socklen_t)needed_size : 0;
        if (msg.msg_control && needed_size <= 0xFFFFFFFFUL &&
            needed <= msg.msg_controllen) {
            if (!user_access_ok(msg.msg_control, needed, UACCESS_WRITE)) {
                spin_unlock(&socket_table_lock);
                return -EFAULT;
            }
            out_fds = (int *)kmalloc((size_t)nfds * sizeof(int));
            if (!out_fds) {
                spin_unlock(&socket_table_lock);
                return -ENOMEM;
            }
            memset(&cmsg, 0, sizeof(cmsg));
            cmsg.cmsg_len = needed;
            cmsg.cmsg_level = SOL_SOCKET;
            cmsg.cmsg_type = SCM_RIGHTS;
            for (i = 0; i < nfds; i++) {
                newfd = task_fd_alloc(current_task);
                if (newfd < 0) {
                    out_fds[i] = -1;
                    socket_release_pending_fd(&sock->pending_fds[i]);
                    continue;
                }
                memcpy(&current_task->fds[newfd], &sock->pending_fds[i], sizeof(task_fd_t));
                current_task->fds[newfd].in_use = 1;
                current_task->fds[newfd].ref_count = 1;
                memset(&sock->pending_fds[i], 0, sizeof(task_fd_t));
                out_fds[i] = newfd;
            }
            control_data = (uint8_t *)msg.msg_control + sizeof(struct cmsghdr);
            if (copy_to_user(msg.msg_control, &cmsg, sizeof(cmsg)) < 0 ||
                copy_to_user(control_data, out_fds,
                             (size_t)nfds * sizeof(int)) < 0) {
                kfree(out_fds);
                spin_unlock(&socket_table_lock);
                return -EFAULT;
            }
            kfree(out_fds);
            msg.msg_controllen = needed;
            sock->pending_fd_count = 0;
            kfree(sock->pending_fds);
            sock->pending_fds = NULL;
            sock->pending_fd_capacity = 0;
        } else {
            for (i = 0; i < nfds; i++) {
                socket_release_pending_fd(&sock->pending_fds[i]);
            }
            sock->pending_fd_count = 0;
            kfree(sock->pending_fds);
            sock->pending_fds = NULL;
            sock->pending_fd_capacity = 0;
            msg.msg_flags |= MSG_CTRUNC;
            msg.msg_controllen = 0;
        }
    } else {
        msg.msg_controllen = 0;
    }
    if (sock->so_passcred && sock->domain == AF_UNIX && msg.msg_control) {
        cred_len = sizeof(struct cmsghdr) + sizeof(struct ucred);
        if (msg.msg_controllen <= user_controllen &&
            cred_len <= user_controllen - msg.msg_controllen) {
            memset(&cmsg, 0, sizeof(cmsg));
            cmsg.cmsg_len = cred_len;
            cmsg.cmsg_level = SOL_SOCKET;
            cmsg.cmsg_type = SCM_CREDENTIALS;
            credentials.pid = sock->peer_pid;
            credentials.uid = sock->peer_uid;
            credentials.gid = sock->peer_gid;
            control_data = (uint8_t *)msg.msg_control + msg.msg_controllen;
            if (!user_access_ok(control_data, cred_len, UACCESS_WRITE)) {
                spin_unlock(&socket_table_lock);
                return -EFAULT;
            }
            if (copy_to_user(control_data, &cmsg, sizeof(cmsg)) < 0 ||
                copy_to_user(control_data + sizeof(cmsg), &credentials,
                             sizeof(credentials)) < 0) {
                spin_unlock(&socket_table_lock);
                return -EFAULT;
            }
            msg.msg_controllen += cred_len;
        } else {
            msg.msg_flags |= MSG_CTRUNC;
        }
    }
    spin_unlock(&socket_table_lock);

    total = 0;
    for (iov_index = 0; iov_index < msg.msg_iovlen; iov_index++) {
        if (copy_from_user(&iov,
                (const void *)(uintptr_t)((uint64_t)(uintptr_t)msg.msg_iov +
                (uint64_t)iov_index * sizeof(struct iovec)), sizeof(iov)) < 0)
            return -EFAULT;
        if (iov.iov_len > 0x7FFFFFFFUL) return -EINVAL;
        if (!user_access_ok(iov.iov_base, iov.iov_len, UACCESS_WRITE))
            return -EFAULT;
        recvd = sys_recvfrom(sockfd, (const char *)(uintptr_t)iov.iov_base,
                             (int)iov.iov_len,
                             flags | (is_dgram ? MSG_TRUNC : 0), 0, 0);
        if (recvd < 0) return recvd;
        if (is_dgram && (uint64_t)recvd > iov.iov_len) {
            msg.msg_flags |= MSG_TRUNC;
            total += (ssize_t)iov.iov_len;
            break;
        }
        total += recvd;
        if ((size_t)recvd < iov.iov_len) break;
    }

    if (copy_to_user((void *)(uintptr_t)msg_ptr, &msg, sizeof(msg)) < 0)
        return -EFAULT;

    return total;
}

static int sys_shutdown(int sockfd, const char *how_ptr, int unused) {
    int how;
    socket_t *sock;
    socket_t *peer;
    tcp_socket_t *tcp;

    (void)unused;
    spin_lock(&socket_table_lock);
    sock = get_socket(sockfd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    
    how = (int)(uintptr_t)how_ptr;
    
    if (sock->state != SOCKSTATE_CONNECTED) {
        spin_unlock(&socket_table_lock);
        return -ENOTCONN;
    }
    
    switch (how) {
        case SHUT_RD:
            sock->shut_rd = 1;
            break;
        case SHUT_WR:
            sock->shut_wr = 1;
            break;
        case SHUT_RDWR:
            sock->shut_rd = 1;
            sock->shut_wr = 1;
            break;
        default:
            spin_unlock(&socket_table_lock);
            return -EINVAL;
    }

    if ((how == SHUT_WR || how == SHUT_RDWR) &&
        sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        peer = &sockets[sock->peer_socket];
        if (peer->in_use) {
            peer->peer_write_closed = 1;
        }
    }
    tcp = sock->tcp;
    spin_unlock(&socket_table_lock);
    descriptor_ready_notify();
    if (tcp && (how == SHUT_WR || how == SHUT_RDWR))
        tcp_disconnect(tcp, 1000);
    return 0;
}

int socket_poll_events(int fd) {
    int events;
    int readable;
    int writable;
    socket_t *sock;
    socket_t *peer;

    spin_lock(&socket_table_lock);
    sock = get_socket(fd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (sock->domain == AF_INET) {
        spin_unlock(&socket_table_lock);
        netif_poll_all();
        spin_lock(&socket_table_lock);
        sock = get_socket(fd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return 0;
        }
    }
    
    events = 0;

    if (sock->state == SOCKSTATE_CONNECTING && sock->tcp) {
        if (sock->tcp->state == TCP_STATE_ESTABLISHED) {
            sock->state = SOCKSTATE_CONNECTED;
        } else if (sock->tcp->state == TCP_STATE_CLOSED) {
            sock->state = SOCKSTATE_CLOSED;
            if (!sock->error) sock->error = ECONNREFUSED;
        }
    }

    socket_recv_lock(sock);
    readable = recv_buf_used(sock) > 0;
    socket_recv_unlock(sock);
    if (readable) {
        events |= 0x01;
    }

    if (sock->tcp && sock->tcp->recv_buffer_head != sock->tcp->recv_buffer_tail) {
        events |= 0x01;
    }
    if (sock->tcp && sock->tcp->recv_buffer_head == sock->tcp->recv_buffer_tail &&
        (sock->tcp->state == TCP_STATE_CLOSE_WAIT ||
         sock->tcp->state == TCP_STATE_CLOSED)) {
        events |= 0x11;
    }
    if (sock->udp && sock->udp->has_data) events |= 0x01;
    
    if ((sock->tcp && sock->state != SOCKSTATE_CONNECTING) || sock->udp) {
        events |= 0x04;
    } else if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        peer = &sockets[sock->peer_socket];
        writable = 0;
        if (peer->in_use) {
            socket_recv_lock(peer);
            writable = recv_buf_free(peer) > 0;
            socket_recv_unlock(peer);
        }
        if (writable) events |= 0x04;
    }
    
    if (sock->state == SOCKSTATE_LISTENING && sock->backlog_count > 0) {
        events |= 0x01;
    }

    if (sock->state == SOCKSTATE_LISTENING && sock->domain == AF_INET &&
        sock->type == SOCK_STREAM &&
        tcp_accept_pending(sock->local_port)) {
        events |= 0x01;
    }
    
    if (sock->error) {
        events |= 0x08;
    }

    if (sock->shut_rd) {
        events |= 0x10;
    }
    if (sock->domain == AF_UNIX && sock->state == SOCKSTATE_CONNECTED &&
        (sock->peer_socket < 0 || sock->peer_write_closed))
        events |= 0x11;
    spin_unlock(&socket_table_lock);
    return events;
}

int is_socket_fd(int fd) {
    int result;

    spin_lock(&socket_table_lock);
    result = get_socket(fd) != NULL;
    spin_unlock(&socket_table_lock);
    return result;
}

#define SIOCGIFADDR    0x8915
#define SIOCGIFNETMASK 0x891b
#define SIOCGIFMTU     0x8921

int socket_ioctl(int fd, unsigned long request, uint64_t arg) {
    socket_t *sock;
    netif_t *netif;
    uint64_t available;
    int value;
    uint8_t ifreq[32];

    if (!arg) return -EFAULT;
    if (request == FIONBIO) {
        if (copy_from_user(&value, (const void *)(uintptr_t)arg,
                           sizeof(value)) < 0)
            return -EFAULT;
        spin_lock(&socket_table_lock);
        sock = get_socket(fd);
        if (!sock) {
            spin_unlock(&socket_table_lock);
            return -EBADF;
        }
        sock->nonblocking = value ? 1 : 0;
        if (value)
            current_task->fds[fd].flags |= 0x800;
        else
            current_task->fds[fd].flags &= ~0x800UL;
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (request != FIONREAD) {
        if (request != SIOCGIFADDR && request != SIOCGIFNETMASK &&
            request != SIOCGIFMTU)
            return -ENOTTY;
        if (copy_from_user(ifreq, (const void *)(uintptr_t)arg,
                           sizeof(ifreq)) < 0)
            return -EFAULT;
        ifreq[15] = 0;
        if (ifreq[0])
            netif = netif_find((const char *)ifreq);
        else
            netif = netif_get_default();
        if (!netif || !netif->link_up) return -ENODEV;
        memset(ifreq + 16, 0, 16);
        if (request == SIOCGIFMTU) {
            value = netif->mtu > INT32_MAX ? INT32_MAX : (int)netif->mtu;
            memcpy(ifreq + 16, &value, sizeof(value));
        } else {
            ipv4_addr_t ip = request == SIOCGIFADDR ? netif->ipv4 :
                                                        netif->netmask;
            ifreq[16] = AF_INET & 0xFF;
            ifreq[17] = (AF_INET >> 8) & 0xFF;
            memcpy(ifreq + 20, &ip, 4);
        }
        if (copy_to_user((void *)(uintptr_t)arg, ifreq, sizeof(ifreq)) < 0)
            return -EFAULT;
        return 0;
    }
    spin_lock(&socket_table_lock);
    sock = get_socket(fd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    socket_recv_lock(sock);
    available = recv_buf_used(sock);
    socket_recv_unlock(sock);
    if (sock->tcp)
        available = sock->tcp->recv_buffer_tail -
                    sock->tcp->recv_buffer_head;
    else if (sock->udp && sock->udp->has_data)
        available = sock->udp->recv_len;
    spin_unlock(&socket_table_lock);
    if (available > INT32_MAX) available = INT32_MAX;
    value = (int)available;
    if (copy_to_user((void *)(uintptr_t)arg, &value, sizeof(value)) < 0)
        return -EFAULT;
    return 0;
}

int socket_write(int fd, const void *buf, int len) {
    int ret;
    int total;
    int nonblocking;
    int peer_idx;
    socket_t *sock;
    socket_t *peer;
    tcp_socket_t *tcp;
    udp_socket_t *udp;
    uint64_t remote_addr;
    uint16_t remote_port;
    int inet_stream;
    uint64_t ready_generation;
    uint64_t snd_ticks;
    uint64_t snd_deadline;
    int snd_timed;
    int snd_have_deadline;
    const uint8_t *bytes;

    if (len < 0) return -EINVAL;
    if (len == 0) return 0;
    total = 0;
    bytes = (const uint8_t *)buf;
    snd_have_deadline = 0;
    snd_deadline = 0;

retry:
    ready_generation = descriptor_ready_generation();

    spin_lock(&socket_table_lock);
    sock = get_socket(fd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    if (sock->type == SOCK_STREAM && sock->state != SOCKSTATE_CONNECTED) {
        if (sock->state == SOCKSTATE_CONNECTING) {
            spin_unlock(&socket_table_lock);
            return total > 0 ? total : -EAGAIN;
        }
        spin_unlock(&socket_table_lock);
        return -ENOTCONN;
    }
    if (sock->shut_wr) {
        spin_unlock(&socket_table_lock);
        return total > 0 ? total : -EPIPE;
    }
    if (sock->domain == AF_UNIX) {
        nonblocking = sock->nonblocking;
        snd_timed = sock->so_sndtimeo.tv_sec || sock->so_sndtimeo.tv_usec;
        if (snd_timed && !snd_have_deadline) {
            snd_deadline = pit_get_ticks() +
                socket_timeval_to_ticks(&sock->so_sndtimeo);
            snd_have_deadline = 1;
        }
        if (snd_timed) {
            snd_ticks = snd_deadline - pit_get_ticks();
            if ((int64_t)snd_ticks <= 0) {
                spin_unlock(&socket_table_lock);
                return total > 0 ? total : -EAGAIN;
            }
        } else {
            snd_ticks = UINT64_MAX;
        }
        peer_idx = sock->peer_socket;
        if (peer_idx >= 0 && peer_idx < socket_capacity) {
            peer = &sockets[peer_idx];
            if (peer->in_use && !peer->peer_write_closed) {
                ret = recv_buf_write(peer, bytes + total,
                                     (size_t)(len - total));
                spin_unlock(&socket_table_lock);
                if (ret < 0) return total > 0 ? total : ret;
                if (ret > 0) {
                    total += ret;
                    descriptor_ready_notify();
                    if (total == len || nonblocking) return total;
                    goto retry;
                }
                if (nonblocking) return total > 0 ? total : -EAGAIN;
                if (task_has_pending_signals())
                    return total > 0 ? total : -EINTR;
                descriptor_ready_wait(ready_generation, snd_ticks);
                if (task_has_pending_signals())
                    return total > 0 ? total : -EINTR;
                goto retry;
            }
        }
        spin_unlock(&socket_table_lock);
        return total > 0 ? total : -EPIPE;
    }
    if (sock->domain == AF_INET && sock->type == SOCK_DGRAM) {
        if (sock->state != SOCKSTATE_CONNECTED) {
            spin_unlock(&socket_table_lock);
            return -EDESTADDRREQ;
        }
        if (!sock->udp) {
            sock->udp = udp_socket_create(sock->local_port);
            if (!sock->udp) {
                spin_unlock(&socket_table_lock);
                return -EADDRINUSE;
            }
            sock->local_port = sock->udp->local_port;
        }
        remote_addr = sock->remote_addr;
        remote_port = sock->remote_port;
        udp = sock->udp;
        udp->ttl = (uint8_t)sock->ip_ttl;
        spin_unlock(&socket_table_lock);
        ret = udp_socket_send(udp,
                              socket_ipv4_from_addr(remote_addr),
                              remote_port, (uint8_t *)bytes, (uint64_t)len);
        return ret < 0 ? -EIO : len;
    }
    tcp = sock->tcp;
    inet_stream = sock->domain == AF_INET && sock->type == SOCK_STREAM && tcp;
    spin_unlock(&socket_table_lock);
    if (inet_stream) {
        ret = tcp_send(tcp, (uint8_t *)buf, (uint64_t)len);
        if (ret < 0) return -EIO;
        return ret;
    }
    return -EOPNOTSUPP;
}

int socket_read(int fd, void *buf, int len) {
    socket_t *sock;
    tcp_socket_t *tcp;
    udp_socket_t *udp;
    int ret;
    int wait_result;
    int nonblocking;
    int inet_stream;
    int inet_dgram;
    uint64_t timeout_ms;
    ipv4_addr_t source_ip;
    uint16_t source_port;

    spin_lock(&socket_table_lock);
    sock = get_socket(fd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    tcp = sock->tcp;
    nonblocking = sock->nonblocking;
    inet_stream = sock->domain == AF_INET && sock->type == SOCK_STREAM && tcp;
    inet_dgram = sock->domain == AF_INET && sock->type == SOCK_DGRAM;
    if (sock->shut_rd) {
        spin_unlock(&socket_table_lock);
        return 0;
    }
    if (inet_stream)
        timeout_ms = socket_rcv_timeout_ms(sock, nonblocking, 0, 15000);
    if (inet_dgram) {
        if (!sock->udp) {
            sock->udp = udp_socket_create(sock->local_port);
            if (!sock->udp) {
                spin_unlock(&socket_table_lock);
                return -EADDRINUSE;
            }
            sock->local_port = sock->udp->local_port;
        }
        udp = sock->udp;
        if (nonblocking)
            timeout_ms = 0;
        else if (sock->so_rcvtimeo.tv_sec || sock->so_rcvtimeo.tv_usec)
            timeout_ms = (uint64_t)sock->so_rcvtimeo.tv_sec * 1000 +
                         (uint64_t)sock->so_rcvtimeo.tv_usec / 1000;
        else
            timeout_ms = UINT64_MAX;
    }
    spin_unlock(&socket_table_lock);
    if (inet_dgram) {
        ret = udp_socket_recv(udp, (uint8_t *)buf, (uint64_t)len,
                              &source_ip, &source_port, timeout_ms, 0, NULL);
        if (ret < 0) return timeout_ms == 0 ? -EAGAIN : -ETIMEDOUT;
        return ret;
    }
    if (inet_stream) {
        ret = tcp_recv(tcp, (uint8_t *)buf, (uint64_t)len, timeout_ms, 0);
        if (ret < 0) return -EIO;
        if (ret == 0) {
            if (nonblocking) return -EAGAIN;
            if (tcp->state != TCP_STATE_CLOSE_WAIT &&
                tcp->state != TCP_STATE_CLOSED)
                return -EAGAIN;
        }
        return ret;
    }
    wait_result = socket_wait_for_data(&sock, fd, 0);
    if (wait_result <= 0) return wait_result;
    ret = (int)recv_buf_read(sock, buf, len, 0);
    spin_unlock(&socket_table_lock);
    if (ret > 0) descriptor_ready_notify();
    return ret;
}

int socket_close_fd(int fd) {
    int idx;
    int result;

    result = 0;
    spin_lock(&socket_table_lock);
    idx = socket_fd_index(current_task, fd);
    if (idx < 0 || idx >= socket_capacity) result = -EBADF;
    else if (!sockets[idx].in_use) result = -EBADF;
    else {
        memset(&current_task->fds[fd], 0, sizeof(task_fd_t));
        socket_release_index(idx, 1);
        socket_reclaim_storage();
    }
    spin_unlock(&socket_table_lock);
    if (result == 0) {
        task_fd_reclaim_unused(current_task);
        descriptor_ready_notify();
    }
    return result;
}

void socket_close_range(unsigned int first, unsigned int last, int cloexec) {
    unsigned int fd;
    int idx;
    int i;

    if (!current_task) return;
    spin_lock(&socket_table_lock);
    for (i = current_task->fds_capacity - 1; i >= 0; i--) {
        if (!current_task->fds[i].in_use ||
            current_task->fds[i].type != FD_TYPE_SOCKET)
            continue;
        fd = (unsigned int)i;
        if (fd < first || fd > last) continue;
        if (cloexec) {
            current_task->fds[i].flags |= 1;
        } else {
            idx = socket_fd_index(current_task, i);
            memset(&current_task->fds[i], 0, sizeof(task_fd_t));
            socket_release_index(idx, 1);
        }
    }
    if (!cloexec) socket_reclaim_storage();
    spin_unlock(&socket_table_lock);
    if (!cloexec) descriptor_ready_notify();
}

#define SOCKET_F_DUPFD       0
#define SOCKET_F_GETFD       1
#define SOCKET_F_SETFD       2
#define SOCKET_F_GETFL       3
#define SOCKET_F_SETFL       4
#define SOCKET_F_DUPFD_CLOEXEC 1030

int socket_fcntl(int fd, int cmd, int arg) {
    socket_t *sock;
    task_fd_t *descriptor;
    int idx;
    int result;

    spin_lock(&socket_table_lock);
    sock = get_socket(fd);
    if (!sock) {
        spin_unlock(&socket_table_lock);
        return -EBADF;
    }
    descriptor = &current_task->fds[fd];
    idx = socket_fd_index(current_task, fd);
    result = 0;
    switch (cmd) {
        case SOCKET_F_DUPFD:
            result = socket_fd_alloc_from(idx, descriptor->flags & ~1UL,
                                          arg);
            break;
        case SOCKET_F_DUPFD_CLOEXEC:
            result = socket_fd_alloc_from(idx, descriptor->flags | 1, arg);
            break;
        case SOCKET_F_GETFD:
            result = descriptor->flags & 1 ? 1 : 0;
            break;
        case SOCKET_F_SETFD:
            if (arg & 1)
                descriptor->flags |= 1;
            else
                descriptor->flags &= ~1UL;
            break;
        case SOCKET_F_GETFL:
            result = sock->nonblocking ? 0x800 : 0;
            break;
        case SOCKET_F_SETFL:
            sock->nonblocking = (arg & 0x800) ? 1 : 0;
            descriptor->flags = (descriptor->flags & 1) |
                                ((uint64_t)arg & ~1UL);
            break;
        default:
            result = -EINVAL;
            break;
    }
    spin_unlock(&socket_table_lock);
    return result;
}

static void socket_icmp_error(uint8_t proto, uint16_t local_port, int error) {
    int i;

    spin_lock(&socket_table_lock);
    for (i = 0; i < socket_capacity; i++) {
        if (!sockets[i].in_use || sockets[i].domain != AF_INET)
            continue;
        if ((proto == IP_PROTO_TCP && sockets[i].type != SOCK_STREAM) ||
            (proto == IP_PROTO_UDP && sockets[i].type != SOCK_DGRAM))
            continue;
        if (sockets[i].local_port != local_port) continue;
        if (!sockets[i].error) sockets[i].error = error;
    }
    spin_unlock(&socket_table_lock);
    descriptor_ready_notify();
}

void syscalls_socket_init(void) {
    sockets = NULL;
    socket_capacity = 0;
    spinlock_init(&socket_table_lock);
    icmp_register_error_hook(socket_icmp_error);
    
    syscall_table_set(SYSCALL_SOCKET, (void *)(sys_socket));
    syscall_table_set(SYSCALL_SOCKETPAIR, (void *)(sys_socketpair));
    syscall_table_set(SYSCALL_BIND, (void *)(sys_bind));
    syscall_table_set(SYSCALL_CONNECT, (void *)(sys_connect));
    syscall_table_set(SYSCALL_LISTEN, (void *)(sys_listen));
    syscall_table_set(SYSCALL_ACCEPT, (void *)(sys_accept));
    syscall_table_set(SYSCALL_ACCEPT4, (void *)(sys_accept4));
    syscall_table_set(SYSCALL_GETSOCKOPT, (void *)(sys_getsockopt));
    syscall_table_set(SYSCALL_SETSOCKOPT, (void *)(sys_setsockopt));
    syscall_table_set(SYSCALL_GETSOCKNAME, (void *)(sys_getsockname));
    syscall_table_set(SYSCALL_GETPEERNAME, (void *)(sys_getpeername));
    syscall_table_set(SYSCALL_SENDTO, (void *)(sys_sendto));
    syscall_table_set(SYSCALL_SENDMSG, (void *)(sys_sendmsg));
    syscall_table_set(SYSCALL_RECVFROM, (void *)(sys_recvfrom));
    syscall_table_set(SYSCALL_RECVMSG, (void *)(sys_recvmsg));
    syscall_table_set(SYSCALL_SHUTDOWN, (void *)(sys_shutdown));
}
