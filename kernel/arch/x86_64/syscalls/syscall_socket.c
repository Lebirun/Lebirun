#include "syscall_defs.h"
#include <lebirun/task.h>

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

#define SOCKET_INIT_COUNT 8
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
    uint64_t s_addr;
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
    uint64_t recv_head;
    uint64_t recv_tail;
    uint8_t *send_buf;
    uint64_t send_head;
    uint64_t send_tail;
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
    sock_state_t state;
    pending_conn_t *backlog;
    char *sun_path;
    task_fd_t pending_fds[SCM_MAX_FDS];
    int pending_fd_count;
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
    sockets[i].recv_buf = (uint8_t *)kmalloc(SOCKET_BUF_SIZE);
    if (!sockets[i].recv_buf) return -1;
    sockets[i].send_buf = (uint8_t *)kmalloc(SOCKET_BUF_SIZE);
    if (!sockets[i].send_buf) {
        kfree(sockets[i].recv_buf);
        sockets[i].recv_buf = NULL;
        return -1;
    }
    memset(sockets[i].recv_buf, 0, SOCKET_BUF_SIZE);
    memset(sockets[i].send_buf, 0, SOCKET_BUF_SIZE);
    sockets[i].in_use = 1;
    sockets[i].so_sndbuf = SOCKET_BUF_SIZE;
    sockets[i].so_rcvbuf = SOCKET_BUF_SIZE;
    sockets[i].peer_socket = -1;
    return i;
}

static void free_socket(int idx) {
    if (idx >= 0 && idx < socket_capacity) {
        kfree(sockets[idx].recv_buf);
        kfree(sockets[idx].send_buf);
        kfree(sockets[idx].backlog);
        kfree(sockets[idx].sun_path);
        sockets[idx].recv_buf = NULL;
        sockets[idx].send_buf = NULL;
        sockets[idx].backlog = NULL;
        sockets[idx].sun_path = NULL;
        sockets[idx].in_use = 0;
    }
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
    return SOCKET_BUF_SIZE - recv_buf_used(sock);
}

static size_t send_buf_used(socket_t *sock) {
    return sock->send_tail - sock->send_head;
}

static size_t send_buf_free(socket_t *sock) {
    return SOCKET_BUF_SIZE - send_buf_used(sock);
}

static size_t recv_buf_write(socket_t *sock, const void *data, size_t len) {
    size_t free = recv_buf_free(sock);
    size_t to_write = (len < free) ? len : free;
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < to_write; i++) {
        sock->recv_buf[sock->recv_tail % SOCKET_BUF_SIZE] = src[i];
        sock->recv_tail++;
    }
    return to_write;
}

static size_t recv_buf_read(socket_t *sock, void *data, size_t len, int peek) {
    size_t used = recv_buf_used(sock);
    size_t to_read = (len < used) ? len : used;
    uint8_t *dst = (uint8_t *)data;
    uint64_t head = sock->recv_head;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = sock->recv_buf[head % SOCKET_BUF_SIZE];
        head++;
    }
    if (!peek) {
        sock->recv_head = head;
    }
    return to_read;
}

static size_t send_buf_write(socket_t *sock, const void *data, size_t len) {
    size_t free = send_buf_free(sock);
    size_t to_write = (len < free) ? len : free;
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < to_write; i++) {
        sock->send_buf[sock->send_tail % SOCKET_BUF_SIZE] = src[i];
        sock->send_tail++;
    }
    return to_write;
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
    if (!sock->sun_path) {
        sock->sun_path = (char *)kmalloc(UNIX_PATH_MAX);
        if (!sock->sun_path) return -ENOMEM;
    }
    strncpy(sock->sun_path, path, UNIX_PATH_MAX - 1);
    sock->sun_path[UNIX_PATH_MAX - 1] = '\0';
    return 0;
}

static int sys_bind(int sockfd, const char *addr_ptr, int addrlen) {
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socket_t *sock = get_socket(sockfd);
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
        struct sockaddr_in6 *addr6;
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
    struct sockaddr_un *uaddr;
    int listener_idx;
    int peer_idx;
    int i;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
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
        
        sockets[peer_idx].domain = AF_UNIX;
        sockets[peer_idx].type = sockets[listener_idx].type;
        sockets[peer_idx].state = SOCKSTATE_CONNECTED;
        sockets[peer_idx].peer_socket = sockfd - socket_base_fd;
        socket_set_sun_path(&sockets[peer_idx], uaddr->sun_path);
        
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
        socket_set_sun_path(sock, uaddr->sun_path);
        
        return 0;
    }
    
    if (sock->domain == AF_INET6) {
        struct sockaddr_in6 *addr6;
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
    
    if (sock->nonblocking) {
        sock->state = SOCKSTATE_CONNECTING;
        return -EINPROGRESS;
    }
    
    sock->state = SOCKSTATE_CONNECTED;
    return 0;
}

static int sys_listen(int sockfd, const char *backlog_ptr, int unused) {
    int backlog;
    (void)unused;
    socket_t *sock = get_socket(sockfd);
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

    if (sock->backlog) kfree(sock->backlog);
    sock->backlog = (pending_conn_t *)kmalloc(backlog * sizeof(pending_conn_t));
    if (!sock->backlog) return -ENOMEM;
    memset(sock->backlog, 0, backlog * sizeof(pending_conn_t));
    sock->backlog_capacity = backlog;
    
    sock->backlog_size = backlog;
    sock->backlog_count = 0;
    sock->state = SOCKSTATE_LISTENING;
    
    return 0;
}

static int sys_accept(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    int idx;
    int i;
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
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
        pending_conn_t *conn = NULL;
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
        socklen_t *addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
        if (uaddr && addrlen && *addrlen >= sizeof(struct sockaddr_un)) {
            uaddr->sun_family = AF_UNIX;
            memset(uaddr->sun_path, 0, UNIX_PATH_MAX);
            *addrlen = sizeof(uint16_t);
        }
        
        return socket_base_fd + idx;
    }
    
    pending_conn_t *conn = NULL;
    for (i = 0; i < sock->backlog_size; i++) {
        if (sock->backlog[i].valid) {
            conn = &sock->backlog[i];
            break;
        }
    }
    
    if (!conn) {
        return -EAGAIN;
    }
    
    idx = alloc_socket();
    if (idx < 0) return -EMFILE;
    
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
    socklen_t *addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(sockets[idx].remote_port);
        addr->sin_addr.s_addr = sockets[idx].remote_addr;
        *addrlen = sizeof(struct sockaddr_in);
    }
    
    return socket_base_fd + idx;
}

static int sys_accept4(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    return sys_accept(sockfd, addr_ptr, addrlen_ptr);
}

static int sys_getsockopt(int sockfd, const char *level_ptr, int optname) {
    int level;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    level = (int)(uintptr_t)level_ptr;
    (void)level;
    (void)optname;
    
    return 0;
}

static int sys_setsockopt(int sockfd, const char *level_ptr, int optname) {
    int level;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    level = (int)(uintptr_t)level_ptr;
    (void)level;
    (void)optname;
    
    return 0;
}

static int sys_getsockname(int sockfd, const char *addr_ptr, int addrlen_ptr) {
    struct sockaddr_in *addr;
    struct sockaddr_un *uaddr;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    if (sock->domain == AF_UNIX) {
        const char *path;
        socklen_t pathlen;
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        socklen_t *addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
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
    socklen_t *addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
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
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    if (sock->state != SOCKSTATE_CONNECTED) {
        return -ENOTCONN;
    }
    
    if (sock->domain == AF_UNIX) {
        uaddr = (struct sockaddr_un *)(uintptr_t)addr_ptr;
        socklen_t *addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
        if (uaddr && addrlen) {
            uaddr->sun_family = AF_UNIX;
            memset(uaddr->sun_path, 0, UNIX_PATH_MAX);
            *addrlen = sizeof(uint16_t);
            return 0;
        }
        return -EINVAL;
    }
    
    addr = (struct sockaddr_in *)(uintptr_t)addr_ptr;
    socklen_t *addrlen = (socklen_t *)(uintptr_t)addrlen_ptr;
    
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(sock->remote_port);
        addr->sin_addr.s_addr = sock->remote_addr;
        *addrlen = sizeof(struct sockaddr_in);
        return 0;
    }
    
    return -EINVAL;
}

static int sys_sendto(int sockfd, const char *buf_ptr, int len) {
    const void *buf;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    if (sock->type == SOCK_STREAM && sock->state != SOCKSTATE_CONNECTED) {
        return -ENOTCONN;
    }
    
    buf = (const void *)(uintptr_t)buf_ptr;
    
    if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        socket_t *peer = &sockets[sock->peer_socket];
        if (peer->in_use) {
            return recv_buf_write(peer, buf, len);
        }
    }
    
    return send_buf_write(sock, buf, len);
}

static int sys_sendmsg(int sockfd, const char *msg_ptr, int flags) {
    struct msghdr *msg;
    ssize_t total;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;

    msg = (struct msghdr *)(uintptr_t)msg_ptr;
    if (!msg) return -EFAULT;

    (void)flags;

    if (msg->msg_control && msg->msg_controllen >= sizeof(struct cmsghdr)) {
        struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int nfds_to_pass = (int)((cmsg->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int));
            int *fd_arr = (int *)((uint8_t *)cmsg + sizeof(struct cmsghdr));
            socket_t *peer;
            int i;
            int src_fd;
            task_fd_t *src_tfd;
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
                src_tfd = &current_task->fds[src_fd];
                if (!src_tfd->in_use) return -EBADF;
                memcpy(&peer->pending_fds[peer->pending_fd_count], src_tfd, sizeof(task_fd_t));
                peer->pending_fds[peer->pending_fd_count].read_buf = NULL;
                peer->pending_fds[peer->pending_fd_count].read_buf_offset = 0;
                peer->pending_fds[peer->pending_fd_count].read_buf_len = 0;
                if (src_tfd->private_data && (src_tfd->type == FD_TYPE_PIPE_R || src_tfd->type == FD_TYPE_PIPE_W)) {
                    pipe_t *p = (pipe_t *)src_tfd->private_data;
                    if (src_tfd->type == FD_TYPE_PIPE_R) p->readers++;
                    else p->writers++;
                }
                peer->pending_fd_count++;
            }
        }
    }

    total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t sent = sys_sendto(sockfd, (const char *)(uintptr_t)msg->msg_iov[i].iov_base, 
                                  msg->msg_iov[i].iov_len);
        if (sent < 0) return sent;
        total += sent;
    }

    return total;
}

static int sys_recvfrom(int sockfd, const char *buf_ptr, int len) {
    void *buf;
    size_t available;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;
    
    buf = (void *)(uintptr_t)buf_ptr;
    
    available = recv_buf_used(sock);
    if (available == 0) {
        if (sock->nonblocking) {
            return -EAGAIN;
        }
        return 0;
    }
    
    return recv_buf_read(sock, buf, len, 0);
}

static int sys_recvmsg(int sockfd, const char *msg_ptr, int flags) {
    struct msghdr *msg;
    ssize_t total;
    socket_t *sock = get_socket(sockfd);
    if (!sock) return -EBADF;

    msg = (struct msghdr *)(uintptr_t)msg_ptr;
    if (!msg) return -EFAULT;

    (void)flags;

    if (msg->msg_control && msg->msg_controllen >= sizeof(struct cmsghdr) && sock->pending_fd_count > 0) {
        struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
        int nfds = sock->pending_fd_count;
        uint64_t needed = sizeof(struct cmsghdr) + (uint64_t)nfds * sizeof(int);
        int *out_fds;
        int i;
        int newfd;
        if (needed <= msg->msg_controllen) {
            cmsg->cmsg_len = needed;
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            out_fds = (int *)((uint8_t *)cmsg + sizeof(struct cmsghdr));
            for (i = 0; i < nfds; i++) {
                newfd = task_fd_alloc(current_task);
                if (newfd < 0) {
                    out_fds[i] = -1;
                    continue;
                }
                memcpy(&current_task->fds[newfd], &sock->pending_fds[i], sizeof(task_fd_t));
                current_task->fds[newfd].in_use = 1;
                current_task->fds[newfd].ref_count = 1;
                current_task->fds[newfd].read_buf = NULL;
                current_task->fds[newfd].read_buf_offset = 0;
                current_task->fds[newfd].read_buf_len = 0;
                out_fds[i] = newfd;
            }
            msg->msg_controllen = needed;
            sock->pending_fd_count = 0;
        }
    } else if (msg->msg_control) {
        msg->msg_controllen = 0;
    }

    total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t recvd = sys_recvfrom(sockfd, (const char *)(uintptr_t)msg->msg_iov[i].iov_base,
                                     msg->msg_iov[i].iov_len);
        if (recvd < 0) return recvd;
        total += recvd;
        if ((size_t)recvd < msg->msg_iov[i].iov_len) break;
    }

    return total;
}

static int sys_shutdown(int sockfd, const char *how_ptr, int unused) {
    int how;
    (void)unused;
    socket_t *sock = get_socket(sockfd);
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
    
    return 0;
}

int socket_poll_events(int fd) {
    int events;
    socket_t *sock = get_socket(fd);
    if (!sock) return 0;
    
    events = 0;
    
    if (recv_buf_used(sock) > 0) {
        events |= 0x01;
    }
    
    if (send_buf_free(sock) > 0) {
        events |= 0x04;
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
    socket_t *sock = get_socket(fd);
    if (!sock) return -EBADF;
    if (sock->type == SOCK_STREAM && sock->state != SOCKSTATE_CONNECTED)
        return -ENOTCONN;
    if (sock->peer_socket >= 0 && sock->peer_socket < socket_capacity) {
        socket_t *peer = &sockets[sock->peer_socket];
        if (peer->in_use) {
            ret = recv_buf_write(peer, buf, len);
            if (ret == 0 && len > 0 && sock->nonblocking)
                return -EAGAIN;
            return ret;
        }
    }
    ret = send_buf_write(sock, buf, len);
    if (ret == 0 && len > 0 && sock->nonblocking)
        return -EAGAIN;
    return ret;
}

int socket_read(int fd, void *buf, int len) {
    socket_t *sock = get_socket(fd);
    size_t available;
    if (!sock) return -EBADF;
    available = recv_buf_used(sock);
    if (available == 0) {
        if (sock->nonblocking) return -EAGAIN;
        return 0;
    }
    return recv_buf_read(sock, buf, len, 0);
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
    memset(sockets, 0, sizeof(sockets));
    
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
