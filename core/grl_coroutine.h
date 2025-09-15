#ifndef __GRL_COROUTINE_H__
#define __GRL_COROUTINE_H__

#define _GNU_SOURCE
#include "grl_queue.h"
#include "grl_tree.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <unistd.h>

#define GRL_CO_MAX_EVENTS    (1024 * 1024)
#define GRL_CO_MAX_STACKSIZE (128 * 1024) // {http: 16*1024, tcp: 4*1024}

#define BIT(x)      (1 << (x))
#define CLEARBIT(x) ~(1 << (x))

#define CANCEL_FD_WAIT_UINT64 1

typedef void (*proc_coroutine)(void *);

typedef enum {
    GRL_COROUTINE_STATUS_WAIT_READ,
    GRL_COROUTINE_STATUS_WAIT_WRITE,
    GRL_COROUTINE_STATUS_NEW,
    GRL_COROUTINE_STATUS_READY,
    GRL_COROUTINE_STATUS_EXITED,
    GRL_COROUTINE_STATUS_BUSY,
    GRL_COROUTINE_STATUS_SLEEPING,
    GRL_COROUTINE_STATUS_EXPIRED,
    GRL_COROUTINE_STATUS_FDEOF,
    GRL_COROUTINE_STATUS_DETACH,
    GRL_COROUTINE_STATUS_CANCELLED,
    GRL_COROUTINE_STATUS_PENDING_RUNCOMPUTE,
    GRL_COROUTINE_STATUS_RUNCOMPUTE,
    GRL_COROUTINE_STATUS_WAIT_IO_READ,
    GRL_COROUTINE_STATUS_WAIT_IO_WRITE,
    GRL_COROUTINE_STATUS_WAIT_MULTI
} grl_coroutine_status;

typedef enum {
    GRL_COROUTINE_COMPUTE_BUSY,
    GRL_COROUTINE_COMPUTE_FREE
} grl_coroutine_compute_status;

typedef enum {
    GRL_COROUTINE_EV_READ,
    GRL_COROUTINE_EV_WRITE
} grl_coroutine_event;

LIST_HEAD(_grl_coroutine_link, _grl_coroutine);
TAILQ_HEAD(_grl_coroutine_queue, _grl_coroutine);

RB_HEAD(_grl_coroutine_rbtree_sleep, _grl_coroutine);
RB_HEAD(_grl_coroutine_rbtree_wait, _grl_coroutine);

typedef struct _grl_coroutine_link grl_coroutine_link;
typedef struct _grl_coroutine_queue grl_coroutine_queue;

typedef struct _grl_coroutine_rbtree_sleep grl_coroutine_rbtree_sleep;
typedef struct _grl_coroutine_rbtree_wait grl_coroutine_rbtree_wait;

typedef struct _grl_cpu_ctx {
    void *esp; //
    void *ebp;
    void *eip;
    void *edi;
    void *esi;
    void *ebx;
    void *r1;
    void *r2;
    void *r3;
    void *r4;
    void *r5;
} grl_cpu_ctx;

///
typedef struct _grl_schedule {
    uint64_t birth;
    grl_cpu_ctx ctx;
    void *stack;
    size_t stack_size;
    int spawned_coroutines;
    uint64_t default_timeout;
    struct _grl_coroutine *curr_thread;
    int page_size;

    int poller_fd;
    int eventfd;
    struct epoll_event eventlist[GRL_CO_MAX_EVENTS];
    int nevents;

    int num_new_events;
    pthread_mutex_t defer_mutex;

    grl_coroutine_queue ready;
    grl_coroutine_queue defer;

    grl_coroutine_link busy;

    grl_coroutine_rbtree_sleep sleeping;
    grl_coroutine_rbtree_wait waiting;

    // private

} grl_schedule;

typedef struct _grl_coroutine {
    // private

    grl_cpu_ctx ctx;
    proc_coroutine func;
    void *arg;
    void *data;
    size_t stack_size;
    size_t last_stack_size;

    grl_coroutine_status status;
    grl_schedule *sched;

    uint64_t birth;
    uint64_t id;
#if CANCEL_FD_WAIT_UINT64
    int fd;
    unsigned short events; // POLL_EVENT
#else
    int64_t fd_wait;
#endif
    char funcname[64];
    struct _grl_coroutine *co_join;

    void **co_exit_ptr;
    void *stack;
    void *ebp;
    uint32_t ops;
    uint64_t sleep_usecs;

    RB_ENTRY(_grl_coroutine) sleep_node;
    RB_ENTRY(_grl_coroutine) wait_node;

    LIST_ENTRY(_grl_coroutine) busy_next;

    TAILQ_ENTRY(_grl_coroutine) ready_next;
    TAILQ_ENTRY(_grl_coroutine) defer_next;
    TAILQ_ENTRY(_grl_coroutine) cond_next;

    TAILQ_ENTRY(_grl_coroutine) io_next;
    TAILQ_ENTRY(_grl_coroutine) compute_next;

    struct {
        void *buf;
        size_t nbytes;
        int fd;
        int ret;
        int err;
    } io;

    struct _grl_coroutine_compute_sched *compute_sched;
    int ready_fds;
    struct pollfd *pfds;
    nfds_t nfds;
} grl_coroutine;

typedef struct _grl_coroutine_compute_sched {
    grl_cpu_ctx ctx;
    grl_coroutine_queue coroutines;

    grl_coroutine *curr_coroutine;

    pthread_mutex_t run_mutex;
    pthread_cond_t run_cond;

    pthread_mutex_t co_mutex;
    LIST_ENTRY(_grl_coroutine_compute_sched) compute_next;

    grl_coroutine_compute_status compute_status;
} grl_coroutine_compute_sched;

extern pthread_key_t global_sched_key;

static inline grl_schedule *grl_coroutine_get_sched(void) {
    return pthread_getspecific(global_sched_key);
}

static inline uint64_t grl_coroutine_diff_usecs(uint64_t t1, uint64_t t2) {
    return t2 - t1;
}

static inline uint64_t grl_coroutine_usec_now(void) {
    struct timeval t1 = {0, 0};
    gettimeofday(&t1, NULL);

    return t1.tv_sec * 1000000 + t1.tv_usec;
}

int grl_epoller_create(void);

void grl_schedule_cancel_event(grl_coroutine *co);
void grl_schedule_sched_event(grl_coroutine *co, int fd, grl_coroutine_event e,
                              uint64_t timeout);

void grl_schedule_desched_sleepdown(grl_coroutine *co);
void grl_schedule_sched_sleepdown(grl_coroutine *co, uint64_t msecs);

grl_coroutine *grl_schedule_desched_wait(int fd);
void grl_schedule_sched_wait(grl_coroutine *co, int fd, unsigned short events,
                             uint64_t timeout);

void grl_schedule_run(void);

int grl_epoller_ev_register_trigger(void);
int grl_epoller_wait(struct timespec t);
int grl_coroutine_resume(grl_coroutine *co);
void grl_coroutine_free(grl_coroutine *co);
int grl_coroutine_create(grl_coroutine **new_co, proc_coroutine func,
                         void *arg);
void grl_coroutine_yield(grl_coroutine *co);

void grl_coroutine_sleep(uint64_t msecs);

int grl_socket(int domain, int type, int protocol);
int grl_accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t grl_recv(int fd, void *buf, size_t len, int flags);
ssize_t grl_send(int fd, void const *buf, size_t len, int flags);
int grl_close(int fd);
int grl_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int grl_connect(int fd, struct sockaddr *name, socklen_t namelen);

ssize_t grl_sendto(int fd, void const *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t grl_recvfrom(int fd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);

#define COROUTINE_HOOK

#ifdef COROUTINE_HOOK

typedef int (*socket_t)(int domain, int type, int protocol);
extern socket_t socket_f;

typedef int (*connect_t)(int, const struct sockaddr *, socklen_t);
extern connect_t connect_f;

typedef ssize_t (*read_t)(int, void *, size_t);
extern read_t read_f;

typedef ssize_t (*recv_t)(int sockfd, void *buf, size_t len, int flags);
extern recv_t recv_f;

typedef ssize_t (*recvfrom_t)(int sockfd, void *buf, size_t len, int flags,
                              struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_t recvfrom_f;

typedef ssize_t (*write_t)(int, void const *, size_t);
extern write_t write_f;

typedef ssize_t (*send_t)(int sockfd, void const *buf, size_t len, int flags);
extern send_t send_f;

typedef ssize_t (*sendto_t)(int sockfd, void const *buf, size_t len, int flags,
                            const struct sockaddr *dest_addr,
                            socklen_t addrlen);
extern sendto_t sendto_f;

typedef int (*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern accept_t accept_f;

// new-syscall
typedef int (*close_t)(int);
extern close_t close_f;

int init_hook(void);

/*

typedef int(*fcntl_t)(int __fd, int __cmd, ...);
extern fcntl_t fcntl_f;

typedef int (*getsockopt_t)(int sockfd, int level, int optname,
        void *optval, socklen_t *optlen);
extern getsockopt_t getsockopt_f;

typedef int (*setsockopt_t)(int sockfd, int level, int optname,
        const void *optval, socklen_t optlen);
extern setsockopt_t setsockopt_f;

*/

#endif

#endif
