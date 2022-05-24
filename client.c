/*
 * Copyright (C) 2020 National Cheng Kung University, Taiwan.
 * Copyright (C) 2011-2012 Roman Arutyunyan (arut@qip.ru)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * htstress - Fast HTTP Benchmarking tool
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

typedef void (*sighandler_t)(int);

#define HTTP_REQUEST_PREFIX "http://"

#define HTTP_REQUEST_FMT "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n"

#define HTTP_REQUEST_DEBUG 0x01
#define HTTP_RESPONSE_DEBUG 0x02

#define INBUFSIZE 1024

#define BAD_REQUEST 0x1

#define MAX_EVENTS 256

struct econn {
    int fd;
    size_t offs;
    int flags;
};

static char *outbuf;
static size_t outbufsize;

static struct sockaddr_storage sss;
static socklen_t sssln = 0;

static int concurrency = 1;
static int num_threads = 1;

static char *udaddr = "";

static volatile _Atomic uint64_t num_requests = 0;
static volatile uint64_t max_requests = 0;
static volatile _Atomic uint64_t good_requests = 0;
static volatile _Atomic uint64_t bad_requests = 0;
static volatile _Atomic uint64_t socket_errors = 0;
static volatile uint64_t in_bytes = 0;
static volatile uint64_t out_bytes = 0;

static uint64_t ticks;

static int debug = 0;
static int exit_i = 0;

static const char short_options[] = "n:c:t:u:h:d46";

static const struct option long_options[] = {
    {"number", 1, NULL, 'n'},  {"concurrency", 1, NULL, 'c'},
    {"threads", 0, NULL, 't'}, {"udaddr", 1, NULL, 'u'},
    {"host", 1, NULL, 'h'},    {"debug", 0, NULL, 'd'},
    {"help", 0, NULL, '%'},    {NULL, 0, NULL, 0}};

static void sigint_handler(int arg)
{
    (void) arg;
    max_requests = num_requests;
}

static void signal_exit(int signal)
{
    (void) signal;
    exit_i++;
}

int main(int argc, char *argv[])
{
    printf("setup sig\n");
    char *host = NULL;
    char *node = NULL;
    char *port = "http";
    struct sockaddr_in *ssin = (struct sockaddr_in *) &sss;
    struct sockaddr_in6 *ssin6 = (struct sockaddr_in6 *) &sss;
    struct sockaddr_un *ssun = (struct sockaddr_un *) &sss;
    struct addrinfo *result, *rp;
    struct addrinfo hints;


    printf("setup sig\n");
    sighandler_t ret_sig = signal(SIGINT, signal_exit);
    if (ret_sig == SIG_ERR) {
        perror("signal(SIGINT, handler)");
        exit(0);
    }

    ret_sig = signal(SIGTERM, signal_exit);
    if (ret_sig == SIG_ERR) {
        perror("signal(SIGTERM, handler)");
        exit(0);
    }

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;

    memset(&sss, 0, sizeof(struct sockaddr_storage));

    printf("parse args\n");
    int next_option;
    do {
        next_option =
            getopt_long(argc, argv, short_options, long_options, NULL);

        switch (next_option) {
        case 'n':
            max_requests = strtoull(optarg, 0, 10);
            break;
        case 'c':
            concurrency = atoi(optarg);
            break;
        case 't':
            num_threads = atoi(optarg);
            break;
        case 'u':
            udaddr = optarg;
            break;
        case 'd':
            debug = 0x03;
            break;
        case 'h':
            host = optarg;
            break;
        case '4':
            hints.ai_family = PF_INET;
            break;
        case '6':
            hints.ai_family = PF_INET6;
            break;
        case -1:
            break;
        default:
            printf("Unexpected argument: '%c'\n", next_option);
            return 1;
        }
    } while (next_option != -1);

    if (optind >= argc) {
        printf("Missing URL\n");
        return 1;
    }

    printf("parse URL\n");
    /* parse URL */
    char *s = argv[optind];
    if (!strncmp(s, HTTP_REQUEST_PREFIX, sizeof(HTTP_REQUEST_PREFIX) - 1))
        s += (sizeof(HTTP_REQUEST_PREFIX) - 1);
    node = s;

    char *rq = strpbrk(s, ":/");
    if (!rq)
        rq = "/";
    else if (*rq == '/') {
        node = strndup(s, rq - s);
        if (!node) {
            perror("node = strndup(s, rq - s)");
            exit(EXIT_FAILURE);
        }
    } else if (*rq == ':') {
        *rq++ = 0;
        port = rq;
        rq = strchr(rq, '/');
        if (rq) {
            if (*rq == '/') {
                port = strndup(port, rq - port);
                if (!port) {
                    perror("port = strndup(rq, rq - port)");
                    exit(EXIT_FAILURE);
                }
            } else
                rq = "/";
        }
    }

    if (strnlen(udaddr, sizeof(ssun->sun_path) - 1) == 0) {
        int j = getaddrinfo(node, port, &hints, &result);
        if (j) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(j));
            exit(EXIT_FAILURE);
        }

        for (rp = result; rp; rp = rp->ai_next) {
            int testfd =
                socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (testfd == -1)
                continue;

            if (connect(testfd, rp->ai_addr, rp->ai_addrlen) == 0) {
                close(testfd);
                break;
            }

            close(testfd);
        }

        if (!rp) { /* No address succeeded */
            fprintf(stderr, "getaddrinfo failed\n");
            exit(EXIT_FAILURE);
        }

        if (rp->ai_addr->sa_family == PF_INET) {
            *ssin = *(struct sockaddr_in *) rp->ai_addr;
        } else if (rp->ai_addr->sa_family == PF_INET6) {
            *ssin6 = *(struct sockaddr_in6 *) rp->ai_addr;
        } else {
            fprintf(stderr, "invalid family %d from getaddrinfo\n",
                    rp->ai_addr->sa_family);
            exit(EXIT_FAILURE);
        }
        sssln = rp->ai_addrlen;

        freeaddrinfo(result);
    } else {
        ssun->sun_family = PF_UNIX;
        strncpy(ssun->sun_path, udaddr, sizeof(ssun->sun_path) - 1);
        sssln = sizeof(struct sockaddr_un);
    }

    /* prepare request buffer */
    if (!host)
        host = node;
    outbufsize = sizeof(HTTP_REQUEST_FMT) + strlen(host);
    outbufsize += rq ? strlen(rq) : 1;

    outbuf = malloc(outbufsize);
    outbufsize =
        snprintf(outbuf, outbufsize, HTTP_REQUEST_FMT, rq ? rq : "/", host);

    ticks = max_requests / 10;

    signal(SIGINT, &sigint_handler);

    //    if (!max_requests) {
    //        ticks = 1000;
    //        printf("[Press Ctrl-C to finish]\n");
    //    }

    printf("init conn\n");
    // init conn
    struct econn *ec = malloc(sizeof(struct econn));

    ec->fd = socket(sss.ss_family, SOCK_STREAM, 0);
    ec->offs = 0;
    ec->flags = 0;

    if (ec->fd == -1) {
        perror("socket() failed");
        exit(1);
    }

    int ret;
    ret = connect(ec->fd, (struct sockaddr *) &sss, sssln);
    if (ret != 0) {
        perror("connect() failed");
        exit(1);
    }

    char inbuf[INBUFSIZE];
    int count = 0;
    for (; count < 20;) {
        sleep(1);
        printf("count: %d\n", ++count);
        memset(inbuf, 0, sizeof(inbuf));
        int bytes_sent = 0;

        //        printf("send..\n");
        do {
            //            printf("%s, size: %zu\n", outbuf, outbufsize);
            //            bytes_sent += send(ec->fd, outbuf + ec->offs,
            //            outbufsize - ec->offs, 0);
            bytes_sent += send(ec->fd, outbuf, outbufsize, 0);
            //            printf("bytes sent: %d\n", bytes_sent);
        } while ((size_t) bytes_sent < outbufsize);

        //        printf("recv..\n");
        //        int bytes_received = 0;
        //        do {
        int bytes_received = recv(ec->fd, inbuf, sizeof(inbuf), 0);
        if (bytes_received < 0) {
            perror("recv() failed");
            exit(1);
        }
        //        printf("bytes recv: %d\n", bytes_received);
        //        } while (bytes_received >0);
    }

    close(ec->fd);
    return 0;
}
