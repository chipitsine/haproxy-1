/*
 * AF_INET/AF_INET6 socket management
 *
 * Copyright 2000-2020 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <string.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/tcp.h>
#include <netinet/in.h>

#include <haproxy/api.h>
#include <haproxy/global.h>
#include <haproxy/sock_inet.h>
#include <haproxy/tools.h>


/* PLEASE NOTE for function below:
 *   - sock_inet4_* is solely for AF_INET (IPv4)
 *   - sock_inet6_* is solely for AF_INET6 (IPv6)
 *   - sock_inet_*  is for either
 *
 * The address family SHOULD always be checked. In some cases a function will
 * be used in a situation where the address family is guaranteed (e.g. protocol
 * definitions), so the test may be avoided. This special case must then be
 * mentioned in the comment before the function definition.
 */

/* determine if the operating system uses IPV6_V6ONLY by default. 0=no, 1=yes.
 * It also remains if IPv6 is not enabled/configured.
 */
int sock_inet6_v6only_default = 0;

/* Default TCPv4/TCPv6 MSS settings. -1=unknown. */
int sock_inet_tcp_maxseg_default = -1;
int sock_inet6_tcp_maxseg_default = -1;

/* Compares two AF_INET sockaddr addresses. Returns 0 if they match or non-zero
 * if they do not match.
 */
int sock_inet4_addrcmp(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
	const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;

	if (a->ss_family != b->ss_family)
		return -1;

	if (a->ss_family != AF_INET)
		return -1;

	if (a4->sin_port != b4->sin_port)
		return -1;

	return memcmp(&a4->sin_addr, &b4->sin_addr, sizeof(a4->sin_addr));
}

/* Compares two AF_INET6 sockaddr addresses. Returns 0 if they match or
 * non-zero if they do not match.
 */
int sock_inet6_addrcmp(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
	const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;

	if (a->ss_family != b->ss_family)
		return -1;

	if (a->ss_family != AF_INET6)
		return -1;

	if (a6->sin6_port != b6->sin6_port)
		return -1;

	return memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr));
}

/*
 * Retrieves the original destination address for the socket <fd> which must be
 * of family AF_INET (not AF_INET6), with <dir> indicating if we're a listener
 * (=0) or an initiator (!=0). In the case of a listener, if the original
 * destination address was translated, the original address is retrieved. It
 * returns 0 in case of success, -1 in case of error. The socket's source
 * address is stored in <sa> for <salen> bytes.
 */
int sock_inet_get_dst(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	if (dir)
		return getpeername(fd, sa, &salen);
	else {
		int ret = getsockname(fd, sa, &salen);

		if (ret < 0)
			return ret;

#if defined(USE_TPROXY) && defined(SO_ORIGINAL_DST)
		/* For TPROXY and Netfilter's NAT, we can retrieve the original
		 * IPv4 address before DNAT/REDIRECT. We must not do that with
		 * other families because v6-mapped IPv4 addresses are still
		 * reported as v4.
		 */
		if (getsockopt(fd, SOL_IP, SO_ORIGINAL_DST, sa, &salen) == 0)
			return 0;
#endif
		return ret;
	}
}

/* Returns true if the passed FD corresponds to a socket bound with LI_O_FOREIGN
 * according to the various supported socket options. The socket's address family
 * must be passed in <family>.
 */
int sock_inet_is_foreign(int fd, sa_family_t family)
{
	int val __maybe_unused;
	socklen_t len __maybe_unused;

	switch (family) {
	case AF_INET:
#if defined(IP_TRANSPARENT)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, SOL_IP, IP_TRANSPARENT, &val, &len) == 0 && val)
			return 1;
#endif
#if defined(IP_FREEBIND)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, SOL_IP, IP_FREEBIND, &val, &len) == 0 && val)
			return 1;
#endif
#if defined(IP_BINDANY)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, IPPROTO_IP, IP_BINDANY, &val, &len) == 0 && val)
			return 1;
#endif
#if defined(SO_BINDANY)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, SOL_SOCKET, SO_BINDANY, &val, &len) == 0 && val)
			return 1;
#endif
		break;

	case AF_INET6:
#if defined(IPV6_TRANSPARENT) && defined(SOL_IPV6)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, SOL_IPV6, IPV6_TRANSPARENT, &val, &len) == 0 && val)
			return 1;
#endif
#if defined(IP_FREEBIND)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, SOL_IP, IP_FREEBIND, &val, &len) == 0 && val)
			return 1;
#endif
#if defined(IPV6_BINDANY)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, IPPROTO_IPV6, IPV6_BINDANY, &val, &len) == 0 && val)
			return 1;
#endif
#if defined(SO_BINDANY)
		val = 0; len = sizeof(val);
		if (getsockopt(fd, SOL_SOCKET, SO_BINDANY, &val, &len) == 0 && val)
			return 1;
#endif
		break;
	}
	return 0;
}

/* Attempt all known socket options to prepare an AF_INET4 socket to be bound
 * to a foreign address. The socket must already exist and must not be bound.
 * 1 is returned on success, 0 on failure. The caller must check the address
 * family before calling this function.
 */
int sock_inet4_make_foreign(int fd)
{
	return
#if defined(IP_TRANSPARENT)
		setsockopt(fd, SOL_IP, IP_TRANSPARENT, &one, sizeof(one)) == 0 ||
#endif
#if defined(IP_FREEBIND)
		setsockopt(fd, SOL_IP, IP_FREEBIND, &one, sizeof(one)) == 0 ||
#endif
#if defined(IP_BINDANY)
		setsockopt(fd, IPPROTO_IP, IP_BINDANY, &one, sizeof(one)) == 0 ||
#endif
#if defined(SO_BINDANY)
		setsockopt(fd, SOL_SOCKET, SO_BINDANY, &one, sizeof(one)) == 0 ||
#endif
		0;
}

/* Attempt all known socket options to prepare an AF_INET6 socket to be bound
 * to a foreign address. The socket must already exist and must not be bound.
 * 1 is returned on success, 0 on failure. The caller must check the address
 * family before calling this function.
 */
int sock_inet6_make_foreign(int fd)
{
	return
#if defined(IPV6_TRANSPARENT) && defined(SOL_IPV6)
		setsockopt(fd, SOL_IPV6, IPV6_TRANSPARENT, &one, sizeof(one)) == 0 ||
#endif
#if defined(IP_FREEBIND)
		setsockopt(fd, SOL_IP, IP_FREEBIND, &one, sizeof(one)) == 0 ||
#endif
#if defined(IPV6_BINDANY)
		setsockopt(fd, IPPROTO_IPV6, IPV6_BINDANY, &one, sizeof(one)) == 0 ||
#endif
#if defined(SO_BINDANY)
		setsockopt(fd, SOL_SOCKET, SO_BINDANY, &one, sizeof(one)) == 0 ||
#endif
		0;
}

static void sock_inet_prepare()
{
	int fd, val;
	socklen_t len;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd >= 0) {
#ifdef TCP_MAXSEG
		/* retrieve the OS' default mss for TCPv4 */
		len = sizeof(val);
		if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &val, &len) == 0)
			sock_inet_tcp_maxseg_default = val;
#endif
		close(fd);
	}

	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd >= 0) {
#if defined(IPV6_V6ONLY)
		/* retrieve the OS' bindv6only value */
		len = sizeof(val);
		if (getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, &len) == 0 && val > 0)
			sock_inet6_v6only_default = 1;
#endif

#ifdef TCP_MAXSEG
		/* retrieve the OS' default mss for TCPv6 */
		len = sizeof(val);
		if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &val, &len) == 0)
			sock_inet6_tcp_maxseg_default = val;
#endif
		close(fd);
	}
}

INITCALL0(STG_PREPARE, sock_inet_prepare);


REGISTER_BUILD_OPTS("Built with transparent proxy support using:"
#if defined(IP_TRANSPARENT)
		    " IP_TRANSPARENT"
#endif
#if defined(IPV6_TRANSPARENT)
		    " IPV6_TRANSPARENT"
#endif
#if defined(IP_FREEBIND)
		    " IP_FREEBIND"
#endif
#if defined(IP_BINDANY)
		    " IP_BINDANY"
#endif
#if defined(IPV6_BINDANY)
		    " IPV6_BINDANY"
#endif
#if defined(SO_BINDANY)
		    " SO_BINDANY"
#endif
		    "");
