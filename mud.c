#include "mud.h"

#ifdef __APPLE__
#define __APPLE_USE_RFC_3542
#endif

#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ifaddrs.h>

#include <sodium.h>

#if defined IP_PKTINFO
#define MUD_PKTINFO IP_PKTINFO
#define MUD_PKTINFO_SRC(X) &((struct in_pktinfo *)(X))->ipi_addr
#define MUD_PKTINFO_DST(X) &((struct in_pktinfo *)(X))->ipi_spec_dst
#define MUD_PKTINFO_SIZE sizeof(struct in_pktinfo)
#elif defined IP_RECVDSTADDR
#define MUD_PKTINFO IP_RECVDSTADDR
#define MUD_PKTINFO_SRC(X) (X)
#define MUD_PKTINFO_DST(X) (X)
#define MUD_PKTINFO_SIZE sizeof(struct in_addr)
#endif

#if defined IP_DONTFRAG
#define MUD_DFRAG IP_DONTFRAG
#define MUD_DFRAG_OPT 1
#elif defined IP_MTU_DISCOVER
#define MUD_DFRAG IP_MTU_DISCOVER
#define MUD_DFRAG_OPT IP_PMTUDISC_DO
#endif

#define MUD_ONE_MSEC (UINT64_C(1000))
#define MUD_ONE_SEC  (1000*MUD_ONE_MSEC)
#define MUD_ONE_MIN  (60*MUD_ONE_SEC)

#define MUD_U48_SIZE (6U)
#define MUD_KEY_SIZE (32U)
#define MUD_MAC_SIZE (16U)

#define MUD_PACKET_MIN_SIZE  (MUD_U48_SIZE+MUD_MAC_SIZE)
#define MUD_PACKET_MAX_SIZE  (1500U)
#define MUD_PACKET_SIZEOF(X) ((X)+MUD_PACKET_MIN_SIZE)

#define MUD_PONG_SIZE      MUD_PACKET_SIZEOF(MUD_U48_SIZE*4)
#define MUD_PKEY_SIZE      (crypto_scalarmult_BYTES+1)
#define MUD_KEYX_SIZE      MUD_PACKET_SIZEOF(MUD_U48_SIZE+2*MUD_PKEY_SIZE)
#define MUD_MTUX_SIZE      MUD_PACKET_SIZEOF(MUD_U48_SIZE*2)
#define MUD_BAKX_SIZE      MUD_PACKET_SIZEOF(MUD_U48_SIZE+1)

#define MUD_PONG_TIMEOUT   (100*MUD_ONE_MSEC)
#define MUD_KEYX_TIMEOUT   (60*MUD_ONE_MIN)
#define MUD_SEND_TIMEOUT   (MUD_ONE_SEC)
#define MUD_TIME_TOLERANCE (10*MUD_ONE_MIN)

enum mud_msg {
    mud_ping,
    mud_pong,
    mud_keyx,
    mud_mtux,
    mud_bakx,
};

struct ipaddr {
    int family;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } ip;
};

struct path {
    struct {
        unsigned active : 1;
    } state;
    struct ipaddr local_addr;
    struct sockaddr_storage addr;
    struct {
        unsigned char data[256];
        size_t size;
    } ctrl;
    struct {
        uint64_t send_time;
        int remote;
        int local;
    } bak;
    unsigned char *tc;
    uint64_t rdt;
    uint64_t rtt;
    uint64_t sdt;
    uint64_t rst;
    uint64_t r_sdt;
    uint64_t r_rdt;
    uint64_t r_rst;
    int64_t r_dt;
    uint64_t limit;
    uint64_t recv_time;
    uint64_t send_time;
    uint64_t pong_time;
    struct path *next;
};

struct public {
    unsigned char send[MUD_PKEY_SIZE];
    unsigned char recv[MUD_PKEY_SIZE];
};

struct crypto_opt {
    unsigned char *dst;
    struct {
        const unsigned char *data;
        size_t size;
    } src, ad;
    unsigned char npub[16];
};

struct crypto_key {
    struct {
        unsigned char key[MUD_KEY_SIZE];
        crypto_aead_aes256gcm_state state;
    } encrypt, decrypt;
    int aes;
};

struct mud {
    int fd;
    uint64_t send_timeout;
    uint64_t time_tolerance;
    struct path *path;
    struct {
        uint64_t recv_time;
        uint64_t send_time;
        unsigned char secret[crypto_scalarmult_SCALARBYTES];
        struct public public;
        struct crypto_key private, last, next, current;
        int use_next;
        int aes;
        int bad_key;
    } crypto;
    struct {
        uint64_t send_time;
        int remote;
        int local;
    } mtu;
};

static
int mud_encrypt_opt (const struct crypto_key *k, const struct crypto_opt *c)
{
    if (k->aes) {
        return crypto_aead_aes256gcm_encrypt_afternm(
                    c->dst, NULL, c->src.data, c->src.size,
                    c->ad.data, c->ad.size, NULL, c->npub,
                    (const crypto_aead_aes256gcm_state *)&k->encrypt.state);
    } else {
        return crypto_aead_chacha20poly1305_encrypt(
                    c->dst, NULL, c->src.data, c->src.size,
                    c->ad.data, c->ad.size, NULL, c->npub, k->encrypt.key);
    }
}

static
int mud_decrypt_opt (const struct crypto_key *k, const struct crypto_opt *c)
{
    if (k->aes) {
        return crypto_aead_aes256gcm_decrypt_afternm(
                    c->dst, NULL, NULL, c->src.data, c->src.size,
                    c->ad.data, c->ad.size, c->npub,
                    (const crypto_aead_aes256gcm_state *)&k->decrypt.state);
    } else {
        return crypto_aead_chacha20poly1305_decrypt(
                    c->dst, NULL, NULL, c->src.data, c->src.size,
                    c->ad.data, c->ad.size, c->npub, k->decrypt.key);
    }
}

static
void mud_write48 (unsigned char *dst, uint64_t src)
{
    dst[0] = (unsigned char)(UINT64_C(255)&(src));
    dst[1] = (unsigned char)(UINT64_C(255)&(src>>8));
    dst[2] = (unsigned char)(UINT64_C(255)&(src>>16));
    dst[3] = (unsigned char)(UINT64_C(255)&(src>>24));
    dst[4] = (unsigned char)(UINT64_C(255)&(src>>32));
    dst[5] = (unsigned char)(UINT64_C(255)&(src>>40));
}

static
uint64_t mud_read48 (const unsigned char *src)
{
    return ((uint64_t)src[0])
         | ((uint64_t)src[1]<<8)
         | ((uint64_t)src[2]<<16)
         | ((uint64_t)src[3]<<24)
         | ((uint64_t)src[4]<<32)
         | ((uint64_t)src[5]<<40);
}

static
uint64_t mud_now (struct mud *mud)
{
    uint64_t now;
#if defined CLOCK_REALTIME
    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    now = tv.tv_sec*MUD_ONE_SEC+tv.tv_nsec/MUD_ONE_MSEC;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now = tv.tv_sec*MUD_ONE_SEC+tv.tv_usec;
#endif
    return now&((UINT64_C(1)<<48)-1);
}

static
uint64_t mud_abs_diff (uint64_t a, uint64_t b)
{
    return (a >= b) ? a-b : b-a;
}

static
int mud_timeout (uint64_t now, uint64_t last, uint64_t timeout)
{
    return ((!last) || ((now > last) && (now-last >= timeout)));
}

static
void mud_unmapv4 (struct sockaddr *addr)
{
    if (addr->sa_family != AF_INET6)
        return;

    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;

    if (!IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
        return;

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = sin6->sin6_port,
    };

    memcpy(&sin.sin_addr.s_addr,
           &sin6->sin6_addr.s6_addr[12],
           sizeof(sin.sin_addr.s_addr));

    memcpy(addr, &sin, sizeof(sin));
}

static
size_t mud_addrlen (struct sockaddr_storage *addr)
{
    return (addr->ss_family == AF_INET) ? sizeof(struct sockaddr_in)
                                        : sizeof(struct sockaddr_in6);
}

static
int mud_addrinfo (struct sockaddr_storage *addr, const char *host, int port)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };

    if (inet_pton(AF_INET, host, &sin.sin_addr) == 1) {
        memcpy(addr, &sin, sizeof(sin));
        return 0;
    }

    struct sockaddr_in6 sin6 = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
    };

    if (inet_pton(AF_INET6, host, &sin6.sin6_addr) == 1) {
        memcpy(addr, &sin6, sizeof(sin6));
        return 0;
    }

    errno = EINVAL;

    return -1;
}

static
ssize_t mud_send_path (struct mud *mud, struct path *path, uint64_t now,
                       void *data, size_t size, int tc)
{
    if (!size)
        return 0;

    struct iovec iov = {
        .iov_base = data,
        .iov_len = size,
    };

    struct msghdr msg = {
        .msg_name = &path->addr,
        .msg_namelen = mud_addrlen(&path->addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = path->ctrl.data,
        .msg_controllen = path->ctrl.size,
    };

    if (path->tc)
        memcpy(path->tc, &tc, sizeof(tc));

    ssize_t ret = sendmsg(mud->fd, &msg, 0);
    path->send_time = now;

    return ret;
}

static
int mud_sso_int (int fd, int level, int optname, int opt)
{
    return setsockopt(fd, level, optname, &opt, sizeof(opt));
}

static
int mud_cmp_ipaddr (struct ipaddr *a, struct ipaddr *b)
{
    if (a == b)
        return 0;

    if (a->family != b->family)
        return 1;

    if (a->family == AF_INET)
        return memcmp(&a->ip.v4, &b->ip.v4, sizeof(a->ip.v4));

    if (a->family == AF_INET6)
        return memcmp(&a->ip.v6, &b->ip.v6, sizeof(a->ip.v6));

    return 1;
}

static
int mud_cmp_addr (struct sockaddr *a, struct sockaddr *b)
{
    if (a == b)
        return 0;

    if (a->sa_family != b->sa_family)
        return 1;

    if (a->sa_family == AF_INET) {
        struct sockaddr_in *_a = (struct sockaddr_in *)a;
        struct sockaddr_in *_b = (struct sockaddr_in *)b;

        return ((_a->sin_port != _b->sin_port) ||
                (memcmp(&_a->sin_addr, &_b->sin_addr,
                        sizeof(_a->sin_addr))));
    }

    if (a->sa_family == AF_INET6) {
        struct sockaddr_in6 *_a = (struct sockaddr_in6 *)a;
        struct sockaddr_in6 *_b = (struct sockaddr_in6 *)b;

        return ((_a->sin6_port != _b->sin6_port) ||
                (memcmp(&_a->sin6_addr, &_b->sin6_addr,
                        sizeof(_a->sin6_addr))));
    }

    return 1;
}

static
void mud_set_path (struct path *path, struct ipaddr *local_addr,
                   struct sockaddr *addr)
{
    struct msghdr msg = {
        .msg_control = path->ctrl.data,
        .msg_controllen = sizeof(path->ctrl.data),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    memset(path->ctrl.data, 0, sizeof(path->ctrl.data));
    memmove(&path->local_addr, local_addr, sizeof(struct ipaddr));

    if (addr->sa_family == AF_INET) {
        memmove(&path->addr, addr, sizeof(struct sockaddr_in));

        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = MUD_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(MUD_PKTINFO_SIZE);

        memcpy(MUD_PKTINFO_DST(CMSG_DATA(cmsg)),
               &local_addr->ip.v4,
               sizeof(struct in_addr));

        cmsg = CMSG_NXTHDR(&msg, cmsg);

        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = IP_TOS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        path->tc = CMSG_DATA(cmsg);
        path->ctrl.size = CMSG_SPACE(MUD_PKTINFO_SIZE)
                        + CMSG_SPACE(sizeof(int));
    }

    if (addr->sa_family == AF_INET6) {
        memmove(&path->addr, addr, sizeof(struct sockaddr_in6));

        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

        memcpy(&((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr,
               &local_addr->ip.v6,
               sizeof(struct in6_addr));

        cmsg = CMSG_NXTHDR(&msg, cmsg);

        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_TCLASS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        path->tc = CMSG_DATA(cmsg);
        path->ctrl.size = CMSG_SPACE(sizeof(struct in6_pktinfo))
                        + CMSG_SPACE(sizeof(int));
    }
}

static
struct path *mud_path (struct mud *mud, struct ipaddr *local_addr,
                       struct sockaddr *addr, int create)
{
    if (local_addr->family != addr->sa_family)
        return NULL;

    struct path *path;

    for (path = mud->path; path; path = path->next) {
        if (mud_cmp_ipaddr(local_addr, &path->local_addr))
            continue;

        if (mud_cmp_addr(addr, (struct sockaddr *)&path->addr))
            continue;

        break;
    }

    if (path || !create)
        return path;

    path = calloc(1, sizeof(struct path));

    if (!path)
        return NULL;

    mud_set_path(path, local_addr, addr);

    path->next = mud->path;
    mud->path = path;

    return path;
}

static
int mud_ipaddrinfo (struct ipaddr *ipaddr, const char *name)
{
    if (!name) {
        errno = EINVAL;
        return -1;
    }

    if (inet_pton(AF_INET, name, &ipaddr->ip.v4) == 1) {
        ipaddr->family = AF_INET;
        return 0;
    }

    if (inet_pton(AF_INET6, name, &ipaddr->ip.v6) == 1) {
        ipaddr->family = AF_INET6;
        return 0;
    }

    return -1;
}

int mud_peer (struct mud *mud, const char *name, const char *host, int port, int backup)
{
    if (!name || !host || !port) {
        errno = EINVAL;
        return -1;
    }

    struct ipaddr local_addr;

    if (mud_ipaddrinfo(&local_addr, name))
        return -1;

    struct sockaddr_storage addr;

    if (mud_addrinfo(&addr, host, port))
        return -1;

    mud_unmapv4((struct sockaddr *)&addr);

    struct path *path = mud_path(mud, &local_addr,
                                 (struct sockaddr *)&addr, 1);

    if (!path) {
        errno = ENOMEM;
        return -1;
    }

    path->state.active = 1;
    path->bak.local = !!backup;

    return 0;
}

int mud_get_key (struct mud *mud, unsigned char *key, size_t *size)
{
    if (!key || !size || (*size < MUD_KEY_SIZE)) {
        errno = EINVAL;
        return -1;
    }

    memcpy(key, mud->crypto.private.encrypt.key, MUD_KEY_SIZE);
    *size = MUD_KEY_SIZE;

    return 0;
}

int mud_set_key (struct mud *mud, unsigned char *key, size_t size)
{
    if (!key || (size < MUD_KEY_SIZE)) {
        errno = EINVAL;
        return -1;
    }

    memcpy(mud->crypto.private.encrypt.key, key, MUD_KEY_SIZE);
    memcpy(mud->crypto.private.decrypt.key, key, MUD_KEY_SIZE);

    mud->crypto.current = mud->crypto.private;
    mud->crypto.next = mud->crypto.private;
    mud->crypto.last = mud->crypto.private;

    return 0;
}

int mud_set_send_timeout_msec (struct mud *mud, unsigned msec)
{
    if (!msec) {
        errno = EINVAL;
        return -1;
    }

    mud->send_timeout = msec*MUD_ONE_MSEC;

    return 0;
}

int mud_set_time_tolerance_sec (struct mud *mud, unsigned sec)
{
    if (!sec) {
        errno = EINVAL;
        return -1;
    }

    mud->time_tolerance = sec*MUD_ONE_SEC;

    return 0;
}

int mud_get_mtu (struct mud *mud)
{
    if ((!mud->mtu.remote) ||
        (mud->mtu.local < mud->mtu.remote))
        return mud->mtu.local;

    return mud->mtu.remote;
}

int mud_set_mtu (struct mud *mud, int mtu)
{
    if ((mtu < 500) ||
        (mtu > MUD_PACKET_MAX_SIZE-50)) {
        errno = EINVAL;
        return -1;
    }

    if (mud->mtu.local != mtu) {
        mud->mtu.local = mtu;
        mud->mtu.send_time = UINT64_C(0);
    }

    return 0;
}

static
int mud_setup_socket (int fd, int v4, int v6)
{
    if ((mud_sso_int(fd, SOL_SOCKET, SO_REUSEADDR, 1)) ||
        (v4 && mud_sso_int(fd, IPPROTO_IP, MUD_PKTINFO, 1)) ||
        (v6 && mud_sso_int(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1)) ||
        (v6 && mud_sso_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, !v4)))
        return -1;

#ifdef __linux__
        if (v4)
            mud_sso_int(fd, IPPROTO_IP, MUD_DFRAG, MUD_DFRAG_OPT);
#endif

//  mud_sso_int(fd, SOL_SOCKET, SO_RCVBUF, 1<<24);
//  mud_sso_int(fd, SOL_SOCKET, SO_SNDBUF, 1<<24);

    return 0;
}

static
int mud_create_socket (int port, int v4, int v6)
{
    struct sockaddr_storage addr;

    if (mud_addrinfo(&addr, v6 ? "::" : "0.0.0.0", port))
        return -1;

    int fd = socket(addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);

    if (fd == -1)
        return -1;

    if (mud_setup_socket(fd, v4, v6) ||
        bind(fd, (struct sockaddr *)&addr, mud_addrlen(&addr))) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }

    return fd;
}

static
void mud_keyx_init (struct mud *mud)
{
    randombytes_buf(mud->crypto.secret, sizeof(mud->crypto.secret));
    crypto_scalarmult_base(mud->crypto.public.send, mud->crypto.secret);
    memset(mud->crypto.public.recv, 0, sizeof(mud->crypto.public.recv));
    mud->crypto.public.send[MUD_PKEY_SIZE-1] = mud->crypto.aes;
}

struct mud *mud_create (int port, int v4, int v6, int aes, int mtu)
{
    if (sodium_init() == -1)
        return NULL;

    struct mud *mud = calloc(1, sizeof(struct mud));

    if (!mud)
        return NULL;

    mud->fd = mud_create_socket(port, v4, v6);

    if (mud->fd == -1) {
        mud_delete(mud);
        return NULL;
    }

    mud->send_timeout = MUD_SEND_TIMEOUT;
    mud->time_tolerance = MUD_TIME_TOLERANCE;
    mud->mtu.local = mtu;

    unsigned char key[MUD_KEY_SIZE];

    randombytes_buf(key, sizeof(key));
    mud_set_key(mud, key, sizeof(key));

    mud->crypto.aes = aes && crypto_aead_aes256gcm_is_available();
    mud_keyx_init(mud);

    return mud;
}

int mud_get_fd (struct mud *mud)
{
    return mud->fd;
}

void mud_delete (struct mud *mud)
{
    if (!mud)
        return;

    while (mud->path) {
        struct path *path = mud->path;
        mud->path = path->next;
        free(path);
    }

    if (mud->fd != -1) {
        int err = errno;
        close(mud->fd);
        errno = err;
    }

    free(mud);
}

static
int mud_encrypt (struct mud *mud, uint64_t nonce,
                 unsigned char *dst, size_t dst_size,
                 const unsigned char *src, size_t src_size)
{
    if (!nonce)
        return 0;

    size_t size = src_size+MUD_PACKET_MIN_SIZE;

    if (size > dst_size)
        return 0;

    struct crypto_opt opt = {
        .dst = dst+MUD_U48_SIZE,
        .src = { .data = src,
                 .size = src_size },
        .ad  = { .data = dst,
                 .size = MUD_U48_SIZE },
    };

    mud_write48(opt.npub, nonce);
    memcpy(dst, opt.npub, MUD_U48_SIZE);

    if (mud->crypto.use_next) {
        mud_encrypt_opt(&mud->crypto.next, &opt);
    } else {
        mud_encrypt_opt(&mud->crypto.current, &opt);
    }

    return size;
}

static
int mud_decrypt (struct mud *mud,
                 unsigned char *dst, size_t dst_size,
                 const unsigned char *src, size_t src_size)
{
    size_t size = src_size-MUD_PACKET_MIN_SIZE;

    if (size > dst_size)
        return 0;

    struct crypto_opt opt = {
        .dst = dst,
        .src = { .data = src+MUD_U48_SIZE,
                 .size = src_size-MUD_U48_SIZE },
        .ad  = { .data = src,
                 .size = MUD_U48_SIZE },
    };

    memcpy(opt.npub, src, MUD_U48_SIZE);

    if (mud_decrypt_opt(&mud->crypto.current, &opt)) {
        if (!mud_decrypt_opt(&mud->crypto.next, &opt)) {
            mud_keyx_init(mud);
            mud->crypto.last = mud->crypto.current;
            mud->crypto.current = mud->crypto.next;
            mud->crypto.use_next = 0;
        } else {
            if (mud_decrypt_opt(&mud->crypto.last, &opt) &&
                mud_decrypt_opt(&mud->crypto.private, &opt))
                return -1;
        }
    }

    return size;
}

static
int mud_localaddr (struct ipaddr *local_addr, struct msghdr *msg, int family)
{
    int cmsg_level = IPPROTO_IP;
    int cmsg_type = MUD_PKTINFO;

    if (family == AF_INET6) {
        cmsg_level = IPPROTO_IPV6;
        cmsg_type = IPV6_PKTINFO;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);

    for (; cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if ((cmsg->cmsg_level == cmsg_level) &&
            (cmsg->cmsg_type == cmsg_type))
            break;
    }

    if (!cmsg)
        return 1;

    local_addr->family = family;

    if (family == AF_INET) {
        memcpy(&local_addr->ip.v4,
               MUD_PKTINFO_SRC(CMSG_DATA(cmsg)),
               sizeof(struct in_addr));
    } else {
        memcpy(&local_addr->ip.v6,
               &((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr,
               sizeof(struct in6_addr));
    }

    return 0;
}

static
void mud_ctrl_path (struct mud *mud, enum mud_msg msg, struct path *path,
                    uint64_t now)
{
    struct {
        unsigned char zero[MUD_U48_SIZE];
        unsigned char time[MUD_U48_SIZE];
        unsigned char data[128+MUD_MAC_SIZE];
    } ctrl;

    size_t size = 0;

    memset(ctrl.zero, 0, MUD_U48_SIZE);
    mud_write48(ctrl.time, now);

    switch (msg) {
    case mud_pong:
        mud_write48(ctrl.data, path->sdt);
        mud_write48(&ctrl.data[MUD_U48_SIZE], path->rdt);
        mud_write48(&ctrl.data[2*MUD_U48_SIZE], path->rst);
        size = MUD_U48_SIZE*3;
        break;
    case mud_keyx:
        memcpy(ctrl.data, &mud->crypto.public, sizeof(mud->crypto.public));
        size = sizeof(mud->crypto.public);
        break;
    case mud_mtux:
        mud_write48(ctrl.data, (uint64_t)mud->mtu.local);
        size = MUD_U48_SIZE;
        break;
    case mud_bakx:
        ctrl.data[0] = (unsigned char)path->bak.local;
        size = 1;
        break;
    }

    struct crypto_opt opt = {
        .dst = ctrl.data+size,
        .ad  = { .data = ctrl.zero,
                 .size = size+2*MUD_U48_SIZE },
    };

    size += 2*MUD_U48_SIZE+MUD_MAC_SIZE;

    mud_encrypt_opt(&mud->crypto.private, &opt);
    mud_send_path(mud, path, now, &ctrl, size, 0);
}

static
void mud_recv_keyx (struct mud *mud, struct path *path, uint64_t now,
                    unsigned char *data)
{
    struct crypto_key *key = &mud->crypto.next;

    struct {
        unsigned char secret[crypto_scalarmult_BYTES];
        struct public public;
    } shared_send, shared_recv;

    memcpy(&shared_recv.public, data, sizeof(shared_recv.public));

    int sync_send = memcmp(shared_recv.public.recv, mud->crypto.public.send,
                           sizeof(shared_recv.public.recv));

    int sync_recv = memcmp(mud->crypto.public.recv, shared_recv.public.send,
                           sizeof(mud->crypto.public.recv));

    memcpy(shared_recv.public.recv, mud->crypto.public.send,
           sizeof(shared_recv.public.recv));

    memcpy(mud->crypto.public.recv, shared_recv.public.send,
           sizeof(mud->crypto.public.recv));

    mud->crypto.use_next = !sync_send;

    if (sync_send)
        mud_ctrl_path(mud, mud_keyx, path, now);

    if (crypto_scalarmult(shared_recv.secret, mud->crypto.secret,
                          shared_recv.public.send))
        return;

    memcpy(shared_send.secret, shared_recv.secret,
           sizeof(shared_send.secret));

    memcpy(shared_send.public.send, shared_recv.public.recv,
           sizeof(shared_send.public.send));

    memcpy(shared_send.public.recv, shared_recv.public.send,
           sizeof(shared_send.public.recv));

    crypto_generichash(key->encrypt.key, MUD_KEY_SIZE,
                       (unsigned char *)&shared_send, sizeof(shared_send),
                       mud->crypto.private.encrypt.key, MUD_KEY_SIZE);

    crypto_generichash(key->decrypt.key, MUD_KEY_SIZE,
                       (unsigned char *)&shared_recv, sizeof(shared_recv),
                       mud->crypto.private.encrypt.key, MUD_KEY_SIZE);

    key->aes = (shared_recv.public.send[MUD_PKEY_SIZE-1] == 1) &&
               (shared_recv.public.recv[MUD_PKEY_SIZE-1] == 1);

    if (key->aes) {
        crypto_aead_aes256gcm_beforenm(&key->encrypt.state, key->encrypt.key);
        crypto_aead_aes256gcm_beforenm(&key->decrypt.state, key->decrypt.key);
    }

    mud->crypto.recv_time = now;
}

int mud_recv (struct mud *mud, void *data, size_t size)
{
    unsigned char packet[MUD_PACKET_MAX_SIZE];

    struct iovec iov = {
        .iov_base = packet,
        .iov_len = sizeof(packet),
    };

    struct sockaddr_storage addr;
    unsigned char ctrl[256];

    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl,
        .msg_controllen = sizeof(ctrl),
    };

    ssize_t packet_size = recvmsg(mud->fd, &msg, 0);

    if (packet_size <= (ssize_t)MUD_PACKET_MIN_SIZE)
        return -(packet_size == (ssize_t)-1);

    uint64_t now = mud_now(mud);
    uint64_t send_time = mud_read48(packet);

    int mud_packet = !send_time;

    if (mud_packet) {
        if (packet_size < (ssize_t)MUD_PACKET_SIZEOF(MUD_U48_SIZE))
            return 0;

        send_time = mud_read48(&packet[MUD_U48_SIZE]);
    }

    if (mud_abs_diff(now, send_time) >= mud->time_tolerance)
        return 0;

    if (mud_packet) {
        unsigned char tmp[sizeof(packet)];

        struct crypto_opt opt = {
            .dst = tmp,
            .src = { .data = packet+packet_size-MUD_MAC_SIZE,
                     .size = MUD_MAC_SIZE },
            .ad  = { .data = packet,
                     .size = packet_size-MUD_MAC_SIZE },
        };

        if (mud_decrypt_opt(&mud->crypto.private, &opt))
            return 0;
    }

    mud_unmapv4((struct sockaddr *)&addr);

    struct ipaddr local_addr;

    if (mud_localaddr(&local_addr, &msg, addr.ss_family))
        return 0;

    struct path *path = mud_path(mud, &local_addr,
                                 (struct sockaddr *)&addr, mud_packet);

    if (!path)
        return 0;

    if (path->rdt) {
        path->rdt = ((now-path->recv_time)+UINT64_C(7)*path->rdt)/UINT64_C(8);
        path->sdt = ((send_time-path->rst)+UINT64_C(7)*path->sdt)/UINT64_C(8);
    } else if (path->recv_time) {
        path->rdt = now-path->recv_time;
        path->sdt = send_time-path->rst;
    }

    path->rst = send_time;

    if ((!path->bak.local) && (path->recv_time) &&
        (mud_timeout(now, path->pong_time, MUD_PONG_TIMEOUT))) {
        mud_ctrl_path(mud, mud_pong, path, now);
        path->pong_time = now;
    }

    path->recv_time = now;

    if (mud_packet) {
        if (packet_size == (ssize_t)MUD_KEYX_SIZE) {
            mud_recv_keyx(mud, path, now, &packet[MUD_U48_SIZE*2]);
        } else if (packet_size == (ssize_t)MUD_MTUX_SIZE) {
            mud->mtu.remote = (int)mud_read48(&packet[MUD_U48_SIZE*2]);
            if (!path->state.active)
                mud_ctrl_path(mud, mud_mtux, path, now);
        } else if (packet_size == (ssize_t)MUD_PONG_SIZE) {
            path->r_sdt = mud_read48(&packet[MUD_U48_SIZE*2]);
            path->r_rdt = mud_read48(&packet[MUD_U48_SIZE*3]);
            path->r_rst = mud_read48(&packet[MUD_U48_SIZE*4]);
            path->r_dt = send_time-path->r_rst;
            path->rtt = now-path->r_rst;
        } else if (packet_size == (ssize_t)MUD_BAKX_SIZE) {
            path->bak.local = 1;
            path->bak.remote = (int)packet[MUD_U48_SIZE*2];
            if (!path->state.active)
                mud_ctrl_path(mud, mud_bakx, path, now);
        }
        return 0;
    }

    int ret = mud_decrypt(mud, data, size, packet, packet_size);

    if (ret == -1) {
        mud->crypto.bad_key = 1;
        return 0;
    }

    return ret;
}

int mud_send_ctrl (struct mud *mud)
{
    struct path *path;

    for (path = mud->path; path; path = path->next) {
        uint64_t now = mud_now(mud);

        if (!path->state.active) {
            if ((mud->crypto.bad_key) &&
                (mud_timeout(now, mud->crypto.send_time, mud->send_timeout))) {
                mud_ctrl_path(mud, mud_keyx, path, now);
                mud->crypto.send_time = now;
                mud->crypto.bad_key = 0;
            }
        } else {
            if ((mud_timeout(now, mud->crypto.send_time, mud->send_timeout)) &&
                (mud_timeout(now, mud->crypto.recv_time, MUD_KEYX_TIMEOUT))) {
                mud_ctrl_path(mud, mud_keyx, path, now);
                mud->crypto.send_time = now;
                continue;
            }

            if ((!mud->mtu.remote) &&
                (mud_timeout(now, mud->mtu.send_time, mud->send_timeout))) {
                mud_ctrl_path(mud, mud_mtux, path, now);
                mud->mtu.send_time = now;
                continue;
            }

            if ((path->bak.local && !path->bak.remote) &&
                (mud_timeout(now, path->bak.send_time, mud->send_timeout))) {
                mud_ctrl_path(mud, mud_bakx, path, now);
                path->bak.send_time = now;
                continue;
            }

            if (!path->send_time)
                mud_ctrl_path(mud, mud_ping, path, now);
        }
    }
}

int mud_send (struct mud *mud, const void *data, size_t size, int tc)
{
    mud_send_ctrl(mud);

    if (!size)
        return 0;

    if (size > (size_t)mud_get_mtu(mud)) {
        errno = EMSGSIZE;
        return -1;
    }

    uint64_t now = mud_now(mud);
    unsigned char packet[2048];

    int packet_size = mud_encrypt(mud, now, packet, sizeof(packet), data, size);

    if (!packet_size) {
        errno = EINVAL;
        return -1;
    }

    struct path *path;
    struct path *path_min = NULL;
    int64_t limit_min = INT64_MAX;

    for (path = mud->path; path; path = path->next) {
        if (path->bak.local)
            continue;

        int64_t limit = path->limit;
        uint64_t elapsed = now-path->send_time;

        if (limit > elapsed) {
            limit += path->rtt/2-elapsed;
        } else {
            limit = path->rtt/2;
        }

        if (mud_timeout(now, path->recv_time, mud->send_timeout)) {
            mud_send_path(mud, path, now, packet, packet_size, tc);
            path->limit = limit;
            continue;
        }

        if (limit_min > limit) {
            limit_min = limit;
            path_min = path;
        }
    }

    if (!path_min) {
        for (path = mud->path; path; path = path->next) {
            if (path->bak.local) {
                path_min = path;
                break;
            }
        }

        if (!path_min)
            return 0;
    }

    ssize_t ret = mud_send_path(mud, path_min, now, packet, packet_size, tc);

    if (ret == packet_size)
        path_min->limit = limit_min;

    return (int)ret;
}
