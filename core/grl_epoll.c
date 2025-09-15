#include <sys/eventfd.h>

#include "grl_coroutine.h"


int grl_epoller_create(void) {
	return epoll_create(1024);
} 

int grl_epoller_wait(struct timespec t) {
	grl_schedule *sched = grl_coroutine_get_sched();
	return epoll_wait(sched->poller_fd, sched->eventlist, GRL_CO_MAX_EVENTS, t.tv_sec*1000.0 + t.tv_nsec/1000000.0);
}

int grl_epoller_ev_register_trigger(void) {
	grl_schedule *sched = grl_coroutine_get_sched();

	if (!sched->eventfd) {
		sched->eventfd = eventfd(0, EFD_NONBLOCK);
		assert(sched->eventfd != -1);
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = sched->eventfd;
	int ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, sched->eventfd, &ev);

	assert(ret != -1);
}


