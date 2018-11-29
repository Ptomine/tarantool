/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "evio.h"
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <trivia/util.h>
#include "exception.h"

void
evio_close(ev_loop *loop, struct ev_io *evio)
{
	/* Stop I/O events. Safe to do even if not started. */
	ev_io_stop(loop, evio);
	if (evio_has_fd(evio)) {
		close(evio->fd);
		ev_io_set(evio, -1, 0);
	}
}

int
evio_socket(struct ev_io *evio, int domain, int type, int protocol)
{
	assert(! evio_has_fd(evio));
	evio->fd = sio_socket(domain, type, protocol);
	if (evio->fd < 0)
		return -1;
	if (evio_setsockopt_client(evio->fd, domain, type) != 0) {
		close(evio->fd);
		ev_io_set(evio, -1, 0);
		return -1;
	}
	return 0;
}

static int
evio_setsockopt_keeping(int fd)
{
	int on = 1;
	/*
	 * SO_KEEPALIVE to ensure connections don't hang
	 * around for too long when a link goes away.
	 */
	if (sio_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0)
		return -1;
#ifdef __linux__
	/*
	 * On Linux, we are able to fine-tune keepalive
	 * intervals. Set smaller defaults, since the system-wide
	 * defaults are in days.
	 */
	int keepcnt = 5;
	if (sio_setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt,
			   sizeof(int)) != 0)
		return -1;

	int keepidle = 30;
	if (sio_setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle,
			   sizeof(int)) != 0)
		return -1;

	int keepintvl = 60;
	if (sio_setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl,
			   sizeof(int)) != 0)
		return -1;
#endif
	return 0;
}

int
evio_setsockopt_client(int fd, int family, int type)
{
	if (sio_setfl(fd, O_NONBLOCK, true) != 0)
		return -1;
	if (type == SOCK_STREAM && family != AF_UNIX) {
		if (evio_setsockopt_keeping(fd) != 0)
			return -1;
		/*
		 * Lower latency is more important than higher
		 * bandwidth, and we usually write entire
		 * request/response in a single syscall.
		 */
		int on = 1;
		if (sio_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
				   &on, sizeof(on)) != 0)
			return -1;
	}
	return 0;
}

/** Set options for server sockets. */
static int
evio_setsockopt_server(int fd, int family, int type)
{
	if (sio_setfl(fd, O_NONBLOCK, true) != 0)
		return -1;
	int on = 1;
	if (sio_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0)
		return -1;

	/*
	 * Send all buffered messages on socket before take
	 * control out from close() or shutdown().
	 */
	struct linger linger = { 0, 0 };
	if (sio_setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger,
			   sizeof(linger)) != 0)
		return -1;
	if (type == SOCK_STREAM && family != AF_UNIX &&
	    evio_setsockopt_keeping(fd) != 0)
		return -1;
	return 0;
}

/**
 * A callback invoked by libev when acceptor socket is ready.
 * Accept the socket, initialize it and pass to the on_accept
 * callback.
 */
static void
evio_service_accept_cb(ev_loop *loop, ev_io *watcher, int events)
{
	(void) loop;
	(void) events;
	struct evio_service *service = (struct evio_service *) watcher->data;
	int fd;
	while (1) {
		/*
		 * Accept all pending connections from backlog
		 * during event loop iteration. Significally
		 * speed up acceptor with enabled
		 * io_collect_interval.
		 */
		struct sockaddr_storage addr;
		socklen_t addrlen = sizeof(addr);
		bool is_error_critical;
		fd = sio_accept(service->ev.fd, (struct sockaddr *)&addr,
				&addrlen, &is_error_critical);
		if (fd < 0) {
			if (is_error_critical)
				break;
			return;
		}
		if (evio_setsockopt_client(fd, service->addr.sa_family,
					   SOCK_STREAM) != 0)
			break;
		if (service->on_accept(service, fd, (struct sockaddr *)&addr,
				       addrlen) != 0)
			break;
	}
	diag_log();
	if (fd >= 0)
		close(fd);
}

/*
 * Check if the UNIX socket which we failed to create exists and
 * no one is listening on it. Unlink the file if it's the case.
 * Set an error and return -1 otherwise.
 */
static int
evio_service_reuse_addr(struct evio_service *service, int fd)
{
	if ((service->addr.sa_family != AF_UNIX) || (errno != EADDRINUSE)) {
		diag_set(SocketError, sio_socketname(fd),
			 "evio_service_reuse_addr");
		return -1;
	}
	int save_errno = errno;
	int cl_fd = sio_socket(service->addr.sa_family, SOCK_STREAM, 0);
	if (cl_fd < 0)
		return -1;

	if (connect(cl_fd, &service->addr, service->addr_len) == 0)
		goto err;

	if (errno != ECONNREFUSED)
		goto err;

	if (unlink(((struct sockaddr_un *)(&service->addr))->sun_path) != 0)
		goto err;
	close(cl_fd);

	return 0;
err:
	errno = save_errno;
	close(cl_fd);
	diag_set(SocketError, sio_socketname(fd), "unlink");
	return -1;
}

/** Try to bind on the configured port. */
static int
evio_service_bind_addr(struct evio_service *service)
{
	say_debug("%s: binding to %s...", service->name,
		  sio_strfaddr(&service->addr, service->addr_len));
	/* Create a socket. */
	int fd = sio_socket(service->addr.sa_family, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0)
		return -1;
	if (evio_setsockopt_server(fd, service->addr.sa_family,
				   SOCK_STREAM) != 0)
		goto error;

	if (sio_bind(fd, &service->addr, service->addr_len)) {
		if (errno != EADDRINUSE)
			goto error;
		if (evio_service_reuse_addr(service, fd))
			goto error;
		if (sio_bind(fd, &service->addr, service->addr_len)) {
			if (errno == EADDRINUSE) {
				diag_set(SocketError, sio_socketname(fd),
					 "bind");
			}
			goto error;
		}
	}

	say_info("%s: bound to %s", service->name,
		 sio_strfaddr(&service->addr, service->addr_len));

	/* Register the socket in the event loop. */
	ev_io_set(&service->ev, fd, EV_READ);
	return 0;
error:
	close(fd);
	return -1;
}

int
evio_service_listen(struct evio_service *service)
{
	say_debug("%s: listening on %s...", service->name,
		  sio_strfaddr(&service->addr, service->addr_len));

	int fd = service->ev.fd;
	if (sio_listen(fd) < 0)
		return -1;
	ev_io_start(service->loop, &service->ev);
	return 0;
}

void
evio_service_init(ev_loop *loop, struct evio_service *service, const char *name,
		  evio_accept_f on_accept, void *on_accept_param)
{
	memset(service, 0, sizeof(struct evio_service));
	snprintf(service->name, sizeof(service->name), "%s", name);

	service->loop = loop;

	service->on_accept = on_accept;
	service->on_accept_param = on_accept_param;
	/*
	 * Initialize libev objects to be able to detect if they
	 * are active or not in evio_service_stop().
	 */
	ev_init(&service->ev, evio_service_accept_cb);
	ev_io_set(&service->ev, -1, 0);
	service->ev.data = service;
}

int
evio_service_bind(struct evio_service *service, const char *uri)
{
	struct uri u;
	if (uri_parse(&u, uri) || u.service == NULL) {
		diag_set(SocketError, sio_socketname(-1),
			 "invalid uri for bind: %s", uri);
		return -1;
	}

	snprintf(service->serv, sizeof(service->serv), "%.*s",
		 (int) u.service_len, u.service);
	if (u.host != NULL && strncmp(u.host, "*", u.host_len) != 0) {
		snprintf(service->host, sizeof(service->host), "%.*s",
			 (int) u.host_len, u.host);
	}

	assert(! ev_is_active(&service->ev));

	if (strcmp(service->host, URI_HOST_UNIX) == 0) {
		struct sockaddr_un *un = (struct sockaddr_un *) &service->addr;
		service->addr_len = sizeof(*un);
		snprintf(un->sun_path, sizeof(un->sun_path), "%s",
			 service->serv);
		un->sun_family = AF_UNIX;
		return evio_service_bind_addr(service);
	}

	/* IP socket. */
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE|AI_ADDRCONFIG;

	/*
	 * Make no difference between empty string and NULL for
	 * host.
	 */
	if (getaddrinfo(*service->host ? service->host : NULL, service->serv,
			&hints, &res) != 0 || res == NULL) {
		diag_set(SocketError, sio_socketname(-1),
			 "can't resolve uri for bind");
		return -1;
	}

	for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
		memcpy(&service->addr, ai->ai_addr, ai->ai_addrlen);
		service->addr_len = ai->ai_addrlen;
		if (evio_service_bind_addr(service) == 0) {
			freeaddrinfo(res);
			return 0;
		}
		struct error *e = diag_last_error(diag_get());
		say_error("%s: failed to bind on %s: %s", service->name,
			  sio_strfaddr(ai->ai_addr, ai->ai_addrlen), e->errmsg);
		/* ignore */
	}
	freeaddrinfo(res);
	diag_set(SocketError, sio_socketname(-1), "%s: failed to bind",
		 service->name);
	return -1;
}

void
evio_service_stop(struct evio_service *service)
{
	say_info("%s: stopped", service->name);
	bool unlink_unix = evio_has_fd(&service->ev) &&
			   service->addr.sa_family == AF_UNIX;
	evio_close(service->loop, &service->ev);
	if (unlink_unix)
		unlink(((struct sockaddr_un *) &service->addr)->sun_path);
}
