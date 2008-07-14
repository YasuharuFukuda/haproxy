/*
 * AF_INET/AF_INET6 SOCK_STREAM protocol layer (tcp)
 *
 * Copyright 2000-2008 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/errors.h>
#include <common/memory.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/time.h>
#include <common/version.h>

#include <types/acl.h>
#include <types/client.h>
#include <types/global.h>
#include <types/polling.h>
#include <types/proxy.h>
#include <types/server.h>

#include <proto/acl.h>
#include <proto/backend.h>
#include <proto/buffers.h>
#include <proto/fd.h>
#include <proto/protocols.h>
#include <proto/proto_tcp.h>
#include <proto/proxy.h>
#include <proto/queue.h>
#include <proto/senddata.h>
#include <proto/session.h>
#include <proto/stream_sock.h>
#include <proto/task.h>

#ifdef CONFIG_HAP_CTTPROXY
#include <import/ip_tproxy.h>
#endif

static int tcp_bind_listeners(struct protocol *proto);

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_tcpv4 = {
	.name = "tcpv4",
	.sock_domain = AF_INET,
	.sock_type = SOCK_STREAM,
	.sock_prot = IPPROTO_TCP,
	.sock_family = AF_INET,
	.sock_addrlen = sizeof(struct sockaddr_in),
	.l3_addrlen = 32/8,
	.read = &stream_sock_read,
	.write = &stream_sock_write,
	.bind_all = tcp_bind_listeners,
	.unbind_all = unbind_all_listeners,
	.enable_all = enable_all_listeners,
	.listeners = LIST_HEAD_INIT(proto_tcpv4.listeners),
	.nb_listeners = 0,
};

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_tcpv6 = {
	.name = "tcpv6",
	.sock_domain = AF_INET6,
	.sock_type = SOCK_STREAM,
	.sock_prot = IPPROTO_TCP,
	.sock_family = AF_INET6,
	.sock_addrlen = sizeof(struct sockaddr_in6),
	.l3_addrlen = 128/8,
	.read = &stream_sock_read,
	.write = &stream_sock_write,
	.bind_all = tcp_bind_listeners,
	.unbind_all = unbind_all_listeners,
	.enable_all = enable_all_listeners,
	.listeners = LIST_HEAD_INIT(proto_tcpv6.listeners),
	.nb_listeners = 0,
};


/* Binds ipv4 address <local> to socket <fd>, unless <flags> is set, in which
 * case we try to bind <remote>. <flags> is a 2-bit field consisting of :
 *  - 0 : ignore remote address (may even be a NULL pointer)
 *  - 1 : use provided address
 *  - 2 : use provided port
 *  - 3 : use both
 *
 * The function supports multiple foreign binding methods :
 *   - linux_tproxy: we directly bind to the foreign address
 *   - cttproxy: we bind to a local address then nat.
 * The second one can be used as a fallback for the first one.
 * This function returns 0 when everything's OK, 1 if it could not bind, to the
 * local address, 2 if it could not bind to the foreign address.
 */
int tcpv4_bind_socket(int fd, int flags, struct sockaddr_in *local, struct sockaddr_in *remote)
{
	struct sockaddr_in bind_addr;
	int foreign_ok = 0;
	int ret;

#ifdef CONFIG_HAP_LINUX_TPROXY
	static int ip_transp_working = 1;
	if (flags && ip_transp_working) {
		if (setsockopt(fd, SOL_IP, IP_TRANSPARENT, (char *) &one, sizeof(one)) == 0
		    || setsockopt(fd, SOL_IP, IP_FREEBIND, (char *) &one, sizeof(one)) == 0)
			foreign_ok = 1;
		else
			ip_transp_working = 0;
	}
#endif
	if (flags) {
		memset(&bind_addr, 0, sizeof(bind_addr));
		if (flags & 1)
			bind_addr.sin_addr = remote->sin_addr;
		if (flags & 2)
			bind_addr.sin_port = remote->sin_port;
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one));
	if (foreign_ok) {
		ret = bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
		if (ret < 0)
			return 2;
	}
	else {
		ret = bind(fd, (struct sockaddr *)local, sizeof(*local));
		if (ret < 0)
			return 1;
	}

	if (!flags)
		return 0;

#ifdef CONFIG_HAP_CTTPROXY
	if (!foreign_ok) {
		struct in_tproxy itp1, itp2;
		memset(&itp1, 0, sizeof(itp1));

		itp1.op = TPROXY_ASSIGN;
		itp1.v.addr.faddr = bind_addr.sin_addr;
		itp1.v.addr.fport = bind_addr.sin_port;

		/* set connect flag on socket */
		itp2.op = TPROXY_FLAGS;
		itp2.v.flags = ITP_CONNECT | ITP_ONCE;

		if (setsockopt(fd, SOL_IP, IP_TPROXY, &itp1, sizeof(itp1)) != -1 &&
		    setsockopt(fd, SOL_IP, IP_TPROXY, &itp2, sizeof(itp2)) != -1) {
			foreign_ok = 1;
		}
	}
#endif
	if (!foreign_ok)
		/* we could not bind to a foreign address */
		return 2;

	return 0;
}

/* This function tries to bind a TCPv4/v6 listener. It may return a warning or
 * an error message in <err> if the message is at most <errlen> bytes long
 * (including '\0'). The return value is composed from ERR_ABORT, ERR_WARN,
 * ERR_ALERT, ERR_RETRYABLE and ERR_FATAL. ERR_NONE indicates that everything
 * was alright and that no message was returned. ERR_RETRYABLE means that an
 * error occurred but that it may vanish after a retry (eg: port in use), and
 * ERR_FATAL indicates a non-fixable error.ERR_WARN and ERR_ALERT do not alter
 * the meaning of the error, but just indicate that a message is present which
 * should be displayed with the respective level. Last, ERR_ABORT indicates
 * that it's pointless to try to start other listeners. No error message is
 * returned if errlen is NULL.
 */
int tcp_bind_listener(struct listener *listener, char *errmsg, int errlen)
{
	__label__ tcp_return, tcp_close_return;
	int fd, err;
	const char *msg = NULL;

	/* ensure we never return garbage */
	if (errmsg && errlen)
		*errmsg = 0;

	if (listener->state != LI_ASSIGNED)
		return ERR_NONE; /* already bound */

	err = ERR_NONE;

	if ((fd = socket(listener->addr.ss_family, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		err |= ERR_RETRYABLE | ERR_ALERT;
		msg = "cannot create listening socket";
		goto tcp_return;
	}
	
	if (fd >= global.maxsock) {
		err |= ERR_FATAL | ERR_ABORT | ERR_ALERT;
		msg = "not enough free sockets (raise '-n' parameter)";
		goto tcp_close_return;
	}

	if ((fcntl(fd, F_SETFL, O_NONBLOCK) == -1) ||
	    (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
			(char *) &one, sizeof(one)) == -1)) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot make socket non-blocking";
		goto tcp_close_return;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one)) == -1) {
		/* not fatal but should be reported */
		msg = "cannot do so_reuseaddr";
		err |= ERR_ALERT;
	}

	if (listener->options & LI_O_NOLINGER)
		setsockopt(fd, SOL_SOCKET, SO_LINGER, (struct linger *) &nolinger, sizeof(struct linger));
	
#ifdef SO_REUSEPORT
	/* OpenBSD supports this. As it's present in old libc versions of Linux,
	 * it might return an error that we will silently ignore.
	 */
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (char *) &one, sizeof(one));
#endif
#ifdef CONFIG_HAP_LINUX_TPROXY
	if ((listener->options & LI_O_FOREIGN) 
	    && (setsockopt(fd, SOL_IP, IP_TRANSPARENT, (char *) &one, sizeof(one)) == -1)
	    && (setsockopt(fd, SOL_IP, IP_FREEBIND, (char *) &one, sizeof(one)) == -1)) {
		msg = "cannot make listening socket transparent";
		err |= ERR_ALERT;
	}
#endif
	if (bind(fd, (struct sockaddr *)&listener->addr, listener->proto->sock_addrlen) == -1) {
		err |= ERR_RETRYABLE | ERR_ALERT;
		msg = "cannot bind socket";
		goto tcp_close_return;
	}
	
	if (listen(fd, listener->backlog ? listener->backlog : listener->maxconn) == -1) {
		err |= ERR_RETRYABLE | ERR_ALERT;
		msg = "cannot listen to socket";
		goto tcp_close_return;
	}
	
	/* the socket is ready */
	listener->fd = fd;
	listener->state = LI_LISTEN;

	/* the function for the accept() event */
	fd_insert(fd);
	fdtab[fd].cb[DIR_RD].f = listener->accept;
	fdtab[fd].cb[DIR_WR].f = NULL; /* never called */
	fdtab[fd].cb[DIR_RD].b = fdtab[fd].cb[DIR_WR].b = NULL;
	fdtab[fd].owner = (struct task *)listener; /* reference the listener instead of a task */
	fdtab[fd].state = FD_STLISTEN;
	fdtab[fd].peeraddr = NULL;
	fdtab[fd].peerlen = 0;
	fdtab[fd].listener = NULL;
 tcp_return:
	if (msg && errlen)
		strlcpy2(errmsg, msg, errlen);
	return err;

 tcp_close_return:
	close(fd);
	goto tcp_return;
}

/* This function creates all TCP sockets bound to the protocol entry <proto>.
 * It is intended to be used as the protocol's bind_all() function.
 * The sockets will be registered but not added to any fd_set, in order not to
 * loose them across the fork(). A call to enable_all_listeners() is needed
 * to complete initialization. The return value is composed from ERR_*.
 */
static int tcp_bind_listeners(struct protocol *proto)
{
	struct listener *listener;
	int err = ERR_NONE;

	list_for_each_entry(listener, &proto->listeners, proto_list) {
		err |= tcp_bind_listener(listener, NULL, 0);
		if ((err & ERR_CODE) == ERR_ABORT)
			break;
	}

	return err;
}

/* Add listener to the list of tcpv4 listeners. The listener's state
 * is automatically updated from LI_INIT to LI_ASSIGNED. The number of
 * listeners is updated. This is the function to use to add a new listener.
 */
void tcpv4_add_listener(struct listener *listener)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->proto = &proto_tcpv4;
	LIST_ADDQ(&proto_tcpv4.listeners, &listener->proto_list);
	proto_tcpv4.nb_listeners++;
}

/* Add listener to the list of tcpv4 listeners. The listener's state
 * is automatically updated from LI_INIT to LI_ASSIGNED. The number of
 * listeners is updated. This is the function to use to add a new listener.
 */
void tcpv6_add_listener(struct listener *listener)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->proto = &proto_tcpv6;
	LIST_ADDQ(&proto_tcpv6.listeners, &listener->proto_list);
	proto_tcpv6.nb_listeners++;
}

/* This function should be called to parse a line starting with the "tcp-request"
 * keyword.
 */
static int tcp_parse_tcp_req(char **args, int section_type, struct proxy *curpx,
			     struct proxy *defpx, char *err, int errlen)
{
	const char *ptr = NULL;
	int val;
	int retlen;

	if (!*args[1]) {
		snprintf(err, errlen, "missing argument for '%s' in %s '%s'",
			 args[0], proxy_type_str(proxy), curpx->id);
		return -1;
	}

	if (!strcmp(args[1], "inspect-delay")) {
		if (curpx == defpx) {
			snprintf(err, errlen, "%s %s is not allowed in 'defaults' sections",
				 args[0], args[1]);
			return -1;
		}

		if (!(curpx->cap & PR_CAP_FE)) {
			snprintf(err, errlen, "%s %s will be ignored because %s '%s' has no %s capability",
				 args[0], args[1], proxy_type_str(proxy), curpx->id,
				 "frontend");
			return 1;
		}

		if (!*args[2] || (ptr = parse_time_err(args[2], &val, TIME_UNIT_MS))) {
			retlen = snprintf(err, errlen,
					  "'%s %s' expects a positive delay in milliseconds, in %s '%s'",
					  args[0], args[1], proxy_type_str(proxy), curpx->id);
			if (ptr && retlen < errlen)
				retlen += snprintf(err+retlen, errlen - retlen,
						   " (unexpected character '%c')", *ptr);
			return -1;
		}

		if (curpx->tcp_req.inspect_delay) {
			snprintf(err, errlen, "ignoring %s %s (was already defined) in %s '%s'",
				 args[0], args[1], proxy_type_str(proxy), curpx->id);
			return 1;
		}
		curpx->tcp_req.inspect_delay = val;
		return 0;
	}

	if (!strcmp(args[1], "content")) {
		int action;
		int pol = ACL_COND_NONE;
		struct acl_cond *cond;
		struct tcp_rule *rule;

		if (curpx == defpx) {
			snprintf(err, errlen, "%s %s is not allowed in 'defaults' sections",
				 args[0], args[1]);
			return -1;
		}

		if (!strcmp(args[2], "accept"))
			action = TCP_ACT_ACCEPT;
		else if (!strcmp(args[2], "reject"))
			action = TCP_ACT_REJECT;
		else {
			retlen = snprintf(err, errlen,
					  "'%s %s' expects 'accept' or 'reject', in %s '%s' (was '%s')",
					  args[0], args[1], proxy_type_str(curpx), curpx->id, args[2]);
			return -1;
		}

		pol = ACL_COND_NONE;
		cond = NULL;

		if (!strcmp(args[3], "if"))
			pol = ACL_COND_IF;
		else if (!strcmp(args[3], "unless"))
			pol = ACL_COND_UNLESS;

		/* Note: we consider "if TRUE" when there is no condition */
		if (pol != ACL_COND_NONE &&
		    (cond = parse_acl_cond((const char **)args+4, &curpx->acl, pol)) == NULL) {
			retlen = snprintf(err, errlen,
					  "Error detected in %s '%s' while parsing '%s' condition",
					  proxy_type_str(curpx), curpx->id, args[3]);
			return -1;
		}

		rule = (struct tcp_rule *)calloc(1, sizeof(*rule));
		rule->cond = cond;
		rule->action = action;
		LIST_INIT(&rule->list);
		LIST_ADDQ(&curpx->tcp_req.inspect_rules, &rule->list);
		return 0;
	}

	snprintf(err, errlen, "unknown argument '%s' after '%s' in %s '%s'",
		 args[1], args[0], proxy_type_str(proxy), curpx->id);
	return -1;
}

/* return the number of bytes in the request buffer */
static int
acl_fetch_req_len(struct proxy *px, struct session *l4, void *l7, int dir,
		  struct acl_expr *expr, struct acl_test *test)
{
	if (!l4 || !l4->req)
		return 0;

	test->i = l4->req->l;
	test->flags = ACL_TEST_F_VOLATILE | ACL_TEST_F_MAY_CHANGE;
	return 1;
}


static struct cfg_kw_list cfg_kws = {{ },{
	{ CFG_LISTEN, "tcp-request", tcp_parse_tcp_req },
	{ 0, NULL, NULL },
}};

static struct acl_kw_list acl_kws = {{ },{
	{ "req_len",    acl_parse_int, acl_fetch_req_len, acl_match_int },
	{ NULL, NULL, NULL, NULL },
}};

__attribute__((constructor))
static void __tcp_protocol_init(void)
{
	protocol_register(&proto_tcpv4);
	protocol_register(&proto_tcpv6);
	cfg_register_keywords(&cfg_kws);
	acl_register_keywords(&acl_kws);
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
