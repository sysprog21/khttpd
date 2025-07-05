#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kthread.h>
#include <linux/mempool.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/version.h>
#include <net/sock.h>

#include "http_server.h"

#define DEFAULT_PORT 8081
#define DEFAULT_BACKLOG 100
#define POOL_MIN_NR 4

mempool_t *http_buf_pool;

static ushort port = DEFAULT_PORT;
module_param(port, ushort, S_IRUGO);
static ushort backlog = DEFAULT_BACKLOG;
module_param(backlog, ushort, S_IRUGO);

static struct socket *listen_socket;
static struct http_server_param param;
static struct task_struct *http_server;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
static int set_sock_opt(struct socket *sock,
                        int level,
                        int optname,
                        char *optval,
                        unsigned int optlen)
{
    int ret = 0;

    if (optlen < sizeof(int))
        return -EINVAL;

    switch (optname) {
    case SO_REUSEADDR:
        sock_set_reuseaddr(sock->sk);
        break;
    case SO_RCVBUF:
        sock_set_rcvbuf(sock->sk, *(int *) optval);
        break;
    }

    return ret;
}

static int set_tcp_opt(struct socket *sock,
                       int level,
                       int optname,
                       char *optval,
                       unsigned int optlen)
{
    int ret = 0;

    if (optlen < sizeof(int))
        return -EINVAL;

    switch (optname) {
    case TCP_NODELAY:
        tcp_sock_set_nodelay(sock->sk);
        break;
    case TCP_CORK:
        tcp_sock_set_cork(sock->sk, *(bool *) optval);
        break;
    }

    return ret;
}

static int kernel_setsockopt(struct socket *sock,
                             int level,
                             int optname,
                             char *optval,
                             unsigned int optlen)
{
    if (level == SOL_SOCKET)
        return set_sock_opt(sock, level, optname, optval, optlen);
    else if (level == SOL_TCP)
        return set_tcp_opt(sock, level, optname, optval, optlen);
    return -EINVAL;
}
#endif

static inline int setsockopt(struct socket *sock,
                             int level,
                             int optname,
                             int optval)
{
    int opt = optval;
    return kernel_setsockopt(sock, level, optname, (char *) &opt, sizeof(opt));
}

static int open_listen_socket(ushort port, ushort backlog, struct socket **res)
{
    struct socket *sock;
    struct sockaddr_in s;

    int err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (err < 0) {
        pr_err("sock_create() failure, err=%d\n", err);
        return err;
    }

    err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_TCP, TCP_NODELAY, 1);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_TCP, TCP_CORK, 0);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 1024 * 1024);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, 1024 * 1024);
    if (err < 0)
        goto bail_setsockopt;

    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_ANY);
    s.sin_port = htons(port);
    err = kernel_bind(sock, (struct sockaddr *) &s, sizeof(s));
    if (err < 0) {
        pr_err("kernel_bind() failure, err=%d\n", err);
        goto bail_sock;
    }

    err = kernel_listen(sock, backlog);
    if (err < 0) {
        pr_err("kernel_listen() failure, err=%d\n", err);
        goto bail_sock;
    }
    *res = sock;
    return 0;

bail_setsockopt:
    pr_err("kernel_setsockopt() failure, err=%d\n", err);
bail_sock:
    sock_release(sock);
    return err;
}

static void close_listen_socket(struct socket *socket)
{
    kernel_sock_shutdown(socket, SHUT_RDWR);
    sock_release(socket);
}

static int __init khttpd_init(void)
{
    if (!(http_buf_pool = mempool_create(POOL_MIN_NR, http_buf_alloc,
                                         http_buf_free, NULL))) {
        pr_err("failed to create mempool\n");
        return -ENOMEM;
    }
    int err = open_listen_socket(port, backlog, &listen_socket);
    if (err < 0) {
        pr_err("can't open listen socket\n");
        return err;
    }
    param.listen_socket = listen_socket;
    http_server = kthread_run(http_server_daemon, &param, KBUILD_MODNAME);
    if (IS_ERR(http_server)) {
        pr_err("can't start http server daemon\n");
        close_listen_socket(listen_socket);
        return PTR_ERR(http_server);
    }
    return 0;
}

static void __exit khttpd_exit(void)
{
    send_sig(SIGTERM, http_server, 1);
    kthread_stop(http_server);
    close_listen_socket(listen_socket);
    pr_info("module unloaded\n");
}

module_init(khttpd_init);
module_exit(khttpd_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("in-kernel HTTP daemon");
MODULE_VERSION("0.1");
