#ifndef KHTTPD_HTTP_SERVER_H
#define KHTTPD_HTTP_SERVER_H

#include <linux/list.h>
#include <linux/workqueue.h>
#include <net/sock.h>

#define MODULE_NAME "khttpd"

struct http_server_param {
    struct socket *listen_socket;
};

struct httpd_service {
    bool is_stopped;       /* 是否收到終止訊號 */
    struct list_head head; /* 串列首節點 */
};

extern int http_server_daemon(void *arg);

#endif
