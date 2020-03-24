#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>
#include <net/sock.h>

#include "common.h"

#define DEFAULT_PORT 8081
#define DEFAULT_BACKLOG 100

static ushort port = DEFAULT_PORT;
module_param(port, ushort, S_IRUGO);
static ushort backlog = DEFAULT_BACKLOG;
module_param(backlog, ushort, S_IRUGO);

static struct socket *listen_socket;
static struct http_server_param param;
static struct task_struct *http_server;

static int open_listen_socket(ushort port, ushort backlog, struct socket **res)
{
    struct socket *sock;
    int err, opt = 1;
    struct sockaddr_in s;

    err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": sock_create() failure, err=%d\n", err);
        return err;
    }
    opt = 1;
    err = kernel_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &opt,
                            sizeof(opt));
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_setsockopt() failure, err=%d\n",
               err);
        sock_release(sock);
        return err;
    }
    opt = 1;
    err = kernel_setsockopt(sock, SOL_TCP, TCP_NODELAY, (char *) &opt,
                            sizeof(opt));
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_setsockopt() failure, err=%d\n",
               err);
        sock_release(sock);
        return err;
    }
    opt = 0;
    err =
        kernel_setsockopt(sock, SOL_TCP, TCP_CORK, (char *) &opt, sizeof(opt));
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_setsockopt() failure, err=%d\n",
               err);
        sock_release(sock);
        return err;
    }
    opt = 1024 * 1024;
    err = kernel_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *) &opt,
                            sizeof(opt));
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_setsockopt() failure, err=%d\n",
               err);
        sock_release(sock);
        return err;
    }
    opt = 1024 * 1024;
    err = kernel_setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *) &opt,
                            sizeof(opt));
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_setsockopt() failure, err=%d\n",
               err);
        sock_release(sock);
        return err;
    }
    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_ANY);
    s.sin_port = htons(port);
    err = kernel_bind(sock, (struct sockaddr *) &s, sizeof(s));
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_bind() failure, err=%d\n", err);
        sock_release(sock);
        return err;
    }
    err = kernel_listen(sock, backlog);
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": kernel_listen() failure, err=%d\n", err);
        sock_release(sock);
        return err;
    }
    *res = sock;
    return 0;
}

static void close_listen_socket(struct socket *socket)
{
    kernel_sock_shutdown(socket, SHUT_RDWR);
    sock_release(socket);
}

static int __init khttpd_init(void)
{
    int err = open_listen_socket(port, backlog, &listen_socket);
    if (err < 0) {
        printk(KERN_ERR MODULE_NAME ": can't open listen socket\n");
        return err;
    }
    param.listen_socket = listen_socket;
    http_server = kthread_run(http_server_daemon, &param, MODULE_NAME);
    if (IS_ERR(http_server)) {
        printk(KERN_ERR MODULE_NAME ": can't start http server daemon\n");
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
    printk(KERN_INFO MODULE_NAME ": module unloaded\n");
}

module_init(khttpd_init);
module_exit(khttpd_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("in-kernel HTTP daemon");
MODULE_VERSION("0.1");
