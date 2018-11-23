#ifndef TARANTOOL_SIO_H_INCLUDED
#define TARANTOOL_SIO_H_INCLUDED
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
/**
 * Exception-aware wrappers around BSD sockets.
 * Provide better error logging and I/O statistics.
 */
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <tarantool_ev.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

enum { SERVICE_NAME_MAXLEN = 32 };

/** Pretty print a peer address. */
const char *
sio_strfaddr(struct sockaddr *addr, socklen_t addrlen);

/** Get socket peer name. */
int
sio_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);

/**
 * Advance write position in the iovec array based on its current
 * value and the number of bytes written.
 *
 * @param iov The vector written with writev().
 * @param nwr Number of bytes written.
 * @param[in, out] iov_len Offset in iov[0];
 *
 * @retval Offset of iov[0] for the next write
 */
static inline int
sio_move_iov(struct iovec *iov, size_t nwr, size_t *iov_len)
{
	nwr += *iov_len;
	struct iovec *begin = iov;
	while (nwr > 0 && nwr >= iov->iov_len) {
		nwr -= iov->iov_len;
		iov++;
	}
	*iov_len = nwr;
	return iov - begin;
}

/**
 * Change values of iov->iov_len and iov->iov_base
 * to adjust to a partial write.
 */
static inline void
sio_add_to_iov(struct iovec *iov, size_t size)
{
	iov->iov_len += size;
	iov->iov_base = (char *) iov->iov_base - size;
}

#if defined(__cplusplus)
} /* extern "C" */

/** Pretty format socket name and peer. */
const char *
sio_socketname(int fd);

/**
 * Create a socket. A wrapper for socket() function, but sets
 * diagnostics on error.
 */
int
sio_socket(int domain, int type, int protocol);

/**
 * Get file descriptor flags. A wrapper for fcntl(F_GETFL), but
 * sets diagnostics on error.
 */
int
sio_getfl(int fd);

/**
 * Set on or off a file descriptor flag. A wrapper for
 * fcntl(F_SETFL, flag | fcntl(F_GETFL)), but sets diagnostics on
 * error.
 * @param fd File descriptor to set @a flag.
 * @param flag Flag to set.
 * @param on When true, @a flag is added to the mask. When false,
 *        it is removed.
 */
int
sio_setfl(int fd, int flag, bool on);

/**
 * Set an option on a socket. A wrapper for setsockopt(), but sets
 * diagnostics on error.
 */
int
sio_setsockopt(int fd, int level, int optname,
	       const void *optval, socklen_t optlen);

/**
 * Get a socket option value. A wrapper for setsockopt(), but sets
 * diagnostics on error.
 */
int
sio_getsockopt(int fd, int level, int optname,
	       void *optval, socklen_t *optlen);

/**
 * Connect a client socket to a server. A wrapper for connect(),
 * but sets diagnostics on error except EINPROGRESS.
 */
int
sio_connect(int fd, struct sockaddr *addr, socklen_t addrlen);

/**
 * Bind a socket to the given address. A wrapper for bind(), but
 * sets diagnostics on error except EADDRINUSE.
 */
int
sio_bind(int fd, struct sockaddr *addr, socklen_t addrlen);

/**
 * Mark a socket as accepting connections. A wrapper for listen(),
 * but sets diagnostics on error.
 */
int
sio_listen(int fd);

/**
 * Accept a client connection on a server socket. A wrapper for
 * accept(), but sets diagnostics on error except EAGAIN, EINTR,
 * EWOULDBLOCK.
 * @param fd Server socket.
 * @param[out] addr Accepted client's address.
 * @param[in, out] addrlen Size of @a addr.
 * @param[out] is_error_critical Set to true, if an error occured
 *             and it was not EAGAIN, EINTR and EWOULDBLOCK.
 *
 * @retval Client socket, or -1 on error.
 */
int
sio_accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
	   bool *is_error_critical);

/**
 * Read up to @a count bytes from a socket. A wrapper for read(),
 * but sets diagnostics on error except EWOULDBLOCK, EINTR,
 * EAGAIN, ECONNRESET.
 * @param fd Socket.
 * @param buf Buffer to read into.
 * @param count How many bytes to read.
 * @param[out] is_error_critical Set to true, if an error occured
 *             and it was not EAGAIN, EINTR, ECONNRESET and
 *             EWOULDBLOCK.
 *
 * @retval How many bytes are read, or -1 on error.
 */
ssize_t
sio_read(int fd, void *buf, size_t count, bool *is_error_critical);

/**
 * Write up to @a count bytes to a socket. A wrapper for write(),
 * but sets diagnostics on error except EAGAIN, EINTR,
 * EWOULDBLOCK.
 * @param fd Socket.
 * @param buf Buffer to write.
 * @param count How many bytes to write.
 * @param[out] is_error_critical Set to true, if an error occured
 *             and it was not EAGAIN, EINTR and EWOULDBLOCK.
 *
 * @retval How many bytes are written, or -1 on error.
 */
ssize_t
sio_write(int fd, const void *buf, size_t count, bool *is_error_critical);

/**
 * Write @a iov vector to a socket. A wrapper for writev(), but
 * sets diagnostics on error except EAGAIN, EINTR, EWOULDBLOCK.
 * @param fd Socket.
 * @param iov Vector to write.
 * @param iovcnt Size of @a iov.
 * @param[out] is_error_critical Set to true, if an error occured
 *             and it was not EAGAIN, EINTR and EWOULDBLOCK.
 *
 * @retval How many bytes are written, or -1 on error.
 */
ssize_t
sio_writev(int fd, const struct iovec *iov, int iovcnt,
	   bool *is_error_critical);

/**
 * Send a message on a socket. A wrapper for sendto(), but sets
 * diagnostics on error except EAGAIN, EINTR, EWOULDBLOCK.
 * @param fd Socket to send to.
 * @param buf Buffer to send.
 * @param len Size of @a buf.
 * @param flags sendto() flags.
 * @param dest_addr Destination address.
 * @param addrlen Size of @a dest_addr.
 * @param[out] is_error_critical Set to true, if an error occured
 *             and it was not EAGAIN, EINTR and EWOULDBLOCK.
 *
 * @param How many bytes are sent, or -1 on error.
 */
ssize_t
sio_sendto(int fd, const void *buf, size_t len, int flags,
	   const struct sockaddr *dest_addr, socklen_t addrlen,
	   bool *is_error_critical);

/**
 * Receive a message on a socket. A wrapper for recvfrom(), but
 * sets diagnostics on error except EAGAIN, EINTR, EWOULDBLOCK.
 * @param fd Socket to receive from.
 * @param buf Buffer to save message.
 * @param len Size of @a buf.
 * @param flags recvfrom() flags.
 * @param[out] src_addr Source address.
 * @param[in, out] addrlen Size of @a src_addr.
 * @param[out] is_error_critical Set to true, if an error occured
 *             and it was not EAGAIN, EINTR and EWOULDBLOCK.
 *
 * @param How many bytes are received, or -1 on error.
 */
ssize_t
sio_recvfrom(int fd, void *buf, size_t len, int flags,
	     struct sockaddr *src_addr, socklen_t *addrlen,
	     bool *is_error_critical);

#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_SIO_H_INCLUDED */
