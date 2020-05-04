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
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
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

#define HTTP_REQUEST_FMT "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n"

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

static volatile uint64_t num_requests = 0;
static volatile uint64_t max_requests = 0;
static volatile uint64_t good_requests = 0;
static volatile uint64_t bad_requests = 0;
static volatile uint64_t socket_errors = 0;
static volatile uint64_t in_bytes = 0;
static volatile uint64_t out_bytes = 0;

static uint64_t ticks;

static int debug = 0;
static int exit_i = 0;

static struct timeval tv, tve;

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

static void start_time()
{
    if (gettimeofday(&tv, NULL)) {
        perror("gettimeofday");
        exit(1);
    }
}

static void end_time()
{
    if (gettimeofday(&tve, NULL)) {
        perror("gettimeofday");
        exit(1);
    }
}

static void init_conn(int efd, struct econn *ec)
{
    int ret;

    ec->fd = socket(sss.ss_family, SOCK_STREAM, 0);
    ec->offs = 0;
    ec->flags = 0;

    if (ec->fd == -1) {
        perror("socket() failed");
        exit(1);
    }

    fcntl(ec->fd, F_SETFL, O_NONBLOCK);

    do {
        ret = connect(ec->fd, (struct sockaddr *) &sss, sssln);
    } while (ret && errno == EAGAIN);

    if (ret && errno != EINPROGRESS) {
        perror("connect() failed");
        exit(1);
    }

    struct epoll_event evt = {
        .events = EPOLLOUT, .data.ptr = ec,
    };

    if (epoll_ctl(efd, EPOLL_CTL_ADD, ec->fd, &evt)) {
        perror("epoll_ctl");
        exit(1);
    }
}

static void *worker(void *arg)
{
    int ret, nevts;
    struct epoll_event evts[MAX_EVENTS];
    char inbuf[INBUFSIZE];
    struct econn ecs[concurrency], *ec;

    (void) arg;

    int efd = epoll_create(concurrency);
    if (efd == -1) {
        perror("epoll");
        exit(1);
    }

    for (int n = 0; n < concurrency; ++n)
        init_conn(efd, ecs + n);

    for (;;) {
        do {
            nevts = epoll_wait(efd, evts, sizeof(evts) / sizeof(evts[0]), -1);
        } while (!exit_i && nevts < 0 && errno == EINTR);

        if (exit_i != 0) {
            exit(0);
        }

        if (nevts == -1) {
            perror("epoll_wait");
            exit(1);
        }

        for (int n = 0; n < nevts; ++n) {
            ec = (struct econn *) evts[n].data.ptr;

            if (!ec) {
                fprintf(stderr, "fatal: NULL econn\n");
                exit(1);
            }

            if (evts[n].events & EPOLLERR) {
                /* normally this should not happen */
                static unsigned int number_of_errors_logged = 0;
                int error = 0;
                socklen_t errlen = sizeof(error);
                number_of_errors_logged += 1;
                if (getsockopt(efd, SOL_SOCKET, SO_ERROR, (void *) &error,
                               &errlen) == 0) {
                    fprintf(stderr, "error = %s\n", strerror(error));
                }
                if (number_of_errors_logged % 100 == 0) {
                    fprintf(stderr, "EPOLLERR caused by unknown error\n");
                }
                __sync_fetch_and_add(&socket_errors, 1);
                close(ec->fd);
                if (num_requests > max_requests)
                    continue;
                init_conn(efd, ec);
                continue;
            }

            if (evts[n].events & EPOLLHUP) {
                /* This can happen for HTTP/1.0 */
                fprintf(stderr, "EPOLLHUP\n");
                exit(1);
            }

            if (evts[n].events & EPOLLOUT) {
                ret = send(ec->fd, outbuf + ec->offs, outbufsize - ec->offs, 0);

                if (ret == -1 && errno != EAGAIN) {
                    /* TODO: something better than this */
                    perror("send");
                    exit(1);
                }

                if (ret > 0) {
                    if (debug & HTTP_REQUEST_DEBUG)
                        write(2, outbuf + ec->offs, outbufsize - ec->offs);

                    ec->offs += ret;

                    /* write done? schedule read */
                    if (ec->offs == outbufsize) {
                        evts[n].events = EPOLLIN;
                        evts[n].data.ptr = ec;

                        ec->offs = 0;

                        if (epoll_ctl(efd, EPOLL_CTL_MOD, ec->fd, evts + n)) {
                            perror("epoll_ctl");
                            exit(1);
                        }
                    }
                }

            } else if (evts[n].events & EPOLLIN) {
                for (;;) {
                    ret = recv(ec->fd, inbuf, sizeof(inbuf), 0);

                    if (ret == -1 && errno != EAGAIN) {
                        perror("recv");
                        exit(1);
                    }

                    if (ret <= 0)
                        break;

                    if (ec->offs <= 9 && ec->offs + ret > 10) {
                        char c = inbuf[9 - ec->offs];
                        if (c == '4' || c == '5')
                            ec->flags |= BAD_REQUEST;
                    }

                    if (debug & HTTP_RESPONSE_DEBUG)
                        write(2, inbuf, ret);

                    ec->offs += ret;
                }

                if (!ret) {
                    close(ec->fd);

                    int m = __sync_fetch_and_add(&num_requests, 1);

                    if (max_requests && (m + 1 > (int) max_requests))
                        __sync_fetch_and_sub(&num_requests, 1);
                    else if (ec->flags & BAD_REQUEST)
                        __sync_fetch_and_add(&bad_requests, 1);
                    else
                        __sync_fetch_and_add(&good_requests, 1);

                    if (max_requests && (m + 1 >= (int) max_requests)) {
                        end_time();
                        return NULL;
                    }

                    if (ticks && m % ticks == 0)
                        printf("%d requests\n", m);

                    init_conn(efd, ec);
                }
            }
        }
    }
}

static void signal_exit(int signal)
{
    (void) signal;
    exit_i++;
}

static void print_usage()
{
    printf(
        "Usage: htstress [options] [http://]hostname[:port]/path\n"
        "Options:\n"
        "   -n, --number       total number of requests (0 for inifinite, "
        "Ctrl-C to abort)\n"
        "   -c, --concurrency  number of concurrent connections\n"
        "   -t, --threads      number of threads (set this to the number of "
        "CPU cores)\n"
        "   -u, --udaddr       path to unix domain socket\n"
        "   -h, --host         host to use for http request\n"
        "   -d, --debug        debug HTTP response\n"
        "   --help             display this message\n");
    exit(0);
}

int main(int argc, char *argv[])
{
    pthread_t useless_thread;
    char *host = NULL;
    char *node = NULL;
    char *port = "http";
    struct sockaddr_in *ssin = (struct sockaddr_in *) &sss;
    struct sockaddr_in6 *ssin6 = (struct sockaddr_in6 *) &sss;
    struct sockaddr_un *ssun = (struct sockaddr_un *) &sss;
    struct addrinfo *result, *rp;
    struct addrinfo hints;

    sighandler_t ret = signal(SIGINT, signal_exit);
    if (ret == SIG_ERR) {
        perror("signal(SIGINT, handler)");
        exit(0);
    }

    ret = signal(SIGTERM, signal_exit);
    if (ret == SIG_ERR) {
        perror("signal(SIGTERM, handler)");
        exit(0);
    }

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;

    memset(&sss, 0, sizeof(struct sockaddr_storage));

    if (argc == 1)
        print_usage();

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
        case '%':
            print_usage();
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
    outbufsize = rq ? snprintf(outbuf, outbufsize, HTTP_REQUEST_FMT, rq, host)
                    : snprintf(outbuf, outbufsize, HTTP_REQUEST_FMT, "/", host);

    ticks = max_requests / 10;

    signal(SIGINT, &sigint_handler);

    if (!max_requests) {
        ticks = 1000;
        printf("[Press Ctrl-C to finish]\n");
    }

    start_time();

    /* run test */
    for (int n = 0; n < num_threads - 1; ++n)
        pthread_create(&useless_thread, 0, &worker, 0);

    worker(0);

    /* output result */
    double delta =
        tve.tv_sec - tv.tv_sec + ((double) (tve.tv_usec - tv.tv_usec)) / 1e6;

    printf(
        "\n"
        "requests:      %" PRIu64
        "\n"
        "good requests: %" PRIu64
        " [%d%%]\n"
        "bad requests:  %" PRIu64
        " [%d%%]\n"
        "socker errors: %" PRIu64
        " [%d%%]\n"
        "seconds:       %.3f\n"
        "requests/sec:  %.3f\n"
        "\n",
        num_requests, good_requests,
        (int) (num_requests ? good_requests * 100 / num_requests : 0),
        bad_requests,
        (int) (num_requests ? bad_requests * 100 / num_requests : 0),
        socket_errors,
        (int) (num_requests ? socket_errors * 100 / num_requests : 0), delta,
        delta > 0 ? max_requests / delta : 0);

    return 0;
}
