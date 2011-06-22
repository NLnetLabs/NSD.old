/*
 * remote.c - remote control for the NSD daemon.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the remote control functionality for the daemon.
 * The remote control can be performed using either the commandline
 * nsd-control tool, or a SSLv3/TLS capable web browser. 
 * The channel is secured using SSLv3 or TLSv1, and certificates.
 * Both the server and the client(control tool) have their own keys.
 */
#include "config.h"
#if defined(HAVE_SSL)

#ifdef HAVE_OPENSSL_SSL_H
#include "openssl/ssl.h"
#endif
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#include <ctype.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include "remote.h"
#include "util.h"
#include "netio.h"
#include "options.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

/** number of seconds timeout on incoming remote control handshake */
#define REMOTE_CONTROL_TCP_TIMEOUT 120

/**
 * a busy control command connection, SSL state
 */
struct rc_state {
	/** the next item in list */
	struct rc_state* next;
	/** the commpoint */
	struct netio_handler* c;
	/** in the handshake part */
	enum { rc_none, rc_hs_read, rc_hs_write } shake_state;
	/** the ssl state */
	SSL* ssl;
	/** the rc this is part of */
	struct daemon_remote* rc;
};

/**
 * The remote control state.
 */
struct daemon_remote {
	/** the master process for this remote control */
	struct xfrd_state* xfrd;
	/** commpoints for accepting remote control connections */
	struct netio_handler_list* accept_list;
	/** number of active commpoints that are handling remote control */
	int active;
	/** max active commpoints */
	int max_active;
	/** current commpoints busy; should be a short list, malloced */
	struct rc_state* busy_list;
	/** the SSL context for creating new SSL streams */
	SSL_CTX* ctx;
};

/** 
 * Print fixed line of text over ssl connection in blocking mode
 * @param ssl: print to
 * @param text: the text.
 * @return false on connection failure.
 */
static int ssl_print_text(SSL* ssl, const char* text);

/** 
 * printf style printing to the ssl connection
 * @param ssl: the SSL connection to print to. Blocking.
 * @param format: printf style format string.
 * @return success or false on a network failure.
 */
static int ssl_printf(SSL* ssl, const char* format, ...)
        ATTR_FORMAT(printf, 2, 3);

/**
 * Read until \n is encountered
 * If SSL signals EOF, the string up to then is returned (without \n).
 * @param ssl: the SSL connection to read from. blocking.
 * @param buf: buffer to read to.
 * @param max: size of buffer.
 * @return false on connection failure.
 */
static int ssl_read_line(SSL* ssl, char* buf, size_t max);


/** ---- end of private defines ---- **/


/** log ssl crypto err */
static void
log_crypto_err(const char* str)
{
	/* error:[error code]:[library name]:[function name]:[reason string] */
	char buf[128];
	unsigned long e;
	ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
	log_msg(LOG_ERR, "%s crypto %s", str, buf);
	while( (e=ERR_get_error()) ) {
		ERR_error_string_n(e, buf, sizeof(buf));
		log_msg(LOG_ERR, "and additionally crypto %s", buf);
	}
}

/** subtract timers and the values do not overflow or become negative */
static void
timeval_subtract(struct timeval* d, const struct timeval* end, 
	const struct timeval* start)
{
#ifndef S_SPLINT_S
	time_t end_usec = end->tv_usec;
	d->tv_sec = end->tv_sec - start->tv_sec;
	if(end_usec < start->tv_usec) {
		end_usec += 1000000;
		d->tv_sec--;
	}
	d->tv_usec = end_usec - start->tv_usec;
#endif
}

/** divide sum of timers to get average */
static void
timeval_divide(struct timeval* avg, const struct timeval* sum, size_t d)
{
#ifndef S_SPLINT_S
	size_t leftover;
	if(d == 0) {
		avg->tv_sec = 0;
		avg->tv_usec = 0;
		return;
	}
	avg->tv_sec = sum->tv_sec / d;
	avg->tv_usec = sum->tv_usec / d;
	/* handle fraction from seconds divide */
	leftover = sum->tv_sec - avg->tv_sec*d;
	avg->tv_usec += (leftover*1000000)/d;
#endif
}

struct daemon_remote*
daemon_remote_create(nsd_options_t* cfg)
{
	char* s_cert;
	char* s_key;
	struct daemon_remote* rc = (struct daemon_remote*)xalloc_zero(
		sizeof(*rc));
	rc->max_active = 10;
	assert(cfg->control_enable);

	/* init SSL library */
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	(void)SSL_library_init();

	rc->ctx = SSL_CTX_new(SSLv23_server_method());
	if(!rc->ctx) {
		log_crypto_err("could not SSL_CTX_new");
		free(rc);
		return NULL;
	}
	/* no SSLv2 because has defects */
	if(!(SSL_CTX_set_options(rc->ctx, SSL_OP_NO_SSLv2) & SSL_OP_NO_SSLv2)){
		log_crypto_err("could not set SSL_OP_NO_SSLv2");
		daemon_remote_delete(rc);
		return NULL;
	}
	s_cert = cfg->server_cert_file;
	s_key = cfg->server_key_file;
	VERBOSITY(2, (LOG_INFO, "setup SSL certificates"));
	if (!SSL_CTX_use_certificate_file(rc->ctx,s_cert,SSL_FILETYPE_PEM)) {
		log_msg(LOG_ERR, "Error for server-cert-file: %s", s_cert);
		log_crypto_err("Error in SSL_CTX use_certificate_file");
		goto setup_error;
	}
	if(!SSL_CTX_use_PrivateKey_file(rc->ctx,s_key,SSL_FILETYPE_PEM)) {
		log_msg(LOG_ERR, "Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX use_PrivateKey_file");
		goto setup_error;
	}
	if(!SSL_CTX_check_private_key(rc->ctx)) {
		log_msg(LOG_ERR, "Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX check_private_key");
		goto setup_error;
	}
	if(!SSL_CTX_load_verify_locations(rc->ctx, s_cert, NULL)) {
		log_crypto_err("Error setting up SSL_CTX verify locations");
	setup_error:
		daemon_remote_delete(rc);
		return NULL;
	}
	SSL_CTX_set_client_CA_list(rc->ctx, SSL_load_client_CA_file(s_cert));
	SSL_CTX_set_verify(rc->ctx, SSL_VERIFY_PEER, NULL);

	/* and try to open the ports */
	if(!daemon_remote_open_ports(rc, cfg)) {
		log_msg(LOG_ERR, "could not open remote control port");
		goto setup_error;
	}

	return rc;
}

void daemon_remote_close(struct daemon_remote* rc)
{
	struct rc_state* p, *np;
	netio_handler_list_type* h, *nh;
	if(!rc) return;

	/* close listen sockets */
	h = rc->accept_list;
	while(h) {
		nh = h->next;
		close(h->handler->fd);
		free(h->handler);
		free(h);
		h = nh;
	}
	rc->accept_list = NULL;

	/* close busy connection sockets */
	p = rc->busy_list;
	while(p) {
		np = p->next;
		if(p->ssl)
			SSL_free(p->ssl);
		close(p->c->fd);
		free(p->c);
		free(p);
		p = np;
	}
	rc->busy_list = NULL;
	rc->active = 0;
}

void daemon_remote_delete(struct daemon_remote* rc)
{
	if(!rc) return;
	daemon_remote_close(rc);
	if(rc->ctx) {
		SSL_CTX_free(rc->ctx);
	}
	free(rc);
}

static int
create_tcp_accept_sock(struct addrinfo* addr, int* noproto)
{
#if defined(SO_REUSEADDR) || (defined(INET6) && (defined(IPV6_V6ONLY) || defined(IPV6_USE_MIN_MTU) || defined(IPV6_MTU)))
	int on = 1;
#endif
	int s;
	*noproto = 0;
	if ((s = socket(addr->ai_family, addr->ai_socktype, 0)) == -1) {
#if defined(INET6)
		if (addr->ai_family == AF_INET6 &&
			errno == EAFNOSUPPORT) {
			*noproto = 1;
			log_msg(LOG_WARNING, "fallback to TCP4, no IPv6: not supported");
			return -1;
		}
#endif /* INET6 */
		log_msg(LOG_ERR, "can't create a socket: %s", strerror(errno));
		return -1;
	}
#ifdef  SO_REUSEADDR
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
		log_msg(LOG_ERR, "setsockopt(..., SO_REUSEADDR, ...) failed: %s", strerror(errno));
	}
#endif /* SO_REUSEADDR */
#if defined(INET6) && defined(IPV6_V6ONLY)
	if (addr->ai_family == AF_INET6 &&
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
	{
		log_msg(LOG_ERR, "setsockopt(..., IPV6_V6ONLY, ...) failed: %s", strerror(errno));
		return -1;
	}
#endif
	/* set it nonblocking */
	/* (StevensUNP p463), if tcp listening socket is blocking, then
	   it may block in accept, even if select() says readable. */
	if (fcntl(s, F_SETFL, O_NONBLOCK) == -1) {
		log_msg(LOG_ERR, "cannot fcntl tcp: %s", strerror(errno));
	}
	/* Bind it... */
	if (bind(s, (struct sockaddr *)addr->ai_addr, addr->ai_addrlen) != 0) {
		log_msg(LOG_ERR, "can't bind tcp socket: %s", strerror(errno));
		return -1;
	}
	/* Listen to it... */
	if (listen(s, TCP_BACKLOG) == -1) {
		log_msg(LOG_ERR, "can't listen: %s", strerror(errno));
		return -1;
	}
	return s;
}

/**
 * Add and open a new control port
 * @param rc: rc with result list.
 * @param ip: ip str
 * @param nr: port nr
 * @param noproto_is_err: if lack of protocol support is an error.
 * @return false on failure.
 */
static int
add_open(struct daemon_remote* rc, const char* ip, int nr, int noproto_is_err)
{
	struct addrinfo hints;
	struct addrinfo* res;
	netio_handler_list_type* hl;
	int noproto;
	int fd, r;
	char port[15];
	snprintf(port, sizeof(port), "%d", nr);
	port[sizeof(port)-1]=0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
	if((r = getaddrinfo(ip, port, &hints, &res)) != 0 || !res) {
                log_msg(LOG_ERR, "control interface %s:%s getaddrinfo: %s %s",
			ip?ip:"default", port, gai_strerror(r),
#ifdef EAI_SYSTEM
			r==EAI_SYSTEM?(char*)strerror(errno):""
#else
			""
#endif
			);
		return 0;
	}

	/* open fd */
	fd = create_tcp_accept_sock(res, &noproto);
	freeaddrinfo(res);
	if(fd == -1 && noproto) {
		if(!noproto_is_err)
			return 1; /* return success, but do nothing */
		log_msg(LOG_ERR, "cannot open control interface %s %d : "
			"protocol not supported", ip, nr);
		return 0;
	}
	if(fd == -1) {
		log_msg(LOG_ERR, "cannot open control interface %s %d", ip, nr);
		return 0;
	}

	/* alloc */
	hl = (netio_handler_list_type*)xalloc_zero(sizeof(*hl));
	hl->handler = (netio_handler_type*)xalloc_zero(sizeof(*hl->handler));
	hl->next = rc->accept_list;
	rc->accept_list = hl;
	hl->handler->fd = fd;
	return 1;
}

int daemon_remote_open_ports(struct daemon_remote* rc, nsd_options_t* cfg)
{
	assert(cfg->control_enable && cfg->control_port);
	if(cfg->control_interface) {
		ip_address_option_t* p;
		for(p = cfg->control_interface; p; p = p->next) {
			if(!add_open(rc, p->address, cfg->control_port, 1)) {
				return 0;
			}
		}
	} else {
		/* defaults */
		if(!cfg->ip4_only &&
			!add_open(rc, "::1", cfg->control_port, 0)) {
			return 0;
		}
		if(!cfg->ip6_only &&
			!add_open(rc, "127.0.0.1", cfg->control_port, 1)) {
			return 0;
		}
	}
	return 1;
}

#if 0
/* TODO */

/** open accept commpoint */
static int
accept_open(struct daemon_remote* rc, int fd)
{
	struct listen_list* n = (struct listen_list*)malloc(sizeof(*n));
	if(!n) {
		log_err("out of memory");
		return 0;
	}
	n->next = rc->accept_list;
	rc->accept_list = n;
	/* open commpt */
	n->com = comm_point_create_raw(rc->worker->base, fd, 0, 
		&remote_accept_callback, rc);
	if(!n->com)
		return 0;
	/* keep this port open, its fd is kept in the rc portlist */
	n->com->do_not_close = 1;
	return 1;
}

int daemon_remote_open_accept(struct daemon_remote* rc, 
	struct listen_port* ports, struct worker* worker)
{
	struct listen_port* p;
	rc->worker = worker;
	for(p = ports; p; p = p->next) {
		if(!accept_open(rc, p->fd)) {
			log_err("could not create accept comm point");
			return 0;
		}
	}
	return 1;
}

int remote_accept_callback(struct comm_point* c, void* arg, int err, 
	struct comm_reply* ATTR_UNUSED(rep))
{
	struct daemon_remote* rc = (struct daemon_remote*)arg;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int newfd;
	struct rc_state* n;
	if(err != NETEVENT_NOERROR) {
		log_err("error %d on remote_accept_callback", err);
		return 0;
	}
	/* perform the accept */
	newfd = comm_point_perform_accept(c, &addr, &addrlen);
	if(newfd == -1)
		return 0;
	/* create new commpoint unless we are servicing already */
	if(rc->active >= rc->max_active) {
		log_warn("drop incoming remote control: too many connections");
		comm_point_stop_listening(c);
	close_exit:
#ifndef USE_WINSOCK
		close(newfd);
#else
		closesocket(newfd);
#endif
		return 0;
	}

	/* setup commpoint to service the remote control command */
	n = (struct rc_state*)calloc(1, sizeof(*n));
	if(!n) {
		log_err("out of memory");
		goto close_exit;
	}
	/* start in reading state */
	n->c = comm_point_create_raw(rc->worker->base, newfd, 0, 
		&remote_control_callback, n);
	if(!n->c) {
		log_err("out of memory");
		free(n);
		goto close_exit;
	}
	log_addr(VERB_QUERY, "new control connection from", &addr, addrlen);
	n->c->do_not_close = 0;
	comm_point_stop_listening(n->c);
	comm_point_start_listening(n->c, -1, REMOTE_CONTROL_TCP_TIMEOUT);
	memcpy(&n->c->repinfo.addr, &addr, addrlen);
	n->c->repinfo.addrlen = addrlen;
	n->shake_state = rc_hs_read;
	n->ssl = SSL_new(rc->ctx);
	if(!n->ssl) {
		log_crypto_err("could not SSL_new");
		free(n);
		goto close_exit;
	}
	SSL_set_accept_state(n->ssl);
        (void)SSL_set_mode(n->ssl, SSL_MODE_AUTO_RETRY);
	if(!SSL_set_fd(n->ssl, newfd)) {
		log_crypto_err("could not SSL_set_fd");
		SSL_free(n->ssl);
		free(n);
		goto close_exit;
	}

	n->rc = rc;
	n->next = rc->busy_list;
	rc->busy_list = n;
	rc->active ++;

	/* perform the first nonblocking read already, for windows, 
	 * so it can return wouldblock. could be faster too. */
	(void)remote_control_callback(n->c, n, NETEVENT_NOERROR, NULL);
	return 0;
}

/** delete from list */
static void
state_list_remove_elem(struct rc_state** list, struct comm_point* c)
{
	while(*list) {
		if( (*list)->c == c) {
			*list = (*list)->next;
			return;
		}
		list = &(*list)->next;
	}
}

/** decrease active count and remove commpoint from busy list */
static void
clean_point(struct daemon_remote* rc, struct rc_state* s)
{
	state_list_remove_elem(&rc->busy_list, s->c);
	rc->active --;
	if(s->ssl) {
		SSL_shutdown(s->ssl);
		SSL_free(s->ssl);
	}
	comm_point_delete(s->c);
	free(s);
}

static int
ssl_print_text(SSL* ssl, const char* text)
{
	int r;
	if(!ssl) 
		return 0;
	ERR_clear_error();
	if((r=SSL_write(ssl, text, (int)strlen(text))) <= 0) {
		if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN) {
			verbose(VERB_QUERY, "warning, in SSL_write, peer "
				"closed connection");
			return 0;
		}
		log_crypto_err("could not SSL_write");
		return 0;
	}
	return 1;
}

/** print text over the ssl connection */
static int
ssl_print_vmsg(SSL* ssl, const char* format, va_list args)
{
	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, args);
	return ssl_print_text(ssl, msg);
}

/** printf style printing to the ssl connection */
static int ssl_printf(SSL* ssl, const char* format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = ssl_print_vmsg(ssl, format, args);
	va_end(args);
	return ret;
}

static int
ssl_read_line(SSL* ssl, char* buf, size_t max)
{
	int r;
	size_t len = 0;
	if(!ssl)
		return 0;
	while(len < max) {
		ERR_clear_error();
		if((r=SSL_read(ssl, buf+len, 1)) <= 0) {
			if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN) {
				buf[len] = 0;
				return 1;
			}
			log_crypto_err("could not SSL_read");
			return 0;
		}
		if(buf[len] == '\n') {
			/* return string without \n */
			buf[len] = 0;
			return 1;
		}
		len++;
	}
	buf[max-1] = 0;
	log_err("control line too long (%d): %s", (int)max, buf);
	return 0;
}

/** skip whitespace, return new pointer into string */
static char*
skipwhite(char* str)
{
	/* EOS \0 is not a space */
	while( isspace(*str) ) 
		str++;
	return str;
}

/** send the OK to the control client */
static void send_ok(SSL* ssl)
{
	(void)ssl_printf(ssl, "ok\n");
}

/** do the stop command */
static void
do_stop(SSL* ssl, struct daemon_remote* rc)
{
	rc->worker->need_to_exit = 1;
	comm_base_exit(rc->worker->base);
	send_ok(ssl);
}

/** do the reload command */
static void
do_reload(SSL* ssl, struct daemon_remote* rc)
{
	rc->worker->need_to_exit = 0;
	comm_base_exit(rc->worker->base);
	send_ok(ssl);
}

/** do the verbosity command */
static void
do_verbosity(SSL* ssl, char* str)
{
	int val = atoi(str);
	if(val == 0 && strcmp(str, "0") != 0) {
		ssl_printf(ssl, "error in verbosity number syntax: %s\n", str);
		return;
	}
	verbosity = val;
	send_ok(ssl);
}

/** print stats from statinfo */
static int
print_stats(SSL* ssl, const char* nm, struct stats_info* s)
{
	struct timeval avg;
	if(!ssl_printf(ssl, "%s.num.queries"SQ"%u\n", nm, 
		(unsigned)s->svr.num_queries)) return 0;
	if(!ssl_printf(ssl, "%s.num.cachehits"SQ"%u\n", nm, 
		(unsigned)(s->svr.num_queries 
			- s->svr.num_queries_missed_cache))) return 0;
	if(!ssl_printf(ssl, "%s.num.cachemiss"SQ"%u\n", nm, 
		(unsigned)s->svr.num_queries_missed_cache)) return 0;
	if(!ssl_printf(ssl, "%s.num.prefetch"SQ"%u\n", nm, 
		(unsigned)s->svr.num_queries_prefetch)) return 0;
	if(!ssl_printf(ssl, "%s.num.recursivereplies"SQ"%u\n", nm, 
		(unsigned)s->mesh_replies_sent)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.avg"SQ"%g\n", nm,
		(s->svr.num_queries_missed_cache+s->svr.num_queries_prefetch)?
			(double)s->svr.sum_query_list_size/
			(s->svr.num_queries_missed_cache+
			s->svr.num_queries_prefetch) : 0.0)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.max"SQ"%u\n", nm,
		(unsigned)s->svr.max_query_list_size)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.overwritten"SQ"%u\n", nm,
		(unsigned)s->mesh_jostled)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.exceeded"SQ"%u\n", nm,
		(unsigned)s->mesh_dropped)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.current.all"SQ"%u\n", nm,
		(unsigned)s->mesh_num_states)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.current.user"SQ"%u\n", nm,
		(unsigned)s->mesh_num_reply_states)) return 0;
	timeval_divide(&avg, &s->mesh_replies_sum_wait, s->mesh_replies_sent);
	if(!ssl_printf(ssl, "%s.recursion.time.avg"SQ"%d.%6.6d\n", nm,
		(int)avg.tv_sec, (int)avg.tv_usec)) return 0;
	if(!ssl_printf(ssl, "%s.recursion.time.median"SQ"%g\n", nm, 
		s->mesh_time_median)) return 0;
	return 1;
}

/** print stats for one thread */
static int
print_thread_stats(SSL* ssl, int i, struct stats_info* s)
{
	char nm[16];
	snprintf(nm, sizeof(nm), "thread%d", i);
	nm[sizeof(nm)-1]=0;
	return print_stats(ssl, nm, s);
}

/** print long number */
static int
print_longnum(SSL* ssl, char* desc, size_t x)
{
	if(x > 1024*1024*1024) {
		/* more than a Gb */
		size_t front = x / (size_t)1000000;
		size_t back = x % (size_t)1000000;
		return ssl_printf(ssl, "%s%u%6.6u\n", desc, 
			(unsigned)front, (unsigned)back);
	} else {
		return ssl_printf(ssl, "%s%u\n", desc, (unsigned)x);
	}
}

/** print mem stats */
static int
print_mem(SSL* ssl, struct worker* worker, struct daemon* daemon)
{
	int m;
	size_t msg, rrset, val, iter;
#ifdef HAVE_SBRK
	extern void* unbound_start_brk;
	void* cur = sbrk(0);
	if(!print_longnum(ssl, "mem.total.sbrk"SQ, 
		(size_t)((char*)cur - (char*)unbound_start_brk))) return 0;
#endif /* HAVE_SBRK */
	msg = slabhash_get_mem(daemon->env->msg_cache);
	rrset = slabhash_get_mem(&daemon->env->rrset_cache->table);
	val=0;
	iter=0;
	m = modstack_find(&worker->env.mesh->mods, "validator");
	if(m != -1) {
		fptr_ok(fptr_whitelist_mod_get_mem(worker->env.mesh->
			mods.mod[m]->get_mem));
		val = (*worker->env.mesh->mods.mod[m]->get_mem)
			(&worker->env, m);
	}
	m = modstack_find(&worker->env.mesh->mods, "iterator");
	if(m != -1) {
		fptr_ok(fptr_whitelist_mod_get_mem(worker->env.mesh->
			mods.mod[m]->get_mem));
		iter = (*worker->env.mesh->mods.mod[m]->get_mem)
			(&worker->env, m);
	}

	if(!print_longnum(ssl, "mem.cache.rrset"SQ, rrset))
		return 0;
	if(!print_longnum(ssl, "mem.cache.message"SQ, msg))
		return 0;
	if(!print_longnum(ssl, "mem.mod.iterator"SQ, iter))
		return 0;
	if(!print_longnum(ssl, "mem.mod.validator"SQ, val))
		return 0;
	return 1;
}

/** print uptime stats */
static int
print_uptime(SSL* ssl, struct worker* worker, int reset)
{
	struct timeval now = *worker->env.now_tv;
	struct timeval up, dt;
	timeval_subtract(&up, &now, &worker->daemon->time_boot);
	timeval_subtract(&dt, &now, &worker->daemon->time_last_stat);
	if(reset)
		worker->daemon->time_last_stat = now;
	if(!ssl_printf(ssl, "time.now"SQ"%d.%6.6d\n", 
		(unsigned)now.tv_sec, (unsigned)now.tv_usec)) return 0;
	if(!ssl_printf(ssl, "time.up"SQ"%d.%6.6d\n", 
		(unsigned)up.tv_sec, (unsigned)up.tv_usec)) return 0;
	if(!ssl_printf(ssl, "time.elapsed"SQ"%d.%6.6d\n", 
		(unsigned)dt.tv_sec, (unsigned)dt.tv_usec)) return 0;
	return 1;
}

/** print extended histogram */
static int
print_hist(SSL* ssl, struct stats_info* s)
{
	struct timehist* hist;
	size_t i;
	hist = timehist_setup();
	if(!hist) {
		log_err("out of memory");
		return 0;
	}
	timehist_import(hist, s->svr.hist, NUM_BUCKETS_HIST);
	for(i=0; i<hist->num; i++) {
		if(!ssl_printf(ssl, 
			"histogram.%6.6d.%6.6d.to.%6.6d.%6.6d=%u\n",
			(int)hist->buckets[i].lower.tv_sec,
			(int)hist->buckets[i].lower.tv_usec,
			(int)hist->buckets[i].upper.tv_sec,
			(int)hist->buckets[i].upper.tv_usec,
			(unsigned)hist->buckets[i].count)) {
			timehist_delete(hist);
			return 0;
		}
	}
	timehist_delete(hist);
	return 1;
}

/** print extended stats */
static int
print_ext(SSL* ssl, struct stats_info* s)
{
	int i;
	char nm[16];
	const ldns_rr_descriptor* desc;
	const ldns_lookup_table* lt;
	/* TYPE */
	for(i=0; i<STATS_QTYPE_NUM; i++) {
		if(inhibit_zero && s->svr.qtype[i] == 0)
			continue;
		desc = ldns_rr_descript((uint16_t)i);
		if(desc && desc->_name) {
			snprintf(nm, sizeof(nm), "%s", desc->_name);
		} else if (i == LDNS_RR_TYPE_IXFR) {
			snprintf(nm, sizeof(nm), "IXFR");
		} else if (i == LDNS_RR_TYPE_AXFR) {
			snprintf(nm, sizeof(nm), "AXFR");
		} else if (i == LDNS_RR_TYPE_MAILA) {
			snprintf(nm, sizeof(nm), "MAILA");
		} else if (i == LDNS_RR_TYPE_MAILB) {
			snprintf(nm, sizeof(nm), "MAILB");
		} else if (i == LDNS_RR_TYPE_ANY) {
			snprintf(nm, sizeof(nm), "ANY");
		} else {
			snprintf(nm, sizeof(nm), "TYPE%d", i);
		}
		if(!ssl_printf(ssl, "num.query.type.%s"SQ"%u\n", 
			nm, (unsigned)s->svr.qtype[i])) return 0;
	}
	if(!inhibit_zero || s->svr.qtype_big) {
		if(!ssl_printf(ssl, "num.query.type.other"SQ"%u\n", 
			(unsigned)s->svr.qtype_big)) return 0;
	}
	/* CLASS */
	for(i=0; i<STATS_QCLASS_NUM; i++) {
		if(inhibit_zero && s->svr.qclass[i] == 0)
			continue;
		lt = ldns_lookup_by_id(ldns_rr_classes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "CLASS%d", i);
		}
		if(!ssl_printf(ssl, "num.query.class.%s"SQ"%u\n", 
			nm, (unsigned)s->svr.qclass[i])) return 0;
	}
	if(!inhibit_zero || s->svr.qclass_big) {
		if(!ssl_printf(ssl, "num.query.class.other"SQ"%u\n", 
			(unsigned)s->svr.qclass_big)) return 0;
	}
	/* OPCODE */
	for(i=0; i<STATS_OPCODE_NUM; i++) {
		if(inhibit_zero && s->svr.qopcode[i] == 0)
			continue;
		lt = ldns_lookup_by_id(ldns_opcodes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "OPCODE%d", i);
		}
		if(!ssl_printf(ssl, "num.query.opcode.%s"SQ"%u\n", 
			nm, (unsigned)s->svr.qopcode[i])) return 0;
	}
	/* transport */
	if(!ssl_printf(ssl, "num.query.tcp"SQ"%u\n", 
		(unsigned)s->svr.qtcp)) return 0;
	if(!ssl_printf(ssl, "num.query.ipv6"SQ"%u\n", 
		(unsigned)s->svr.qipv6)) return 0;
	/* flags */
	if(!ssl_printf(ssl, "num.query.flags.QR"SQ"%u\n", 
		(unsigned)s->svr.qbit_QR)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.AA"SQ"%u\n", 
		(unsigned)s->svr.qbit_AA)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.TC"SQ"%u\n", 
		(unsigned)s->svr.qbit_TC)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.RD"SQ"%u\n", 
		(unsigned)s->svr.qbit_RD)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.RA"SQ"%u\n", 
		(unsigned)s->svr.qbit_RA)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.Z"SQ"%u\n", 
		(unsigned)s->svr.qbit_Z)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.AD"SQ"%u\n", 
		(unsigned)s->svr.qbit_AD)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.CD"SQ"%u\n", 
		(unsigned)s->svr.qbit_CD)) return 0;
	if(!ssl_printf(ssl, "num.query.edns.present"SQ"%u\n", 
		(unsigned)s->svr.qEDNS)) return 0;
	if(!ssl_printf(ssl, "num.query.edns.DO"SQ"%u\n", 
		(unsigned)s->svr.qEDNS_DO)) return 0;

	/* RCODE */
	for(i=0; i<STATS_RCODE_NUM; i++) {
		if(inhibit_zero && s->svr.ans_rcode[i] == 0)
			continue;
		lt = ldns_lookup_by_id(ldns_rcodes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "RCODE%d", i);
		}
		if(!ssl_printf(ssl, "num.answer.rcode.%s"SQ"%u\n", 
			nm, (unsigned)s->svr.ans_rcode[i])) return 0;
	}
	if(!inhibit_zero || s->svr.ans_rcode_nodata) {
		if(!ssl_printf(ssl, "num.answer.rcode.nodata"SQ"%u\n", 
			(unsigned)s->svr.ans_rcode_nodata)) return 0;
	}
	/* validation */
	if(!ssl_printf(ssl, "num.answer.secure"SQ"%u\n", 
		(unsigned)s->svr.ans_secure)) return 0;
	if(!ssl_printf(ssl, "num.answer.bogus"SQ"%u\n", 
		(unsigned)s->svr.ans_bogus)) return 0;
	if(!ssl_printf(ssl, "num.rrset.bogus"SQ"%u\n", 
		(unsigned)s->svr.rrset_bogus)) return 0;
	/* threat detection */
	if(!ssl_printf(ssl, "unwanted.queries"SQ"%u\n", 
		(unsigned)s->svr.unwanted_queries)) return 0;
	if(!ssl_printf(ssl, "unwanted.replies"SQ"%u\n", 
		(unsigned)s->svr.unwanted_replies)) return 0;
	return 1;
}

/** do the stats command */
static void
do_stats(SSL* ssl, struct daemon_remote* rc, int reset)
{
	struct daemon* daemon = rc->worker->daemon;
	struct stats_info total;
	struct stats_info s;
	int i;
	log_assert(daemon->num > 0);
	/* gather all thread statistics in one place */
	for(i=0; i<daemon->num; i++) {
		server_stats_obtain(rc->worker, daemon->workers[i], &s, reset);
		if(!print_thread_stats(ssl, i, &s))
			return;
		if(i == 0)
			total = s;
		else	server_stats_add(&total, &s);
	}
	/* print the thread statistics */
	total.mesh_time_median /= (double)daemon->num;
	if(!print_stats(ssl, "total", &total)) 
		return;
	if(!print_uptime(ssl, rc->worker, reset))
		return;
	if(daemon->cfg->stat_extended) {
		if(!print_mem(ssl, rc->worker, daemon)) 
			return;
		if(!print_hist(ssl, &total))
			return;
		if(!print_ext(ssl, &total))
			return;
	}
}

/** parse commandline argument domain name */
static int
parse_arg_name(SSL* ssl, char* str, uint8_t** res, size_t* len, int* labs)
{
	ldns_rdf* rdf;
	*res = NULL;
	*len = 0;
	*labs = 0;
	rdf = ldns_dname_new_frm_str(str);
	if(!rdf) {
		ssl_printf(ssl, "error cannot parse name %s\n", str);
		return 0;
	}
	*res = memdup(ldns_rdf_data(rdf), ldns_rdf_size(rdf));
	ldns_rdf_deep_free(rdf);
	if(!*res) {
		ssl_printf(ssl, "error out of memory\n");
		return 0;
	}
	*labs = dname_count_size_labels(*res, len);
	return 1;
}

/** find second argument, modifies string */
static int
find_arg2(SSL* ssl, char* arg, char** arg2)
{
	char* as = strchr(arg, ' ');
	char* at = strchr(arg, '\t');
	if(as && at) {
		if(at < as)
			as = at;
		as[0]=0;
		*arg2 = skipwhite(as+1);
	} else if(as) {
		as[0]=0;
		*arg2 = skipwhite(as+1);
	} else if(at) {
		at[0]=0;
		*arg2 = skipwhite(at+1);
	} else {
		ssl_printf(ssl, "error could not find next argument "
			"after %s\n", arg);
		return 0;
	}
	return 1;
}

/** Add a new zone */
static void
do_zone_add(SSL* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	char* arg2;
	enum localzone_type t;
	struct local_zone* z;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	if(!local_zone_str2type(arg2, &t)) {
		ssl_printf(ssl, "error not a zone type. %s\n", arg2);
		free(nm);
		return;
	}
	lock_quick_lock(&worker->daemon->local_zones->lock);
	if((z=local_zones_find(worker->daemon->local_zones, nm, nmlen, 
		nmlabs, LDNS_RR_CLASS_IN))) {
		/* already present in tree */
		lock_rw_wrlock(&z->lock);
		z->type = t; /* update type anyway */
		lock_rw_unlock(&z->lock);
		free(nm);
		lock_quick_unlock(&worker->daemon->local_zones->lock);
		send_ok(ssl);
		return;
	}
	if(!local_zones_add_zone(worker->daemon->local_zones, nm, nmlen, 
		nmlabs, LDNS_RR_CLASS_IN, t)) {
		lock_quick_unlock(&worker->daemon->local_zones->lock);
		ssl_printf(ssl, "error out of memory\n");
		return;
	}
	lock_quick_unlock(&worker->daemon->local_zones->lock);
	send_ok(ssl);
}

/** Remove a zone */
static void
do_zone_remove(SSL* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	struct local_zone* z;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	lock_quick_lock(&worker->daemon->local_zones->lock);
	if((z=local_zones_find(worker->daemon->local_zones, nm, nmlen, 
		nmlabs, LDNS_RR_CLASS_IN))) {
		/* present in tree */
		local_zones_del_zone(worker->daemon->local_zones, z);
	}
	lock_quick_unlock(&worker->daemon->local_zones->lock);
	free(nm);
	send_ok(ssl);
}

/** Add new RR data */
static void
do_data_add(SSL* ssl, struct worker* worker, char* arg)
{
	if(!local_zones_add_RR(worker->daemon->local_zones, arg,
		worker->env.scratch_buffer)) {
		ssl_printf(ssl,"error in syntax or out of memory, %s\n", arg);
		return;
	}
	send_ok(ssl);
}

/** Remove RR data */
static void
do_data_remove(SSL* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	local_zones_del_data(worker->daemon->local_zones, nm,
		nmlen, nmlabs, LDNS_RR_CLASS_IN);
	free(nm);
	send_ok(ssl);
}

/** cache lookup of nameservers */
static void
do_lookup(SSL* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	(void)print_deleg_lookup(ssl, worker, nm, nmlen, nmlabs);
	free(nm);
}

/** flush something from rrset and msg caches */
static void
do_cache_remove(struct worker* worker, uint8_t* nm, size_t nmlen,
	uint16_t t, uint16_t c)
{
	hashvalue_t h;
	struct query_info k;
	rrset_cache_remove(worker->env.rrset_cache, nm, nmlen, t, c, 0);
	if(t == LDNS_RR_TYPE_SOA)
		rrset_cache_remove(worker->env.rrset_cache, nm, nmlen, t, c,
			PACKED_RRSET_SOA_NEG);
	k.qname = nm;
	k.qname_len = nmlen;
	k.qtype = t;
	k.qclass = c;
	h = query_info_hash(&k);
	slabhash_remove(worker->env.msg_cache, h, &k);
}

/** flush a type */
static void
do_flush_type(SSL* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	char* arg2;
	uint16_t t;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	t = ldns_get_rr_type_by_name(arg2);
	do_cache_remove(worker, nm, nmlen, t, LDNS_RR_CLASS_IN);
	
	free(nm);
	send_ok(ssl);
}

/** flush statistics */
static void
do_flush_stats(SSL* ssl, struct worker* worker)
{
	worker_stats_clear(worker);
	send_ok(ssl);
}

/** flush infra cache */
static void
do_flush_infra(SSL* ssl, struct worker* worker, char* arg)
{
	struct sockaddr_storage addr;
	socklen_t len;
	if(strcmp(arg, "all") == 0) {
		slabhash_clear(worker->env.infra_cache->hosts);
		send_ok(ssl);
		return;
	}
	if(!ipstrtoaddr(arg, UNBOUND_DNS_PORT, &addr, &len)) {
		(void)ssl_printf(ssl, "error parsing ip addr: '%s'\n", arg);
		return;
	}
	infra_remove_host(worker->env.infra_cache, &addr, len);
	send_ok(ssl);
}

/** flush requestlist */
static void
do_flush_requestlist(SSL* ssl, struct worker* worker)
{
	mesh_delete_all(worker->env.mesh);
	send_ok(ssl);
}

/**
 * Local info for deletion functions
 */
struct del_info {
	/** worker */
	struct worker* worker;
	/** name to delete */
	uint8_t* name;
	/** length */
	size_t len;
	/** labels */
	int labs;
	/** now */
	uint32_t now;
	/** time to invalidate to */
	uint32_t expired;
	/** number of rrsets removed */
	size_t num_rrsets;
	/** number of msgs removed */
	size_t num_msgs;
	/** number of key entries removed */
	size_t num_keys;
};

/** callback to delete rrsets in a zone */
static void
zone_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct ub_packed_rrset_key* k = (struct ub_packed_rrset_key*)e->key;
	if(dname_subdomain_c(k->rk.dname, inf->name)) {
		struct packed_rrset_data* d = 
			(struct packed_rrset_data*)e->data;
		if(d->ttl >= inf->now) {
			d->ttl = inf->expired;
			inf->num_rrsets++;
		}
	}
}

/** callback to delete messages in a zone */
static void
zone_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct msgreply_entry* k = (struct msgreply_entry*)e->key;
	if(dname_subdomain_c(k->key.qname, inf->name)) {
		struct reply_info* d = (struct reply_info*)e->data;
		if(d->ttl >= inf->now) {
			d->ttl = inf->expired;
			inf->num_msgs++;
		}
	}
}

/** callback to delete keys in zone */
static void
zone_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_key* k = (struct key_entry_key*)e->key;
	if(dname_subdomain_c(k->name, inf->name)) {
		struct key_entry_data* d = (struct key_entry_data*)e->data;
		if(d->ttl >= inf->now) {
			d->ttl = inf->expired;
			inf->num_keys++;
		}
	}
}

/** remove all rrsets and keys from zone from cache */
static void
do_flush_zone(SSL* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	struct del_info inf;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	/* delete all RRs and key entries from zone */
	/* what we do is to set them all expired */
	inf.worker = worker;
	inf.name = nm;
	inf.len = nmlen;
	inf.labs = nmlabs;
	inf.now = *worker->env.now;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;
	slabhash_traverse(&worker->env.rrset_cache->table, 1, 
		&zone_del_rrset, &inf);

	slabhash_traverse(worker->env.msg_cache, 1, &zone_del_msg, &inf);

	/* and validator cache */
	if(worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab, 1, 
			&zone_del_kcache, &inf);
	}

	free(nm);

	(void)ssl_printf(ssl, "ok removed %u rrsets, %u messages "
		"and %u key entries\n", (unsigned)inf.num_rrsets, 
		(unsigned)inf.num_msgs, (unsigned)inf.num_keys);
}

/** remove name rrset from cache */
static void
do_flush_name(SSL* ssl, struct worker* w, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_AAAA, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_SOA, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_CNAME, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_DNAME, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_MX, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_PTR, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_SRV, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_NAPTR, LDNS_RR_CLASS_IN);
	
	free(nm);
	send_ok(ssl);
}

/** printout a delegation point info */
static int
ssl_print_name_dp(SSL* ssl, char* str, uint8_t* nm, uint16_t dclass,
	struct delegpt* dp)
{
	char buf[257];
	struct delegpt_ns* ns;
	struct delegpt_addr* a;
	int f = 0;
	if(str) { /* print header for forward, stub */
		char* c = ldns_rr_class2str(dclass);
		dname_str(nm, buf);
		if(!ssl_printf(ssl, "%s %s %s: ", buf, c, str)) {
			free(c);
			return 0;
		}
		free(c);
	}
	for(ns = dp->nslist; ns; ns = ns->next) {
		dname_str(ns->name, buf);
		if(!ssl_printf(ssl, "%s%s", (f?" ":""), buf))
			return 0;
		f = 1;
	}
	for(a = dp->target_list; a; a = a->next_target) {
		addr_to_str(&a->addr, a->addrlen, buf, sizeof(buf));
		if(!ssl_printf(ssl, "%s%s", (f?" ":""), buf))
			return 0;
		f = 1;
	}
	return ssl_printf(ssl, "\n");
}


/** print root forwards */
static int
print_root_fwds(SSL* ssl, struct iter_forwards* fwds, uint8_t* root)
{
	struct delegpt* dp;
	dp = forwards_lookup(fwds, root, LDNS_RR_CLASS_IN);
	if(!dp)
		return ssl_printf(ssl, "off (using root hints)\n");
	/* if dp is returned it must be the root */
	log_assert(query_dname_compare(dp->name, root)==0);
	return ssl_print_name_dp(ssl, NULL, root, LDNS_RR_CLASS_IN, dp);
}

/** parse args into delegpt */
static struct delegpt*
parse_delegpt(SSL* ssl, struct regional* region, char* args, uint8_t* root)
{
	/* parse args and add in */
	char* p = args;
	char* todo;
	struct delegpt* dp = delegpt_create(region);
	struct sockaddr_storage addr;
	socklen_t addrlen;
	if(!dp || !delegpt_set_name(dp, region, root)) {
		(void)ssl_printf(ssl, "error out of memory\n");
		return NULL;
	}
	while(p) {
		todo = p;
		p = strchr(p, ' '); /* find next spot, if any */
		if(p) {
			*p++ = 0;	/* end this spot */
			p = skipwhite(p); /* position at next spot */
		}
		/* parse address */
		if(!extstrtoaddr(todo, &addr, &addrlen)) {
			(void)ssl_printf(ssl, "error cannot parse"
				" IP address '%s'\n", todo);
			return NULL;
		}
		/* add address */
		if(!delegpt_add_addr(dp, region, &addr, addrlen, 0, 0)) {
			(void)ssl_printf(ssl, "error out of memory\n");
			return NULL;
		}
	}
	return dp;
}

/** do the status command */
static void
do_forward(SSL* ssl, struct worker* worker, char* args)
{
	struct iter_forwards* fwd = worker->env.fwds;
	uint8_t* root = (uint8_t*)"\000";
	if(!fwd) {
		(void)ssl_printf(ssl, "error: structure not allocated\n");
		return;
	}
	if(args == NULL || args[0] == 0) {
		(void)print_root_fwds(ssl, fwd, root);
		return;
	}
	/* set root forwards for this thread. since we are in remote control
	 * the actual mesh is not running, so we can freely edit it. */
	/* delete all the existing queries first */
	mesh_delete_all(worker->env.mesh);
	/* reset the fwd structure ; the cfg is unchanged (shared by threads)*/
	/* this reset frees up memory */
	forwards_apply_cfg(fwd, worker->env.cfg);
	if(strcmp(args, "off") == 0) {
		forwards_delete_zone(fwd, LDNS_RR_CLASS_IN, root);
	} else {
		struct delegpt* dp;
		if(!(dp = parse_delegpt(ssl, fwd->region, args, root)))
			return;
		if(!forwards_add_zone(fwd, LDNS_RR_CLASS_IN, dp)) {
			(void)ssl_printf(ssl, "error out of memory\n");
			return;
		}
	}
	send_ok(ssl);
}

/** do the status command */
static void
do_status(SSL* ssl, struct worker* worker)
{
	int i;
	time_t uptime;
	if(!ssl_printf(ssl, "version: %s\n", PACKAGE_VERSION))
		return;
	if(!ssl_printf(ssl, "verbosity: %d\n", verbosity))
		return;
	if(!ssl_printf(ssl, "threads: %d\n", worker->daemon->num))
		return;
	if(!ssl_printf(ssl, "modules: %d [", worker->daemon->mods.num))
		return;
	for(i=0; i<worker->daemon->mods.num; i++) {
		if(!ssl_printf(ssl, " %s", worker->daemon->mods.mod[i]->name))
			return;
	}
	if(!ssl_printf(ssl, " ]\n"))
		return;
	uptime = (time_t)time(NULL) - (time_t)worker->daemon->time_boot.tv_sec;
	if(!ssl_printf(ssl, "uptime: %u seconds\n", (unsigned)uptime))
		return;
	if(!ssl_printf(ssl, "unbound (pid %d) is running...\n",
		(int)getpid()))
		return;
}

/** get age for the mesh state */
static void
get_mesh_age(struct mesh_state* m, char* buf, size_t len, 
	struct module_env* env)
{
	if(m->reply_list) {
		struct timeval d;
		struct mesh_reply* r = m->reply_list;
		/* last reply is the oldest */
		while(r && r->next)
			r = r->next;
		timeval_subtract(&d, env->now_tv, &r->start_time);
		snprintf(buf, len, "%d.%6.6d", (int)d.tv_sec, (int)d.tv_usec);
	} else {
		snprintf(buf, len, "-");
	}
}

/** get status of a mesh state */
static void
get_mesh_status(struct mesh_area* mesh, struct mesh_state* m, 
	char* buf, size_t len)
{
	enum module_ext_state s = m->s.ext_state[m->s.curmod];
	const char *modname = mesh->mods.mod[m->s.curmod]->name;
	size_t l;
	if(strcmp(modname, "iterator") == 0 && s == module_wait_reply &&
		m->s.minfo[m->s.curmod]) {
		/* break into iterator to find out who its waiting for */
		struct iter_qstate* qstate = (struct iter_qstate*)
			m->s.minfo[m->s.curmod];
		struct outbound_list* ol = &qstate->outlist;
		struct outbound_entry* e;
		snprintf(buf, len, "%s wait for", modname);
		l = strlen(buf);
		buf += l; len -= l;
		if(ol->first == NULL)
			snprintf(buf, len, " (empty_list)");
		for(e = ol->first; e; e = e->next) {
			snprintf(buf, len, " ");
			l = strlen(buf);
			buf += l; len -= l;
			addr_to_str(&e->qsent->addr, e->qsent->addrlen, 
				buf, len);
			l = strlen(buf);
			buf += l; len -= l;
		}
	} else if(s == module_wait_subquery) {
		/* look in subs from mesh state to see what */
		char nm[257];
		struct mesh_state_ref* sub;
		snprintf(buf, len, "%s wants", modname);
		l = strlen(buf);
		buf += l; len -= l;
		if(m->sub_set.count == 0)
			snprintf(buf, len, " (empty_list)");
		RBTREE_FOR(sub, struct mesh_state_ref*, &m->sub_set) {
			char* t = ldns_rr_type2str(sub->s->s.qinfo.qtype);
			char* c = ldns_rr_class2str(sub->s->s.qinfo.qclass);
			dname_str(sub->s->s.qinfo.qname, nm);
			snprintf(buf, len, " %s %s %s", t, c, nm);
			l = strlen(buf);
			buf += l; len -= l;
			free(t);
			free(c);
		}
	} else {
		snprintf(buf, len, "%s is %s", modname, strextstate(s));
	}
}

/** do the dump_requestlist command */
static void
do_dump_requestlist(SSL* ssl, struct worker* worker)
{
	struct mesh_area* mesh;
	struct mesh_state* m;
	int num = 0;
	char buf[257];
	char timebuf[32];
	char statbuf[10240];
	if(!ssl_printf(ssl, "thread #%d\n", worker->thread_num))
		return;
	if(!ssl_printf(ssl, "#   type cl name    seconds    module status\n"))
		return;
	/* show worker mesh contents */
	mesh = worker->env.mesh;
	if(!mesh) return;
	RBTREE_FOR(m, struct mesh_state*, &mesh->all) {
		char* t = ldns_rr_type2str(m->s.qinfo.qtype);
		char* c = ldns_rr_class2str(m->s.qinfo.qclass);
		dname_str(m->s.qinfo.qname, buf);
		get_mesh_age(m, timebuf, sizeof(timebuf), &worker->env);
		get_mesh_status(mesh, m, statbuf, sizeof(statbuf));
		if(!ssl_printf(ssl, "%3d %4s %2s %s %s %s\n", 
			num, t, c, buf, timebuf, statbuf)) {
			free(t);
			free(c);
			return;
		}
		num++;
		free(t);
		free(c);
	}
}

/** structure for argument data for dump infra host */
struct infra_arg {
	/** the infra cache */
	struct infra_cache* infra;
	/** the SSL connection */
	SSL* ssl;
	/** the time now */
	uint32_t now;
	/** ipstr */
	char* ipstr;
};

/** callback for every lame element in the infra cache */
static void
dump_infra_lame(struct lruhash_entry* e, void* arg)
{
	struct infra_arg* a = (struct infra_arg*)arg;
	struct infra_lame_key* k = (struct infra_lame_key*)e->key;
	struct infra_lame_data* d = (struct infra_lame_data*)e->data;
	ldns_rdf* rdf;
	size_t pos = 0;
	char* nm;
	/* skip expired */
	if(d->ttl < a->now) {
		return;
	}
	/* use ldns print for domain name */
	if(ldns_wire2dname(&rdf, k->zonename, k->namelen, &pos)
		!= LDNS_STATUS_OK)
		return;
	nm = ldns_rdf2str(rdf);
	ldns_rdf_deep_free(rdf);
	if(!ssl_printf(a->ssl, "%s lame %s ttl %d dnssec %d rec %d "
		"A %d other %d\n", a->ipstr, nm, (int)(d->ttl - a->now),
		d->isdnsseclame, d->rec_lame, d->lame_type_A, d->lame_other)) {
		free(nm);
		return;
	}
	free(nm);
}

/** callback for every host element in the infra cache */
static void
dump_infra_host(struct lruhash_entry* e, void* arg)
{
	struct infra_arg* a = (struct infra_arg*)arg;
	struct infra_host_key* k = (struct infra_host_key*)e->key;
	struct infra_host_data* d = (struct infra_host_data*)e->data;
	char ip_str[1024];
	addr_to_str(&k->addr, k->addrlen, ip_str, sizeof(ip_str));
	a->ipstr = ip_str;
	/* skip expired stuff (only backed off) */
	if(d->ttl < a->now) {
		if(d->rtt.rto >= USEFUL_SERVER_TOP_TIMEOUT) {
			if(!ssl_printf(a->ssl, "%s expired rto %d\n", ip_str,
				d->rtt.rto)) return;
		}
		if(d->lameness)
			lruhash_traverse(d->lameness, 0, &dump_infra_lame, arg);
		return;
	}
	if(!ssl_printf(a->ssl, "%s ttl %d ping %d var %d rtt %d rto %d "
		"ednsknown %d edns %d delay %d\n",
		ip_str, (int)(d->ttl - a->now),
		d->rtt.srtt, d->rtt.rttvar, rtt_notimeout(&d->rtt), d->rtt.rto,
		(int)d->edns_lame_known, (int)d->edns_version,
		(int)(a->now<d->probedelay?d->probedelay-a->now:0)))
		return;
	if(d->lameness)
		lruhash_traverse(d->lameness, 0, &dump_infra_lame, arg);
}

/** do the dump_infra command */
static void
do_dump_infra(SSL* ssl, struct worker* worker)
{
	struct infra_arg arg;
	arg.infra = worker->env.infra_cache;
	arg.ssl = ssl;
	arg.now = *worker->env.now;
	slabhash_traverse(arg.infra->hosts, 0, &dump_infra_host, (void*)&arg);
}

/** do the log_reopen command */
static void
do_log_reopen(SSL* ssl, struct worker* worker)
{
	struct config_file* cfg = worker->env.cfg;
	send_ok(ssl);
	log_init(cfg->logfile, cfg->use_syslog, cfg->chrootdir);
}

/** do the set_option command */
static void
do_set_option(SSL* ssl, struct worker* worker, char* arg)
{
	char* arg2;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	if(!config_set_option(worker->env.cfg, arg, arg2)) {
		(void)ssl_printf(ssl, "error setting option\n");
		return;
	}
	send_ok(ssl);
}

/* routine to printout option values over SSL */
void remote_get_opt_ssl(char* line, void* arg)
{
	SSL* ssl = (SSL*)arg;
	(void)ssl_printf(ssl, "%s\n", line);
}

/** do the get_option command */
static void
do_get_option(SSL* ssl, struct worker* worker, char* arg)
{
	int r;
	r = config_get_option(worker->env.cfg, arg, remote_get_opt_ssl, ssl);
	if(!r) {
		(void)ssl_printf(ssl, "error unknown option\n");
		return;
	}
}

/** do the list_forwards command */
static void
do_list_forwards(SSL* ssl, struct worker* worker)
{
	/* since its a per-worker structure no locks needed */
	struct iter_forwards* fwds = worker->env.fwds;
	struct iter_forward_zone* z;
	RBTREE_FOR(z, struct iter_forward_zone*, fwds->tree) {
		if(!z->dp) continue; /* skip empty marker for stub */
		if(!ssl_print_name_dp(ssl, "forward", z->name, z->dclass,
			z->dp))
			return;
	}
}

/** do the list_stubs command */
static void
do_list_stubs(SSL* ssl, struct worker* worker)
{
	/* readonly structure */
	int m;
	struct iter_hints_stub* z;
	struct iter_env* ie;
	m = modstack_find(&worker->env.mesh->mods, "iterator");
	if(m == -1) {
		(void)ssl_printf(ssl, "error no iterator module\n");
		return;
	}
	ie = (struct iter_env*)worker->env.modinfo[m];
	RBTREE_FOR(z, struct iter_hints_stub*, &ie->hints->tree) {
		if(!ssl_print_name_dp(ssl, 
			z->noprime?"stub noprime":"stub prime", z->node.name,
			z->node.dclass, z->dp))
			return;
	}
}

/** do the list_local_zones command */
static void
do_list_local_zones(SSL* ssl, struct worker* worker)
{
	struct local_zones* zones = worker->daemon->local_zones;
	struct local_zone* z;
	char buf[257];
	lock_quick_lock(&zones->lock);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		dname_str(z->name, buf);
		(void)ssl_printf(ssl, "%s %s\n", buf, 
			local_zone_type2str(z->type));
		lock_rw_unlock(&z->lock);
	}
	lock_quick_unlock(&zones->lock);
}

/** do the list_local_data command */
static void
do_list_local_data(SSL* ssl, struct worker* worker)
{
	struct local_zones* zones = worker->daemon->local_zones;
	struct local_zone* z;
	struct local_data* d;
	struct local_rrset* p;
	lock_quick_lock(&zones->lock);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		RBTREE_FOR(d, struct local_data*, &z->data) {
			for(p = d->rrsets; p; p = p->next) {
				ldns_rr_list* rr = packed_rrset_to_rr_list(
					p->rrset, worker->env.scratch_buffer);
				char* str = ldns_rr_list2str(rr);
				(void)ssl_printf(ssl, "%s", str);
				free(str);
				ldns_rr_list_free(rr);
			}
		}
		lock_rw_unlock(&z->lock);
	}
	lock_quick_unlock(&zones->lock);
}

/** tell other processes to execute the command */
static void
distribute_cmd(struct daemon_remote* rc, SSL* ssl, char* cmd)
{
	int i;
	if(!cmd || !ssl) 
		return;
	/* skip i=0 which is me */
	for(i=1; i<rc->worker->daemon->num; i++) {
		worker_send_cmd(rc->worker->daemon->workers[i],
			worker_cmd_remote);
		if(!tube_write_msg(rc->worker->daemon->workers[i]->cmd,
			(uint8_t*)cmd, strlen(cmd)+1, 0)) {
			ssl_printf(ssl, "error could not distribute cmd\n");
			return;
		}
	}
}

/** check for name with end-of-string, space or tab after it */
static int
cmdcmp(char* p, const char* cmd, size_t len)
{
	return strncmp(p,cmd,len)==0 && (p[len]==0||p[len]==' '||p[len]=='\t');
}

/** execute a remote control command */
static void
execute_cmd(struct daemon_remote* rc, SSL* ssl, char* cmd, 
	struct worker* worker)
{
	char* p = skipwhite(cmd);
	/* compare command */
	if(cmdcmp(p, "stop", 4)) {
		do_stop(ssl, rc);
		return;
	} else if(cmdcmp(p, "reload", 6)) {
		do_reload(ssl, rc);
		return;
	} else if(cmdcmp(p, "stats_noreset", 13)) {
		do_stats(ssl, rc, 0);
		return;
	} else if(cmdcmp(p, "stats", 5)) {
		do_stats(ssl, rc, 1);
		return;
	} else if(cmdcmp(p, "status", 6)) {
		do_status(ssl, worker);
		return;
	} else if(cmdcmp(p, "dump_cache", 10)) {
		(void)dump_cache(ssl, worker);
		return;
	} else if(cmdcmp(p, "load_cache", 10)) {
		if(load_cache(ssl, worker)) send_ok(ssl);
		return;
	} else if(cmdcmp(p, "list_forwards", 13)) {
		do_list_forwards(ssl, worker);
		return;
	} else if(cmdcmp(p, "list_stubs", 10)) {
		do_list_stubs(ssl, worker);
		return;
	} else if(cmdcmp(p, "list_local_zones", 16)) {
		do_list_local_zones(ssl, worker);
		return;
	} else if(cmdcmp(p, "list_local_data", 15)) {
		do_list_local_data(ssl, worker);
		return;
	} else if(cmdcmp(p, "forward", 7)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_forward(ssl, worker, skipwhite(p+7));
		return;
	} else if(cmdcmp(p, "flush_stats", 11)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_flush_stats(ssl, worker);
		return;
	} else if(cmdcmp(p, "flush_requestlist", 17)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_flush_requestlist(ssl, worker);
		return;
	} else if(cmdcmp(p, "lookup", 6)) {
		do_lookup(ssl, worker, skipwhite(p+6));
		return;
	}

#ifdef THREADS_DISABLED
	/* other processes must execute the command as well */
	/* commands that should not be distributed, returned above. */
	if(rc) { /* only if this thread is the master (rc) thread */
		/* done before the code below, which may split the string */
		distribute_cmd(rc, ssl, cmd);
	}
#endif
	if(cmdcmp(p, "verbosity", 9)) {
		do_verbosity(ssl, skipwhite(p+9));
	} else if(cmdcmp(p, "local_zone_remove", 17)) {
		do_zone_remove(ssl, worker, skipwhite(p+17));
	} else if(cmdcmp(p, "local_zone", 10)) {
		do_zone_add(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "local_data_remove", 17)) {
		do_data_remove(ssl, worker, skipwhite(p+17));
	} else if(cmdcmp(p, "local_data", 10)) {
		do_data_add(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "flush_zone", 10)) {
		do_flush_zone(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "flush_type", 10)) {
		do_flush_type(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "flush_infra", 11)) {
		do_flush_infra(ssl, worker, skipwhite(p+11));
	} else if(cmdcmp(p, "flush", 5)) {
		do_flush_name(ssl, worker, skipwhite(p+5));
	} else if(cmdcmp(p, "dump_requestlist", 16)) {
		do_dump_requestlist(ssl, worker);
	} else if(cmdcmp(p, "dump_infra", 10)) {
		do_dump_infra(ssl, worker);
	} else if(cmdcmp(p, "log_reopen", 10)) {
		do_log_reopen(ssl, worker);
	} else if(cmdcmp(p, "set_option", 10)) {
		do_set_option(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "get_option", 10)) {
		do_get_option(ssl, worker, skipwhite(p+10));
	} else {
		(void)ssl_printf(ssl, "error unknown command '%s'\n", p);
	}
}

void 
daemon_remote_exec(struct worker* worker)
{
	/* read the cmd string */
	uint8_t* msg = NULL;
	uint32_t len = 0;
	if(!tube_read_msg(worker->cmd, &msg, &len, 0)) {
		log_err("daemon_remote_exec: tube_read_msg failed");
		return;
	}
	verbose(VERB_ALGO, "remote exec distributed: %s", (char*)msg);
	execute_cmd(NULL, NULL, (char*)msg, worker);
	free(msg);
}

/** handle remote control request */
static void
handle_req(struct daemon_remote* rc, struct rc_state* s, SSL* ssl)
{
	int r;
	char pre[10];
	char magic[7];
	char buf[1024];
#ifdef USE_WINSOCK
	/* makes it possible to set the socket blocking again. */
	/* basically removes it from winsock_event ... */
	WSAEventSelect(s->c->fd, NULL, 0);
#endif
	fd_set_block(s->c->fd);

	/* try to read magic UBCT[version]_space_ string */
	ERR_clear_error();
	if((r=SSL_read(ssl, magic, (int)sizeof(magic)-1)) <= 0) {
		if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN)
			return;
		log_crypto_err("could not SSL_read");
		return;
	}
	magic[6] = 0;
	if( r != 6 || strncmp(magic, "UBCT", 4) != 0) {
		verbose(VERB_QUERY, "control connection has bad magic string");
		/* probably wrong tool connected, ignore it completely */
		return;
	}

	/* read the command line */
	if(!ssl_read_line(ssl, buf, sizeof(buf))) {
		return;
	}
	snprintf(pre, sizeof(pre), "UBCT%d ", UNBOUND_CONTROL_VERSION);
	if(strcmp(magic, pre) != 0) {
		verbose(VERB_QUERY, "control connection had bad "
			"version %s, cmd: %s", magic, buf);
		ssl_printf(ssl, "error version mismatch\n");
		return;
	}
	verbose(VERB_DETAIL, "control cmd: %s", buf);

	/* figure out what to do */
	execute_cmd(rc, ssl, buf, rc->worker);
}

int remote_control_callback(struct comm_point* c, void* arg, int err, 
	struct comm_reply* ATTR_UNUSED(rep))
{
	struct rc_state* s = (struct rc_state*)arg;
	struct daemon_remote* rc = s->rc;
	int r;
	if(err != NETEVENT_NOERROR) {
		if(err==NETEVENT_TIMEOUT) 
			log_err("remote control timed out");
		clean_point(rc, s);
		return 0;
	}
	/* (continue to) setup the SSL connection */
	ERR_clear_error();
	r = SSL_do_handshake(s->ssl);
	if(r != 1) {
		int r2 = SSL_get_error(s->ssl, r);
		if(r2 == SSL_ERROR_WANT_READ) {
			if(s->shake_state == rc_hs_read) {
				/* try again later */
				return 0;
			}
			s->shake_state = rc_hs_read;
			comm_point_listen_for_rw(c, 1, 0);
			return 0;
		} else if(r2 == SSL_ERROR_WANT_WRITE) {
			if(s->shake_state == rc_hs_write) {
				/* try again later */
				return 0;
			}
			s->shake_state = rc_hs_write;
			comm_point_listen_for_rw(c, 0, 1);
			return 0;
		} else {
			if(r == 0)
				log_err("remote control connection closed prematurely");
			log_addr(1, "failed connection from",
				&s->c->repinfo.addr, s->c->repinfo.addrlen);
			log_crypto_err("remote control failed ssl");
			clean_point(rc, s);
			return 0;
		}
	}
	s->shake_state = rc_none;

	/* once handshake has completed, check authentication */
	if(SSL_get_verify_result(s->ssl) == X509_V_OK) {
		X509* x = SSL_get_peer_certificate(s->ssl);
		if(!x) {
			verbose(VERB_DETAIL, "remote control connection "
				"provided no client certificate");
			clean_point(rc, s);
			return 0;
		}
		verbose(VERB_ALGO, "remote control connection authenticated");
		X509_free(x);
	} else {
		verbose(VERB_DETAIL, "remote control connection failed to "
			"authenticate with client certificate");
		clean_point(rc, s);
		return 0;
	}

	/* if OK start to actually handle the request */
	handle_req(rc, s, s->ssl);

	verbose(VERB_ALGO, "remote control operation completed");
	clean_point(rc, s);
	return 0;
}

#endif /* end of TODO */

#endif /* HAVE_SSL */
