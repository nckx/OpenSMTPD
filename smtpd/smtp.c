/*	$OpenBSD: smtp.c,v 1.166 2019/08/10 16:07:01 gilles Exp $	*/

/*
 * Copyright (c) 2008 Gilles Chehade <gilles@poolp.org>
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tls.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "smtpd.h"
#include "log.h"
#include "ssl.h"

static void smtp_setup_events(void);
static void smtp_pause(void);
static void smtp_resume(void);
static void smtp_accept(int, short, void *);
static void smtp_dropped(struct listener *, int, const struct sockaddr_storage *);
static int smtp_enqueue(void);
static int smtp_can_accept(void);
static void smtp_setup_listeners(void);

int
proxy_session(struct listener *listener, int sock,
    const struct sockaddr_storage *ss,
    void (*accepted)(struct listener *, int,
	const struct sockaddr_storage *, struct io *),
    void (*dropped)(struct listener *, int,
	const struct sockaddr_storage *));

static void smtp_accepted(struct listener *, int, const struct sockaddr_storage *, struct io *);

/*
 * This function is not publicy exported because it is a hack until libtls
 * has a proper privsep setup
 */
void tls_config_skip_private_key_check(struct tls_config *config);

#define	SMTP_FD_RESERVE	5
static size_t	sessions;
static size_t	maxsessions;

void
smtp_imsg(struct mproc *p, struct imsg *imsg)
{
	switch (imsg->hdr.type) {
	case IMSG_SMTP_CHECK_SENDER:
	case IMSG_SMTP_EXPAND_RCPT:
	case IMSG_SMTP_LOOKUP_HELO:
	case IMSG_SMTP_AUTHENTICATE:
	case IMSG_FILTER_SMTP_PROTOCOL:
	case IMSG_FILTER_SMTP_DATA_BEGIN:
		smtp_session_imsg(p, imsg);
		return;

	case IMSG_SMTP_MESSAGE_COMMIT:
	case IMSG_SMTP_MESSAGE_CREATE:
	case IMSG_SMTP_MESSAGE_OPEN:
	case IMSG_QUEUE_ENVELOPE_SUBMIT:
	case IMSG_QUEUE_ENVELOPE_COMMIT:
		smtp_session_imsg(p, imsg);
		return;

	case IMSG_QUEUE_SMTP_SESSION:
		m_compose(p, IMSG_QUEUE_SMTP_SESSION, 0, 0, smtp_enqueue(),
		    imsg->data, imsg->hdr.len - sizeof imsg->hdr);
		return;

	case IMSG_CTL_SMTP_SESSION:
		m_compose(p, IMSG_CTL_SMTP_SESSION, imsg->hdr.peerid, 0,
		    smtp_enqueue(), NULL, 0);
		return;

	case IMSG_CTL_PAUSE_SMTP:
		log_debug("debug: smtp: pausing listening sockets");
		smtp_pause();
		env->sc_flags |= SMTPD_SMTP_PAUSED;
		return;

	case IMSG_CTL_RESUME_SMTP:
		log_debug("debug: smtp: resuming listening sockets");
		env->sc_flags &= ~SMTPD_SMTP_PAUSED;
		smtp_resume();
		return;
	}

	errx(1, "smtp_imsg: unexpected %s imsg", imsg_to_str(imsg->hdr.type));
}

void
smtp_postfork(void)
{
	smtp_setup_listeners();
}

void
smtp_postprivdrop(void)
{
}

void
smtp_configure(void)
{
	smtp_setup_events();
}

static void
smtp_setup_listeners(void)
{
	struct listener	       *l;
	int			opt;

	TAILQ_FOREACH(l, env->sc_listeners, entry) {
		if ((l->fd = socket(l->ss.ss_family, SOCK_STREAM, 0)) == -1) {
			if (errno == EAFNOSUPPORT) {
				log_warn("smtpd: socket");
				continue;
			}
			fatal("smtpd: socket");
		}
		opt = 1;
		if (setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &opt,
		    sizeof(opt)) == -1)
			fatal("smtpd: setsockopt");
		if (bind(l->fd, (struct sockaddr *)&l->ss, l->ss.ss_len) == -1)
			fatal("smtpd: bind");
	}
}

static void
smtp_setup_events(void)
{
	struct listener *l;
	struct pki	*pki;
	struct ca	*ca;
	void		*iter;
	const char	*k;
	const char	*fake_key;
	int		 fake_keylen;
	char		 hash[TLS_CERT_HASH_SIZE];
	char		*p = &hash[0];

	iter = NULL;
	while (dict_iter(env->sc_pki_dict, &iter, &k, (void **)&pki)) {
		if ((fake_keylen = tls_ctx_fake_private_key(pki->pki_cert,
			    pki->pki_cert_len, &fake_key,
			    NULL, &pki->pki_pkey, p)) == -1)
			err(1, "tls_ctx_fake_private_key");

		/* let ca.c create a mapping hash -> pki */
		m_create(p_ca, IMSG_CA_HASH_TO_PKI, 0, 0, -1);
		m_add_string(p_ca, p);
		m_add_string(p_ca, pki->pki_name);
		m_flush(p_ca);
	}

	TAILQ_FOREACH(l, env->sc_listeners, entry) {
		log_debug("debug: smtp: listen on %s port %d flags 0x%01x"
		    " pki \"%s\""
		    " ca \"%s\"", ss_to_text(&l->ss), ntohs(l->port),
		    l->flags, l->pki_name, l->ca_name);

		io_set_nonblocking(l->fd);
		if (listen(l->fd, SMTPD_BACKLOG) == -1)
			fatal("listen");
		event_set(&l->ev, l->fd, EV_READ|EV_PERSIST, smtp_accept, l);

		if (!(env->sc_flags & SMTPD_SMTP_PAUSED))
			event_add(&l->ev, NULL);

		if (l->flags & F_SSL) {
			if ((l->tls_cfg = tls_config_new()) == NULL)
				fatal("smtpd: tls_config_new");

			tls_config_skip_private_key_check(l->tls_cfg);

			if (tls_config_set_ciphers(l->tls_cfg, env->sc_tls_ciphers) == -1)
				err(1, "%s", tls_config_error(l->tls_cfg));

			if (l->pki_name[0])
				pki = dict_get(env->sc_pki_dict, l->pki_name);
			else {
				if (l->hostname[0])
					pki = dict_get(env->sc_pki_dict, l->hostname);
				if (pki == NULL)
					if (env->sc_hostname[0])
						pki = dict_get(env->sc_pki_dict, env->sc_hostname);
				if (pki == NULL)
					pki = dict_get(env->sc_pki_dict, "*");
			}
			if (pki == NULL)
				fatalx("smtpd: could not find valid PKI for listener");

			if (l->ca_name[0])
				ca = dict_get(env->sc_ca_dict, l->ca_name);
			else
				ca = ssl_default_ca();
			tls_config_set_ca_mem(l->tls_cfg,
			    ca->ca_cert,
			    ca->ca_cert_len);

			if ((fake_keylen = tls_ctx_fake_private_key(pki->pki_cert,
				    pki->pki_cert_len, &fake_key,
				    NULL, &pki->pki_pkey, p)) == -1)
				err(1, "tls_ctx_fake_private_key");
			
			if (tls_config_set_keypair_mem(l->tls_cfg,
				pki->pki_cert, pki->pki_cert_len,
				fake_key, fake_keylen) != 0)
				err(1, "%s", tls_config_error(l->tls_cfg));

			if (l->flags & F_TLS_VERIFY)
				tls_config_verify_client(l->tls_cfg);
			//else
			//	tls_config_verify_client_optional(l->tls_cfg);

			//
			// SNI ?
			//if (sni_cb)
			//	SSL_CTX_set_tlsext_servername_callback(ctx, sni_cb);


		}
	}

	purge_config(PURGE_PKI_KEYS);

	maxsessions = (getdtablesize() - getdtablecount()) / 2 - SMTP_FD_RESERVE;
	log_debug("debug: smtp: will accept at most %zu clients", maxsessions);
}

static void
smtp_pause(void)
{
	struct listener *l;

	if (env->sc_flags & (SMTPD_SMTP_DISABLED|SMTPD_SMTP_PAUSED))
		return;

	TAILQ_FOREACH(l, env->sc_listeners, entry)
		event_del(&l->ev);
}

static void
smtp_resume(void)
{
	struct listener *l;

	if (env->sc_flags & (SMTPD_SMTP_DISABLED|SMTPD_SMTP_PAUSED))
		return;

	TAILQ_FOREACH(l, env->sc_listeners, entry)
		event_add(&l->ev, NULL);
}

static int
smtp_enqueue(void)
{
	struct listener	*listener = env->sc_sock_listener;
	int		 fd[2];

	/*
	 * Some enqueue requests buffered in IMSG may still arrive even after
	 * call to smtp_pause() because enqueue listener is not a real socket
	 * and thus cannot be paused properly.
	 */
	if (env->sc_flags & SMTPD_SMTP_PAUSED)
		return (-1);

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, fd))
		return (-1);

	if ((smtp_session(listener, fd[0], &listener->ss, env->sc_hostname, NULL)) == -1) {
		close(fd[0]);
		close(fd[1]);
		return (-1);
	}

	sessions++;
	stat_increment("smtp.session", 1);
	stat_increment("smtp.session.local", 1);

	return (fd[1]);
}

static void
smtp_accept(int fd, short event, void *p)
{
	struct listener		*listener = p;
	struct sockaddr_storage	 ss;
	socklen_t		 len;
	int			 sock;

	if (env->sc_flags & SMTPD_SMTP_PAUSED)
		fatalx("smtp_session: unexpected client");

	if (!smtp_can_accept()) {
		log_warnx("warn: Disabling incoming SMTP connections: "
		    "Client limit reached");
		goto pause;
	}

	len = sizeof(ss);
	if ((sock = accept(fd, (struct sockaddr *)&ss, &len)) == -1) {
		if (errno == ENFILE || errno == EMFILE) {
			log_warn("warn: Disabling incoming SMTP connections");
			goto pause;
		}
		if (errno == EINTR || errno == EWOULDBLOCK ||
		    errno == ECONNABORTED)
			return;
		fatal("smtp_accept");
	}

	if (listener->flags & F_PROXY) {
		io_set_nonblocking(sock);
		if (proxy_session(listener, sock, &ss,
			smtp_accepted, smtp_dropped) == -1) {
			close(sock);
			return;
		}
		return;
	}

	smtp_accepted(listener, sock, &ss, NULL);
	return;

pause:
	smtp_pause();
	env->sc_flags |= SMTPD_SMTP_DISABLED;
	return;
}

static int
smtp_can_accept(void)
{
	if (sessions + 1 == maxsessions)
		return 0;
	return (getdtablesize() - getdtablecount() - SMTP_FD_RESERVE >= 2);
}

void
smtp_collect(void)
{
	sessions--;
	stat_decrement("smtp.session", 1);

	if (!smtp_can_accept())
		return;

	if (env->sc_flags & SMTPD_SMTP_DISABLED) {
		log_warnx("warn: smtp: "
		    "fd exhaustion over, re-enabling incoming connections");
		env->sc_flags &= ~SMTPD_SMTP_DISABLED;
		smtp_resume();
	}
}

static void
smtp_accepted(struct listener *listener, int sock, const struct sockaddr_storage *ss, struct io *io)
{
	int     ret;

	ret = smtp_session(listener, sock, ss, NULL, io);
	if (ret == -1) {
		log_warn("warn: Failed to create SMTP session");
		close(sock);
		return;
	}
	io_set_nonblocking(sock);

	sessions++;
	stat_increment("smtp.session", 1);
	if (listener->ss.ss_family == AF_LOCAL)
		stat_increment("smtp.session.local", 1);
	if (listener->ss.ss_family == AF_INET)
		stat_increment("smtp.session.inet4", 1);
	if (listener->ss.ss_family == AF_INET6)
		stat_increment("smtp.session.inet6", 1);
}

static void
smtp_dropped(struct listener *listener, int sock, const struct sockaddr_storage *ss)
{
	close(sock);
	sessions--;
}
