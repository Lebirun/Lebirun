#include "syscall_defs.h"
#include <lebirun/task.h>
#include <lebirun/drivers/net/tcp.h>

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
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

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

#define SOCKET_INIT_COUNT 1
#define SOCKET_BUF_SIZE 4096
#define BACKLOG_INIT_SIZE 8
#define UNIX_PATH_MAX 108
#define SCM_MAX_FDS 16

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
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
    socklen_t msg_namelen;
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

typedef enum {
    SOCKSTATE_CLOSED = 0,
    SOCKSTATE_BOUND,
    SOCKSTATE_LISTENING,
    SOCKSTATE_CONNECTING,
    SOCKSTATE_CONNECTED,
    SOCKSTATE_SHUTDOWN_RD,
    SOCKSTATE_SHUTDOWN_WR,
    SOCKSTATE_SHUTDOWN_RDWR
} sock_state_t;

struct cmsghdr {
    uint64_t cmsg_len;
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
    uint64_t remote_addr;
    uint16_t remote_port;
    uint8_t *recv_buf;
    uint32_t recv_capacity;
    uint64_t recv_head;
    uint64_t recv_tail;
    int nonblocking;
    int cloexec;
    int error;
    int so_reuseaddr;
    int so_reuseport;
    int so_keepalive;
    int so_broadcast;
    int so_sndbuf;
    int so_rcvbuf;
    struct timeval so_rcvtimeo;
    struct timeval so_sndtimeo;
    int backlog_size;
    int backlog_count;
    int backlog_capacity;
    int peer_socket;
    tcp_socket_t *tcp;
    sock_state_t state;
    pending_conn_t *backlog;
    char *sun_path;
    task_fd_t *pending_fds;
    int pending_fd_count;
    int pending_fd_capacity;
} socket_t;

static socket_t *sockets = NULL;
static int socket_capacity = 0;
static int socket_base_fd = 100;
static uint64_t next_ephemeral_port = 49152;

static int socket_grow(void) {
    int new_cap;
    int i;
    socket_t *new_arr;

    new_cap = socket_capacity ? socket_capacity * 2 : SOCKET_INIT_COUNT;
    new_arr = (socket_t *)krealloc(sockets, new_cap * sizeof(socket_t));
    if (!new_arr) return -1;
    for (i = socket_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(socket_t));
    }
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
    sockets[i].so_sndbuf = SOCKET_BUF_SIZE;
    sockets[i].so_rcvbuf = SOCKET_BUF_SIZE;
    sockets[i].peer_socket = -1;
    return i;
}

static void socket_release_pending_fd(task_fd_t *fd) {
    pipe_t *pipe;

    if (!fd || !fd->in_use) return;
    if (fd->type == FD_TYPE_FILE && fd->node) {
        vfs_close((vfs_node_t *)fd->node);
    } else if ((fd->type == FD_TYPE_PIPE_R || fd->type == FD_TYPE_PIPE_W) && fd->private_data) {
        pipe = (pipe_t *)fd->private_data;
        if (pipe_release_reference(pipe, fd->type)) {
            if (pipe->buffer) kfree(pipe->buffer);
            kfree(pipe);
        }
    }
    memset(fd, 0, sizeof(task_fd_t));
}

static int socket_ensure_pending_fd_capacity(socket_t *sock, int needed) {
    task_fd_t *new_fds;
    int new_capacity;

    if (!sock || needed < 0 || needed > SCM_MAX_FDS) return -ENOMEM;
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

static void free_socket(int idx) {
    int i;

    if (idx >= 0 && idx < socket_capacity) {
        for (i = 0; i < sockets[idx].pending_fd_count; i++) {
            socket_release_pending_fd(&sockets[idx].pending_fds[i]);
        }
        sockets[idx].pending_fd_count = 0;
        if (sockets[idx].tcp) {
            tcp_disconnect(sockets[idx].tcp, 1000);
            tcp_socket_close(sockets[idx].tcp);
        }
        kfree(sockets[idx].recv_buf);
        kfree(sockets[idx].backlog);
        kfree(sockets[idx].sun_path);
        kfree(sockets[idx].pending_fds);
        sockets[idx].recv_buf = NULL;
        sockets[idx].backlog = NULL;
        sockets[idx].sun_path = NULL;
        sockets[idx].pending_fds = NULL;
        sockets[idx].pending_fd_capacity = 0;
        sockets[idx].tcp = NULL;
        sockets[idx].in_use = 0;
    }
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
    int idx = fd - socket_base_fd;
    if (idx < 0 || idx >= socket_capacity) return NULL;
    if (!sockets[idx].in_use) return NULL;
    return &sockets[idx];
}

static uint16_t alloc_ephemeral_port(void) {
    uint16_t port = next_ephemeral_port++;
    if (next_ephemeral_port > 65535) {
        next_ephemeral_port = 49152;
    }
    return port;
}

static size_t recv_buf_used(socket_t *sock) {
    return sock->recv_tail - sock->recv_head;
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

static void socket_compact_recv_buffer(socket_t *sock) {
    uint64_t used;
    uint8_t *new_buffer;
    uint64_t i;

    if (!sock) return;
    used = sock->recv_tail - sock->recv_head;
    if (used == sock->recv_capacity) return;
    if (used == 0) {
        kfree(sock->recv_buf);
        sock->recv_buf = NULL;
        sock->recv_capacity = 0;
        sock->recv_head = 0;
        sock->recv_tail = 0;
        return;
    }
    new_buffer = (uint8_t *)kmalloc(used);
    if (!new_buffer) return;
    for (i = 0; i < used; i++) {
        new_buffer[i] = sock->recv_buf[(sock->recv_head + i) % sock->recv_capacity];
    }
    kfree(sock->recv_buf);
    sock->recv_buf = new_buffer;
    sock->recv_capacity = (uint32_t)used;
    sock->recv_head = 0;
    sock->recv_tail = used;
}

static int recv_buf_write(socket_t *sock, const void *data, size_t len) {
    size_t free;
    size_t to_write;
    const uint8_t *src;
    size_t i;

    free = recv_buf_free(sock);
    to_write = (len < free) ? len : free;
    if (to_write > 0 && socket_ensure_recv_buf(sock, to_write) < 0) return -ENOMEM;
    src = (const uint8_t *)data;
    for (i = 0; i < to_write; i++) {
        sock->recv_buf[sock->recv_tail % sock->recv_capacity] = src[i];
        sock->recv_tail++;
    }
    return (int)to_write;
}

static size_t recv_buf_read(socket_t *sock, void *data, size_t len, int peek) {
    size_t used;
    size_t to_read;
    uint8_t *dst;
    uint64_t head;
    size_t i;

    used = recv_buf_used(sock);
    to_read = (len < used) ? len : used;
    dst = (uint8_t *)data;
    head = sock->recv_head;
    for (i = 0; i < to_read; i++) {
        dst[i] = sock->recv_buf[head % sock->recv_capacity];
        head++;
    }
    if (!peek) {
        sock->recv_head = head;
        socket_compact_recv_buffer(sock);
    }
    return to_read;
}

static int sys_socket(int domain, const char *type_ptr, int protocol) {
    int type = (int)(uintptr_t)type_ptr;
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int idx;
    type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    
    if (domain != AF_INET && domain != AF_UNIX && domain != AF_INET6) {
        return -EAFNOSUPPORT;
    }
    
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW && type != SOCK_SEQPACKET) {
        return -ESOCKTNOSUPPORT;
    }
    
    idx = alloc_socket();
    if (idx < 0) return -EMFILE;
    
    sockets[idx].domain = domain;
    sockets[idx].type = type;
    sockets[idx].protocol = protocol;
    sockets[idx].state = SOCKSTATE_CLOSED;
    sockets[idx].nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
    sockets[idx].cloexec = (flags & SOCK_CLOEXEC) ? 1 : 0;
    
    return socket_base_fd + idx;
}

static int sys_socketpair(int domain, const char *type_ptr, int protocol_sv) {
    int type = (int)(uintptr_t)type_ptr;
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int *sv;
    int idx1;
    int idx2;
    type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    sv = (int *)(uintptr_t)protocol_sv;
    
    if (domain != AF_UNIX) {
        return -EAFNOSUPPORT;
    }
    
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) {
        return -ESOCKTNOSUPPORT;
    }
    
    idx1 = alloc_socket();
    if (idx1 < 0) return -EMFILE;
    
    idx2 = alloc_socket();
    if (idx2 < 0) {
        free_socket(idx1);
        return -EMFILE;
    }
    
    sockets[idx1].domain = domain;
    sockets[idx1].type = type;
    sockets[idx1].state = SOCKSTATE_CONNECTED;
    sockets[idx1].nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
    sockets[idx1].cloexec = (flags & SOCK_CLOEXEC) ? 1 : 0;
    sockets[idx1].peer_socket = idx2;
    
    sockets[idx2].domain = domain;
    sockets[idx2].type = type;
    sockets[idx2].state = SOCKSTATE_CONNECTED;
    sockets[idx2].nonblocking = (flags & SOCK_NONBLOCK) ? 1 : 0;
    sockets[idx2].cloexec = (flags & SOCK_CLOEXEC) ? 1 : 0;
    sockets[idx2].peer_socket = idx1;
    
    sv[0] = socket_base_fd + idx1;
    sv[1] = socket_base_fd + idx2;
    
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

static int sys_bind(int sockfd, const char *addr_ptr, int addrlen) {
    struct sockaddr_in *addr;
    struct sockaddr_in6 *addr6;
    struct sockaddr_un *uaddr;
    socket_t *sock;

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    if (sock->state != SOCKSTATE_CLOSED) {
        return -EINVAL;
    }
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        if (!uaddr || addrlen < 3) {
            return -EINVAL;
        }
        if (uaddr->sun_family != AF_UNIX) {
            return -EAFNOSUPPORT;
        }
        if (socket_set_sun_path(sock, uaddr->sun_path) < 0) return -ENOMEM;
        sock->state = SOCKSTATE_BOUND;
        return 0;
    }
    
    if (sock->domain == AF_INET6) {
        addr6 = (struct sockaddr_in6 *)(uintptr_t)addr_ptr;
        if (!addr6 || addrlen < (int)sizeof(struct sockaddr_in6)) {
            return -EINVAL;
        }
        if (addr6->sin6_family != AF_INET6) {
            return -EAFNOSUPPORT;
        }
        sock->local_port = ntohs(addr6->sin6_port);
        if (sock->local_port == 0) {
            sock->local_port = alloc_ephemeral_port();
        }
        sock->state = SOCKSTATE_BOUND;
        return 0;
    }

    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    if (!addr || addrlen < (int)sizeof(struct sockaddr_in)) {
        return -EINVAL;
    }
    
    if (sock->domain == AF_INET && addr->sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }
    
    sock->local_addr = addr->sin_addr.s_addr;
    sock->local_port = ntohs(addr->sin_port);
    
    if (sock->local_port == 0) {
        sock->local_port = alloc_ephemeral_port();
    }
    
    sock->state = SOCKSTATE_BOUND;
    return 0;
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

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    socket_idx = sockfd - socket_base_fd;
    
    if (sock->state == SOCKSTATE_CONNECTED) {
        return -EISCONN;
    }
    
    if (sock->state == SOCKSTATE_LISTENING) {
        return -EINVAL;
    }
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        if (!uaddr || addrlen < 3) {
            return -EINVAL;
        }
        
        listener_idx = find_unix_listener(uaddr->sun_path);
        if (listener_idx < 0) {
            return -ECONNREFUSED;
        }
        
        if (sockets[listener_idx].backlog_count >= sockets[listener_idx].backlog_size) {
            return -ECONNREFUSED;
        }
        
        peer_idx = alloc_socket();
        if (peer_idx < 0) return -ENOMEM;
        sock = &sockets[socket_idx];
        
        sockets[peer_idx].domain = AF_UNIX;
        sockets[peer_idx].type = sockets[listener_idx].type;
        sockets[peer_idx].state = SOCKSTATE_CONNECTED;
        sockets[peer_idx].peer_socket = sockfd - socket_base_fd;
        if (socket_set_sun_path(&sockets[peer_idx], uaddr->sun_path) < 0) {
            free_socket(peer_idx);
            return -ENOMEM;
        }
        if (socket_set_sun_path(sock, uaddr->sun_path) < 0) {
            free_socket(peer_idx);
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
        sock->state = SOCKSTATE_CONNECTED;
        
        return 0;
    }
    
    if (sock->domain == AF_INET6) {
        addr6 = (struct sockaddr_in6 *)(uintptr_t)addr_ptr;
        if (!addr6 || addrlen < (int)sizeof(struct sockaddr_in6)) {
            return -EINVAL;
        }
        if (addr6->sin6_family != AF_INET6) {
            return -EAFNOSUPPORT;
        }
        sock->remote_port = ntohs(addr6->sin6_port);
        if (sock->state == SOCKSTATE_CLOSED) {
            sock->local_port = alloc_ephemeral_port();
        }
        if (sock->nonblocking) {
            sock->state = SOCKSTATE_CONNECTING;
            return -EINPROGRESS;
        }
        sock->state = SOCKSTATE_CONNECTED;
        return 0;
    }

    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    if (!addr || addrlen < (int)sizeof(struct sockaddr_in)) {
        return -EINVAL;
    }
    
    if (sock->domain == AF_INET && addr->sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }
    
    sock->remote_addr = addr->sin_addr.s_addr;
    sock->remote_port = ntohs(addr->sin_port);
    
    if (sock->state == SOCKSTATE_CLOSED) {
        sock->local_port = alloc_ephemeral_port();
    }
    
    sock->tcp = tcp_socket_create();
    if (!sock->tcp) {
        return -ENOMEM;
    }

    if (sock->nonblocking) {
        sock->state = SOCKSTATE_CONNECTING;
        return -EINPROGRESS;
    }

    if (tcp_connect(sock->tcp, socket_ipv4_from_addr(addr->sin_addr.s_addr),
                    sock->remote_port, 60000) < 0) {
        tcp_socket_close(sock->tcp);
        sock->tcp = NULL;
        sock->state = SOCKSTATE_CLOSED;
        return -ECONNREFUSED;
    }

    sock->local_port = sock->tcp->local_port;
    sock->local_addr = socket_addr_from_ipv4(sock->tcp->local_ip);
    sock->state = SOCKSTATE_CONNECTED;
    return 0;
}

static int sys_listen(int sockfd, const char *backlog_ptr, int unused) {
    int backlog;
    socket_t *sock;
    pending_conn_t *new_backlog;

    (void)unused;
    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    backlog = (int)(uintptr_t)backlog_ptr;
    
    if (sock->state != SOCKSTATE_BOUND) {
        return -EINVAL;
    }
    
    if (sock->type != SOCK_STREAM && sock->type != SOCK_SEQPACKET) {
        return -EOPNOTSUPP;
    }
    
    if (backlog < 1) backlog = 1;
    if (backlog > 128) backlog = 128;

    new_backlog = (pending_conn_t *)kmalloc(backlog * sizeof(pending_conn_t));
    if (!new_backlog) return -ENOMEM;
    memset(new_backlog, 0, backlog * sizeof(pending_conn_t));
    kfree(sock->backlog);
    sock->backlog = new_backlog;
    sock->backlog_capacity = backlog;
    
    sock->backlog_size = backlog;
    sock->backlog_count = 0;
    sock->state = SOCKSTATE_LISTENING;
    
    return 0;
}

static int sys_accept(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    int idx;
    int i;
    int listener_idx;
    int conn_idx;
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socklen_t *user_addrlen;
    pending_conn_t *conn;
    socket_t *sock;

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    listener_idx = sockfd - socket_base_fd;
    
    if (sock->state != SOCKSTATE_LISTENING) {
        return -EINVAL;
    }
    
    if (sock->backlog_count == 0) {
        if (sock->nonblocking) {
            return -EAGAIN;
        }
        return -EAGAIN;
    }
    
    if (sock->domain == AF_UNIX) {
        conn = NULL;
        for (i = 0; i < sock->backlog_size; i++) {
            if (sock->backlog[i].valid) {
                conn = &sock->backlog[i];
                break;
            }
        }
        if (!conn) return -EAGAIN;
        
        idx = conn->peer_idx;
        conn->valid = 0;
        sock->backlog_count--;
        
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        user_addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
        if (uaddr && user_addrlen && *user_addrlen >= sizeof(struct sockaddr_un)) {
            uaddr->sun_family = AF_UNIX;
            memset(uaddr->sun_path, 0, UNIX_PATH_MAX);
            *user_addrlen = sizeof(uint16_t);
        }
        
        return socket_base_fd + idx;
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
        return -EAGAIN;
    }
    
    idx = alloc_socket();
    if (idx < 0) return -EMFILE;
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
    
    conn->valid = 0;
    sock->backlog_count--;
    
    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    user_addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
    if (addr && user_addrlen && *user_addrlen >= sizeof(struct sockaddr_in)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(sockets[idx].remote_port);
        addr->sin_addr.s_addr = sockets[idx].remote_addr;
        *user_addrlen = sizeof(struct sockaddr_in);
    }
    
    return socket_base_fd + idx;
}

static int sys_accept4(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    return sys_accept(sockfd, addr_ptr, addrlen_ptr);
}

static int sys_getsockopt(int sockfd, const char *level_ptr, int optname,
                          int optval_ptr, int optlen_ptr, int unused) {
    int level;
    int *optval;
    socklen_t *optlen;
    int value;
    socket_t *sock = get_socket(sockfd);
    (void)unused;
    if (!sock) return -EBADF;
    
    level = (int)(uintptr_t)level_ptr;
    optval = (int *)(uintptr_t)optval_ptr;
    optlen = (socklen_t *)(uintptr_t)optlen_ptr;
    if (!optval || !optlen || *optlen < sizeof(int)) return -EINVAL;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;

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
        default:
            return -ENOPROTOOPT;
    }

    *optval = value;
    *optlen = sizeof(int);
    return 0;
}

static int sys_setsockopt(int sockfd, const char *level_ptr, int optname,
                          int optval_ptr, int optlen, int unused) {
    int level;
    int value;
    int *optval;
    socket_t *sock = get_socket(sockfd);
    (void)unused;
    if (!sock) return -EBADF;
    
    level = (int)(uintptr_t)level_ptr;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    if (optlen < (int)sizeof(int)) return -EINVAL;
    optval = (int *)(uintptr_t)optval_ptr;
    if (!optval) return -EINVAL;
    value = *optval;

    switch (optname) {
        case SO_REUSEADDR:
            sock->so_reuseaddr = value ? 1 : 0;
            break;
        case SO_REUSEPORT:
            sock->so_reuseport = value ? 1 : 0;
            break;
        case SO_KEEPALIVE:
            sock->so_keepalive = value ? 1 : 0;
            break;
        case SO_BROADCAST:
            sock->so_broadcast = value ? 1 : 0;
            break;
        case SO_SNDBUF:
            if (value <= 0) return -EINVAL;
            if (value > SOCKET_BUF_SIZE) value = SOCKET_BUF_SIZE;
            sock->so_sndbuf = value;
            break;
        case SO_RCVBUF:
            if (value <= 0) return -EINVAL;
            if (value > SOCKET_BUF_SIZE) value = SOCKET_BUF_SIZE;
            sock->so_rcvbuf = value;
            break;
        default:
            return -ENOPROTOOPT;
    }

    return 0;
}

static int sys_getsockname(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socket_t *sock;
    const char *path;
    socklen_t pathlen;
    socklen_t *addrlen;

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
        if (uaddr && addrlen) {
            path = sock->sun_path ? sock->sun_path : "";
            pathlen = strlen(path);
            uaddr->sun_family = AF_UNIX;
            if (*addrlen > sizeof(uint16_t)) {
                strncpy(uaddr->sun_path, path,
                        *addrlen - sizeof(uint16_t));
            }
            *addrlen = sizeof(uint16_t) + pathlen + 1;
            return 0;
        }
        return -EINVAL;
    }
    
    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(sock->local_port);
        addr->sin_addr.s_addr = sock->local_addr;
        *addrlen = sizeof(struct sockaddr_in);
        return 0;
    }
    
    return -EINVAL;
}

static int sys_getpeername(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socket_t *sock;
    socklen_t *addrlen;

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    if (sock->state != SOCKSTATE_CONNECTED) {
        return -ENOTCONN;
    }
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
        if (uaddr && addrlen) {
            uaddr->sun_family = AF_UNIX;
            memset(uaddr->sun_path, 0, UNIX_PATH_MAX);
            *addrlen = sizeof(uint16_t);
            return 0;
        }
        return -EINVAL;
    }
    
    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(sock->remote_port);
        addr->sin_addr.s_addr = sock->remote_addr;
        *addrlen = sizeof(struct sockaddr_in);
        return 0;
    }
    
    return -EINVAL;
}

static int sys_sendto(int sockfd, const char *buf_ptr, int len,
                      int flags, int dest_addr_ptr, int addrlen) {
    const void *buf;
    int ret;
    socket_t *sock;
    socket_t *peer;

    sock = get_socket(sockfd);
    (void)flags;
    (void)dest_addr_ptr;
    (void)addrlen;
    if (!sock) return -EBADF;
    
    if (sock->type == SOCK_STREAM && sock->state != SOCKSTATE_CONNECTED) {
        return -ENOTCONN;
    }
    
    buf = (const void *)(uintptr_t)buf_ptr;

    if (sock->domain == AF_INET && sock->type == SOCK_STREAM && sock->tcp) {
        ret = tcp_send(sock->tcp, (uint8_t *)buf, (uint64_t)len);
        if (ret < 0) return -EIO;
        return ret;
    }
    
    if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        peer = &sockets[sock->peer_socket];
        if (peer->in_use) {
            return recv_buf_write(peer, buf, len);
        }
    }
    
    return -EOPNOTSUPP;
}

static int sys_sendmsg(int sockfd, const char *msg_ptr, int flags) {
    struct msghdr *msg;
    struct cmsghdr *cmsg;
    ssize_t total;
    ssize_t sent;
    socket_t *sock;
    socket_t *peer;
    task_fd_t *src_tfd;
    pipe_t *passed_pipe;
    int nfds_to_pass;
    int *fd_arr;
    int i;
    int src_fd;
    size_t iov_index;

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;

    msg = (struct msghdr *)(uintptr_t)msg_ptr;
    if (!msg) return -EFAULT;

    (void)flags;

    if (msg->msg_control && msg->msg_controllen >= sizeof(struct cmsghdr)) {
        cmsg = (struct cmsghdr *)msg->msg_control;
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            if (cmsg->cmsg_len < sizeof(struct cmsghdr) ||
                cmsg->cmsg_len > msg->msg_controllen)
                return -EINVAL;
            nfds_to_pass = (int)((cmsg->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int));
            fd_arr = (int *)((uint8_t *)cmsg + sizeof(struct cmsghdr));
            if (nfds_to_pass > SCM_MAX_FDS) nfds_to_pass = SCM_MAX_FDS;
            if (sock->peer_socket < 0 || sock->peer_socket >= socket_capacity)
                return -ENOTCONN;
            peer = &sockets[sock->peer_socket];
            if (!peer->in_use) return -ENOTCONN;
            if (peer->pending_fd_count + nfds_to_pass > SCM_MAX_FDS)
                return -ENOMEM;
            for (i = 0; i < nfds_to_pass; i++) {
                src_fd = fd_arr[i];
                if (!current_task || src_fd < 0 || src_fd >= current_task->fds_capacity)
                    return -EBADF;
                if (!current_task->fds[src_fd].in_use) return -EBADF;
            }
            if (socket_ensure_pending_fd_capacity(
                    peer, peer->pending_fd_count + nfds_to_pass) < 0)
                return -ENOMEM;
            for (i = 0; i < nfds_to_pass; i++) {
                src_fd = fd_arr[i];
                src_tfd = &current_task->fds[src_fd];
                memcpy(&peer->pending_fds[peer->pending_fd_count], src_tfd, sizeof(task_fd_t));
                if (src_tfd->type == FD_TYPE_FILE && src_tfd->node) {
                    vfs_open((vfs_node_t *)src_tfd->node, 0);
                    task_fd_position_share(src_tfd, &peer->pending_fds[peer->pending_fd_count]);
                }
                if (src_tfd->private_data && (src_tfd->type == FD_TYPE_PIPE_R || src_tfd->type == FD_TYPE_PIPE_W)) {
                    passed_pipe = (pipe_t *)src_tfd->private_data;
                    pipe_retain_reference(passed_pipe, src_tfd->type);
                }
                peer->pending_fd_count++;
            }
        }
    }

    total = 0;
    for (iov_index = 0; iov_index < msg->msg_iovlen; iov_index++) {
        sent = sys_sendto(sockfd, (const char *)(uintptr_t)msg->msg_iov[iov_index].iov_base,
                          msg->msg_iov[iov_index].iov_len, flags, 0, 0);
        if (sent < 0) return sent;
        total += sent;
    }

    return total;
}

static int sys_recvfrom(int sockfd, const char *buf_ptr, int len,
                        int flags, int src_addr_ptr, int addrlen_ptr) {
    void *buf;
    size_t available;
    uint64_t timeout_ms;
    int ret;
    socket_t *sock = get_socket(sockfd);
    (void)src_addr_ptr;
    (void)addrlen_ptr;
    if (!sock) return -EBADF;
    
    buf = (void *)(uintptr_t)buf_ptr;

    if (sock->domain == AF_INET && sock->type == SOCK_STREAM && sock->tcp) {
        timeout_ms = (sock->nonblocking || (flags & MSG_DONTWAIT)) ? 0 : 15000;
        ret = tcp_recv(sock->tcp, (uint8_t *)buf, (uint64_t)len, timeout_ms);
        if (ret < 0) return -EIO;
        if (ret == 0 && (sock->nonblocking || (flags & MSG_DONTWAIT))) return -EAGAIN;
        return ret;
    }
    
    available = recv_buf_used(sock);
    if (available == 0) {
        if (sock->nonblocking) {
            return -EAGAIN;
        }
        return 0;
    }
    
    return (int)recv_buf_read(sock, buf, len, flags & MSG_PEEK);
}

static int sys_recvmsg(int sockfd, const char *msg_ptr, int flags) {
    struct msghdr *msg;
    struct cmsghdr *cmsg;
    ssize_t total;
    ssize_t recvd;
    socket_t *sock;
    int nfds;
    uint64_t needed;
    int *out_fds;
    int i;
    int newfd;
    size_t iov_index;

    sock = get_socket(sockfd);
    if (!sock) return -EBADF;

    msg = (struct msghdr *)(uintptr_t)msg_ptr;
    if (!msg) return -EFAULT;

    (void)flags;

    if (msg->msg_control && msg->msg_controllen >= sizeof(struct cmsghdr) && sock->pending_fd_count > 0) {
        cmsg = (struct cmsghdr *)msg->msg_control;
        nfds = sock->pending_fd_count;
        needed = sizeof(struct cmsghdr) + (uint64_t)nfds * sizeof(int);
        if (needed <= msg->msg_controllen) {
            cmsg->cmsg_len = needed;
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            out_fds = (int *)((uint8_t *)cmsg + sizeof(struct cmsghdr));
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
            msg->msg_controllen = needed;
            sock->pending_fd_count = 0;
            kfree(sock->pending_fds);
            sock->pending_fds = NULL;
            sock->pending_fd_capacity = 0;
        }
    } else if (msg->msg_control) {
        msg->msg_controllen = 0;
    }

    total = 0;
    for (iov_index = 0; iov_index < msg->msg_iovlen; iov_index++) {
        recvd = sys_recvfrom(sockfd, (const char *)(uintptr_t)msg->msg_iov[iov_index].iov_base,
                             msg->msg_iov[iov_index].iov_len, flags, 0, 0);
        if (recvd < 0) return recvd;
        total += recvd;
        if ((size_t)recvd < msg->msg_iov[iov_index].iov_len) break;
    }

    return total;
}

static int sys_shutdown(int sockfd, const char *how_ptr, int unused) {
    int how;
    socket_t *sock;

    (void)unused;
    sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    how = (int)(uintptr_t)how_ptr;
    
    if (sock->state != SOCKSTATE_CONNECTED) {
        return -ENOTCONN;
    }
    
    switch (how) {
        case SHUT_RD:
            sock->state = SOCKSTATE_SHUTDOWN_RD;
            break;
        case SHUT_WR:
            sock->state = SOCKSTATE_SHUTDOWN_WR;
            break;
        case SHUT_RDWR:
            sock->state = SOCKSTATE_SHUTDOWN_RDWR;
            break;
        default:
            return -EINVAL;
    }

    if (sock->tcp && (how == SHUT_WR || how == SHUT_RDWR)) {
        tcp_disconnect(sock->tcp, 1000);
    }
    
    return 0;
}

int socket_poll_events(int fd) {
    int events;
    socket_t *sock;
    socket_t *peer;

    sock = get_socket(fd);
    if (!sock) return 0;
    
    events = 0;
    
    if (recv_buf_used(sock) > 0) {
        events |= 0x01;
    }

    if (sock->tcp && sock->tcp->recv_buffer_head != sock->tcp->recv_buffer_tail) {
        events |= 0x01;
    }
    
    if (sock->tcp) {
        events |= 0x04;
    } else if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        peer = &sockets[sock->peer_socket];
        if (peer->in_use && recv_buf_free(peer) > 0) events |= 0x04;
    }
    
    if (sock->state == SOCKSTATE_LISTENING && sock->backlog_count > 0) {
        events |= 0x01;
    }
    
    if (sock->error) {
        events |= 0x08;
    }
    
    if (sock->state == SOCKSTATE_SHUTDOWN_RD || sock->state == SOCKSTATE_SHUTDOWN_RDWR) {
        events |= 0x10;
    }
    
    return events;
}

int is_socket_fd(int fd) {
    return get_socket(fd) != NULL;
}

int socket_write(int fd, const void *buf, int len) {
    int ret;
    socket_t *sock;
    socket_t *peer;

    sock = get_socket(fd);
    if (!sock) return -EBADF;
    if (sock->type == SOCK_STREAM && sock->state != SOCKSTATE_CONNECTED)
        return -ENOTCONN;
    if (sock->domain == AF_INET && sock->type == SOCK_STREAM && sock->tcp) {
        ret = tcp_send(sock->tcp, (uint8_t *)buf, (uint64_t)len);
        if (ret < 0) return -EIO;
        return ret;
    }
    if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        peer = &sockets[sock->peer_socket];
        if (peer->in_use) {
            ret = recv_buf_write(peer, buf, len);
            if (ret == 0 && len > 0 && sock->nonblocking)
                return -EAGAIN;
            return ret;
        }
    }
    return -EOPNOTSUPP;
}

int socket_read(int fd, void *buf, int len) {
    socket_t *sock = get_socket(fd);
    size_t available;
    int ret;
    if (!sock) return -EBADF;
    if (sock->domain == AF_INET && sock->type == SOCK_STREAM && sock->tcp) {
        ret = tcp_recv(sock->tcp, (uint8_t *)buf, (uint64_t)len,
                       sock->nonblocking ? 0 : 15000);
        if (ret < 0) return -EIO;
        if (ret == 0 && sock->nonblocking) return -EAGAIN;
        return ret;
    }
    available = recv_buf_used(sock);
    if (available == 0) {
        if (sock->nonblocking) return -EAGAIN;
        return 0;
    }
    return recv_buf_read(sock, buf, len, 0);
}

int socket_close_fd(int fd) {
    int idx;

    idx = fd - socket_base_fd;
    if (idx < 0 || idx >= socket_capacity) return -EBADF;
    if (!sockets[idx].in_use) return -EBADF;
    free_socket(idx);
    return 0;
}

#define SOCKET_F_DUPFD       0
#define SOCKET_F_GETFD       1
#define SOCKET_F_SETFD       2
#define SOCKET_F_GETFL       3
#define SOCKET_F_SETFL       4

int socket_fcntl(int fd, int cmd, int arg) {
    socket_t *sock = get_socket(fd);
    if (!sock) return -EBADF;
    switch (cmd) {
        case SOCKET_F_GETFD:
            return sock->cloexec ? 1 : 0;
        case SOCKET_F_SETFD:
            sock->cloexec = (arg & 1) ? 1 : 0;
            return 0;
        case SOCKET_F_GETFL:
            return sock->nonblocking ? 0x800 : 0;
        case SOCKET_F_SETFL:
            sock->nonblocking = (arg & 0x800) ? 1 : 0;
            return 0;
        default:
            return -EINVAL;
    }
}

void syscalls_socket_init(void) {
    sockets = NULL;
    socket_capacity = 0;
    
    syscall_table[SYSCALL_SOCKET] = sys_socket;
    syscall_table[SYSCALL_SOCKETPAIR] = sys_socketpair;
    syscall_table[SYSCALL_BIND] = sys_bind;
    syscall_table[SYSCALL_CONNECT] = sys_connect;
    syscall_table[SYSCALL_LISTEN] = sys_listen;
    syscall_table[SYSCALL_ACCEPT] = sys_accept;
    syscall_table[SYSCALL_ACCEPT4] = sys_accept4;
    syscall_table[SYSCALL_GETSOCKOPT] = sys_getsockopt;
    syscall_table[SYSCALL_SETSOCKOPT] = sys_setsockopt;
    syscall_table[SYSCALL_GETSOCKNAME] = sys_getsockname;
    syscall_table[SYSCALL_GETPEERNAME] = sys_getpeername;
    syscall_table[SYSCALL_SENDTO] = sys_sendto;
    syscall_table[SYSCALL_SENDMSG] = sys_sendmsg;
    syscall_table[SYSCALL_RECVFROM] = sys_recvfrom;
    syscall_table[SYSCALL_RECVMSG] = sys_recvmsg;
    syscall_table[SYSCALL_SHUTDOWN] = sys_shutdown;
}
