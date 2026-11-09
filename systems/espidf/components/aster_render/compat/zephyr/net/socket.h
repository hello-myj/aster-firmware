/* SPDX-License-Identifier: MIT */
#pragma once
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#define zsock_socket socket
#define zsock_close close
#define zsock_bind bind
#define zsock_listen listen
#define zsock_accept accept
#define zsock_connect connect
#define zsock_recv recv
#define zsock_send send
#define zsock_sendto sendto
/* lwIP recvfrom truncates its return count even with MSG_TRUNC. The service
 * must detect and drop an oversized UDP datagram rather than accept its prefix. */
ssize_t aster_idf_recvfrom(int fd,void *data,size_t size,int flags,struct sockaddr *address,socklen_t *length);
#define zsock_recvfrom aster_idf_recvfrom
#define zsock_getsockopt getsockopt
#define zsock_getsockname getsockname
#define zsock_fcntl fcntl
#define zsock_inet_pton inet_pton
#define zsock_inet_ntop inet_ntop
#define zsock_poll poll
#define zsock_pollfd pollfd
#define ZSOCK_POLLOUT POLLOUT
#define ZSOCK_POLLIN POLLIN
