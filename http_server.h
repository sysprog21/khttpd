#ifndef KHTTPD_HTTP_SERVER_H
#define KHTTPD_HTTP_SERVER_H

#include <net/sock.h>

#define RECV_BUFFER_SIZE 4096
extern mempool_t *http_buf_pool;

struct http_server_param {
    struct socket *listen_socket;
};

extern int http_server_daemon(void *arg);

static inline void *http_buf_alloc(gfp_t gfp_mask, void *pool_data)
{
    return kzalloc(RECV_BUFFER_SIZE, gfp_mask);
}

static inline void http_buf_free(void *element, void *pool_data)
{
    kfree(element);
}
#endif
