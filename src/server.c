#define _GNU_SOURCE
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "affinity.h"
#include "feed.h"
#include "timing.h"

#define MAX_CONNS         16
#define EPOLL_BATCH       64
#define EPOLL_TIMEOUT_MS  100
#define CONN_BUF_BYTES    (FEED_MSG_SIZE * 256)  /* 16 KiB, a few pages */

/* Per-connection read-side buffer. Frames are fixed 64 bytes, but TCP
 * gives us arbitrary byte runs, so we batch into buf[] and drain whole
 * frames out of the front. */
typedef struct {
    int      fd;
    uint8_t  buf[CONN_BUF_BYTES];
    size_t   buf_len;
} conn_t;

static void conn_init(conn_t *c) {
    c->fd = -1;
    c->buf_len = 0;
}

static void conn_close(conn_t *c) {
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->buf_len = 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listener(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int one = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        close(s);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    if (listen(s, 32) < 0) {
        close(s);
        return -1;
    }
    if (set_nonblocking(s) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static conn_t *find_free_slot(conn_t *conns) {
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (conns[i].fd < 0) return &conns[i];
    }
    return NULL;
}

static void dispatch_frame(const server_config_t *cfg, feed_msg_t *m) {
    /* Sample rdtsc first so the ingress stamp is as close to the wire
     * as we can get, then validate against the producer's checksum
     * (computed with ingress_tsc==0), then write the stamp in. */
    uint64_t ingress = rdtsc_now();

    if (feed_validate(m) != 0) {
        atomic_fetch_add_explicit(cfg->bad_frames, 1, memory_order_relaxed);
        return;
    }
    m->ingress_tsc = ingress;

    size_t w = (size_t)(m->symbol_id) % cfg->num_workers;
    if (ring_try_push(&cfg->worker_rings[w], m) != 0) {
        atomic_fetch_add_explicit(cfg->msgs_dropped, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(cfg->msgs_received, 1, memory_order_relaxed);
}

static void drain_conn(const server_config_t *cfg, conn_t *c) {
    for (;;) {
        ssize_t space = (ssize_t)(sizeof(c->buf) - c->buf_len);
        if (space <= 0) {
            /* Should not happen as long as we drain frames eagerly,
             * but guard against a runaway producer. */
            c->buf_len = 0;
            space = (ssize_t)sizeof(c->buf);
        }

        ssize_t n = recv(c->fd, c->buf + c->buf_len, (size_t)space, 0);
        if (n > 0) {
            c->buf_len += (size_t)n;
        } else if (n == 0) {
            /* peer closed */
            conn_close(c);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            conn_close(c);
            return;
        }

        /* Drain complete frames. */
        while (c->buf_len >= FEED_MSG_SIZE) {
            feed_msg_t msg;
            memcpy(&msg, c->buf, FEED_MSG_SIZE);
            dispatch_frame(cfg, &msg);

            size_t left = c->buf_len - FEED_MSG_SIZE;
            if (left > 0) {
                memmove(c->buf, c->buf + FEED_MSG_SIZE, left);
            }
            c->buf_len = left;
        }
    }
}

void *server_thread_main(void *arg) {
    server_config_t *cfg = (server_config_t *)arg;

    if (cfg->cpu >= 0) {
        int rc = affinity_set_self(cfg->cpu);
        if (rc != 0) {
            fprintf(stderr, "server: affinity set to cpu %d failed (rc=%d)\n",
                    cfg->cpu, rc);
        }
    }

    int listen_fd = create_listener(cfg->port);
    if (listen_fd < 0) {
        fprintf(stderr, "server: listen on port %d failed: %s\n",
                cfg->port, strerror(errno));
        return NULL;
    }

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) {
        close(listen_fd);
        return NULL;
    }

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.fd  = listen_fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        close(ep);
        close(listen_fd);
        return NULL;
    }

    conn_t conns[MAX_CONNS];
    for (int i = 0; i < MAX_CONNS; ++i) conn_init(&conns[i]);

    printf("server: listening on :%d, %zu workers\n",
           cfg->port, cfg->num_workers);

    struct epoll_event events[EPOLL_BATCH];
    while (!*cfg->stop_flag) {
        int n = epoll_wait(ep, events, EPOLL_BATCH, EPOLL_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                for (;;) {
                    int c = accept(listen_fd, NULL, NULL);
                    if (c < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (set_nonblocking(c) < 0) { close(c); continue; }
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    conn_t *slot = find_free_slot(conns);
                    if (!slot) {
                        close(c);
                        continue;
                    }
                    slot->fd      = c;
                    slot->buf_len = 0;

                    struct epoll_event cev;
                    cev.events  = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = c;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, c, &cev) < 0) {
                        conn_close(slot);
                    }
                }
                continue;
            }

            /* Client fd. */
            conn_t *c = NULL;
            for (int k = 0; k < MAX_CONNS; ++k) {
                if (conns[k].fd == fd) { c = &conns[k]; break; }
            }
            if (!c) {
                /* Unknown fd - drop it from epoll and close. */
                epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }

            if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) {
                drain_conn(cfg, c);
            }
        }
    }

    for (int i = 0; i < MAX_CONNS; ++i) conn_close(&conns[i]);
    close(ep);
    close(listen_fd);
    printf("server: stopped\n");
    return NULL;
}
