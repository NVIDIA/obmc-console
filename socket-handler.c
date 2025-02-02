/**
 * Copyright © 2016 IBM Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <endian.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-daemon.h>

#include "console-server.h"

#define SOCKET_HANDLER_PKT_SIZE 512
/* Set poll() timeout to 4000 uS, or 4 mS */
#define SOCKET_HANDLER_PKT_US_TIMEOUT 4000

struct client {
	struct socket_handler *sh;
	struct poller *poller;
	struct ringbuffer_consumer *rbc;
	int fd;
	bool blocked;
};

struct socket_handler {
	struct handler handler;
	struct console *console;
	struct poller *poller;
	int sd;

	struct client **clients;
	int n_clients;
};

static struct timeval const socket_handler_timeout = {
	.tv_sec = 0,
	.tv_usec = SOCKET_HANDLER_PKT_US_TIMEOUT
};

static struct socket_handler *to_socket_handler(struct handler *handler)
{
	return container_of(handler, struct socket_handler, handler);
}

static void client_close(struct client *client)
{
	struct socket_handler *sh = client->sh;
	int idx;

	close(client->fd);
	if (client->poller) {
		console_poller_unregister(sh->console, client->poller);
	}

	if (client->rbc) {
		ringbuffer_consumer_unregister(client->rbc);
	}

	for (idx = 0; idx < sh->n_clients; idx++) {
		if (sh->clients[idx] == client) {
			break;
		}
	}

	assert(idx < sh->n_clients);

	free(client);
	client = NULL;

	sh->n_clients--;
	/*
	 * We're managing an array of pointers to aggregates, so don't warn about sizeof() on a
	 * pointer type.
	 */
	/* NOLINTBEGIN(bugprone-sizeof-expression) */
	memmove(&sh->clients[idx], &sh->clients[idx + 1],
		sizeof(*sh->clients) * (sh->n_clients - idx));
	sh->clients =
		reallocarray(sh->clients, sh->n_clients, sizeof(*sh->clients));
	/* NOLINTEND(bugprone-sizeof-expression) */
}

static void client_set_blocked(struct client *client, bool blocked)
{
	int events;

	if (client->blocked == blocked) {
		return;
	}

	client->blocked = blocked;

	events = POLLIN;
	if (client->blocked) {
		events |= POLLOUT;
	}

	console_poller_set_events(client->sh->console, client->poller, events);
}

static ssize_t send_all(struct client *client, void *buf, size_t len,
			bool block)
{
	int fd;
	int flags;
	ssize_t rc;
	size_t pos;

	if (len > SSIZE_MAX) {
		return -EINVAL;
	}

	fd = client->fd;

	flags = MSG_NOSIGNAL;
	if (!block) {
		flags |= MSG_DONTWAIT;
	}

	for (pos = 0; pos < len; pos += rc) {
		rc = send(fd, (char *)buf + pos, len - pos, flags);
		if (rc < 0) {
			if (!block &&
			    (errno == EAGAIN || errno == EWOULDBLOCK)) {
				client_set_blocked(client, true);
				break;
			}

			if (errno == EINTR) {
				continue;
			}

			return -1;
		}
		if (rc == 0) {
			return -1;
		}
	}

	return (ssize_t)pos;
}

/* Drain the queue to the socket and update the queue buffer. If force_len is
 * set, send at least that many bytes from the queue, possibly while blocking
 */
static int client_drain_queue(struct client *client, size_t force_len)
{
	uint8_t *buf;
	ssize_t wlen;
	size_t len;
	size_t total_len;
	bool block;

	total_len = 0;
	wlen = 0;
	block = !!force_len;

	/* if we're already blocked, no need for the write */
	if (!block && client->blocked) {
		return 0;
	}

	for (;;) {
		len = ringbuffer_dequeue_peek(client->rbc, total_len, &buf);
		if (!len) {
			break;
		}

		wlen = send_all(client, buf, len, block);
		if (wlen <= 0) {
			break;
		}

		total_len += wlen;

		if (force_len && total_len >= force_len) {
			break;
		}
	}

	if (wlen < 0) {
		return -1;
	}

	if (force_len && total_len < force_len) {
		return -1;
	}

	ringbuffer_dequeue_commit(client->rbc, total_len);
	return 0;
}

static enum ringbuffer_poll_ret client_ringbuffer_poll(void *arg,
						       size_t force_len)
{
	struct client *client = arg;
	size_t len;
	int rc;

	len = ringbuffer_len(client->rbc);
	if (!force_len && (len < SOCKET_HANDLER_PKT_SIZE)) {
		/* Do nothing until many small requests have accumulated, or
		 * the UART is idle for awhile (as determined by the timeout
		 * value supplied to the poll function call in console_server.c. */
		console_poller_set_timeout(client->sh->console, client->poller,
					   &socket_handler_timeout);
		return RINGBUFFER_POLL_OK;
	}

	rc = client_drain_queue(client, force_len);
	if (rc) {
		client->rbc = NULL;
		client_close(client);
		return RINGBUFFER_POLL_REMOVE;
	}

	return RINGBUFFER_POLL_OK;
}

static enum poller_ret
client_timeout(struct handler *handler __attribute__((unused)), void *data)
{
	struct client *client = data;
	int rc = 0;

	if (client->blocked) {
		/* nothing to do here, we'll call client_drain_queue when
		 * we become unblocked */
		return POLLER_OK;
	}

	rc = client_drain_queue(client, 0);
	if (rc) {
		client_close(client);
		return POLLER_REMOVE;
	}

	return POLLER_OK;
}

static uint8_t *process_buffer_range(struct socket_handler *sh, uint8_t *begin,
				     uint8_t *end)
{
	static const uint8_t tilde = '~';
	uint8_t *cursor = NULL;

	/* Caller to enforce */
	assert(begin < end);

	/*
	 * SSH-style escape sequence handling: <newline><leader><descriminator>
	 *
	 * This may look like:
	 *
	 * - \n~B
	 * - \r~B
	 * - \r\n~B
	 * - \n~~
	 * - etc
	 */
	switch (sh->console->state) {
	case escape_idle:
		/* Handle \r, \n, and \r\n by searching for \r first */
		if ((cursor = memchr(begin, '\r', end - begin))) {
			sh->console->state = escape_cr;
			/* Include the newline in the output */
			cursor += 1;
		} else if ((cursor = memchr(begin, '\n', end - begin))) {
			sh->console->state = escape_lf;
			/* Include the newline in the output */
			cursor += 1;
		} else {
			cursor = end;
		}
		console_data_out(sh->console, begin, cursor - begin);
		return cursor;
	case escape_cr:
		cursor = begin;
		switch (*cursor) {
		case '\n':
			/* Ensure \r\n new line sequences are emitted too */
			sh->console->state = escape_lf;
			cursor++;
			console_data_out(sh->console, begin, cursor - begin);
			return cursor;
		case '~':
			sh->console->state = escape_leader;
			cursor++;
			return cursor;
		default:
			/* Emit the current character on the following invocation */
			sh->console->state = escape_idle;
			return cursor;
		}
		assert(false);
		break;
	case escape_lf:
		cursor = begin;
		switch (*cursor) {
		case '~':
			sh->console->state = escape_leader;
			cursor++;
			return cursor;
		default:
			/* Emit the current character on the following invocation */
			sh->console->state = escape_idle;
			return cursor;
		}
		assert(false);
		break;
	case escape_leader:
		/*
		 * Either:
		 *
		 * 1. It's a known escape and we handle it, then return to the idle state,
		 *    or,
		 * 2. It's an unknown escape sequence and we pass through the characters,
		 *    then return to the idle state
		 *
		 * Whatever the case, we end up in the idle state. Set that first to avoid
		 * complexities in the code paths that follow.
		 */
		sh->console->state = escape_idle;
		cursor = begin;
		switch (*cursor) {
		/* Escape sequence for a UART break signal */
		case 'B':
			tcsendbreak(sh->console->tty.fd, 0);
			cursor++;
			return cursor;

		/* Escape sequence for emitting a tilde */
		case '~':
			/* Emit the tilde already in the buffer on the following invocation */
			return cursor;

		/* Unrecognised escape sequence */
		default:
			/*
			 * Emit the consumed tilde now. Emit the unrecognised escape
			 * discriminator (current character) on the following invocation.
			 */
			console_data_out(sh->console, &tilde, 1);
			return cursor;
		}
		assert(false);
		break;
	}
	fprintf(stderr, "Programming error: Reached default return in %s\n",
		__func__);
	return NULL;
}

static enum poller_ret client_poll(struct handler *handler, int events,
				   void *data)
{
	struct socket_handler *sh;
	struct client *client;
	uint8_t buf[4096];
	ssize_t rc;

	sh = to_socket_handler(handler);
	client = data;
	if (events & POLLIN) {
		rc = recv(client->fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (rc < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return POLLER_OK;
			}
			goto err_close;
		}
		if (rc == 0) {
			goto err_close;
		}

		assert(rc >= 0 && (size_t)rc <= sizeof(buf));
		uint8_t *end = buf + rc;
		uint8_t *begin = buf;
		while (begin && begin < end) {
			begin = process_buffer_range(sh, begin, end);
		}
	}

	if (events & POLLOUT) {
		client_set_blocked(client, false);
		rc = client_drain_queue(client, 0);
		if (rc) {
			goto err_close;
		}
	}

	return POLLER_OK;

err_close:
	client->poller = NULL;
	client_close(client);
	return POLLER_REMOVE;
}

static enum poller_ret socket_poll(struct handler *handler, int events,
				   void __attribute__((unused)) * data)
{
	struct socket_handler *sh = to_socket_handler(handler);
	struct client *client;
	int fd;
	int n;

	if (!(events & POLLIN)) {
		return POLLER_OK;
	}

	fd = accept(sh->sd, NULL, NULL);
	if (fd < 0) {
		return POLLER_OK;
	}

	client = malloc(sizeof(*client));
	memset(client, 0, sizeof(*client));

	client->sh = sh;
	client->fd = fd;
	client->poller = console_poller_register(sh->console, handler,
						 client_poll, client_timeout,
						 client->fd, POLLIN, client);
	client->rbc = console_ringbuffer_consumer_register(
		sh->console, client_ringbuffer_poll, client);

	n = sh->n_clients++;
	/*
	 * We're managing an array of pointers to aggregates, so don't warn about sizeof() on a
	 * pointer type.
	 */
	/* NOLINTBEGIN(bugprone-sizeof-expression) */
	sh->clients =
		reallocarray(sh->clients, sh->n_clients, sizeof(*sh->clients));
	/* NOLINTEND(bugprone-sizeof-expression) */
	sh->clients[n] = client;

	return POLLER_OK;
}

/* Create socket pair and register one end as poller/consumer and return
 * the other end to the caller.
 * Return file descriptor on success and negative value on error.
 */
int dbus_create_socket_consumer(struct console *console)
{
	struct socket_handler *sh = NULL;
	struct client *client;
	int fds[2];
	int i;
	int rc = -1;
	int n;

	for (i = 0; i < console->n_handlers; i++) {
		if (strcmp(console->handlers[i]->name, "socket") == 0) {
			sh = to_socket_handler(console->handlers[i]);
			break;
		}
	}

	if (!sh) {
		return -ENOSYS;
	}

	/* Create a socketpair */
	rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	if (rc < 0) {
		warn("Failed to create socket pair");
		return -errno;
	}

	client = malloc(sizeof(*client));
	if (client == NULL) {
		warnx("Failed to allocate client structure.");
		rc = -ENOMEM;
		goto close_fds;
	}
	memset(client, 0, sizeof(*client));

	client->sh = sh;
	client->fd = fds[0];
	client->poller = console_poller_register(sh->console, &sh->handler,
						 client_poll, client_timeout,
						 client->fd, POLLIN, client);
	client->rbc = console_ringbuffer_consumer_register(
		sh->console, client_ringbuffer_poll, client);
	if (client->rbc == NULL) {
		warnx("Failed to register a consumer.\n");
		rc = -ENOMEM;
		goto free_client;
	}

	n = sh->n_clients++;

	/*
	 * We're managing an array of pointers to aggregates, so don't warn about
	 * sizeof() on a pointer type.
	 */
	/* NOLINTBEGIN(bugprone-sizeof-expression) */
	sh->clients =
		reallocarray(sh->clients, sh->n_clients, sizeof(*sh->clients));
	/* NOLINTEND(bugprone-sizeof-expression) */
	sh->clients[n] = client;

	/* Return the second FD to caller. */
	return fds[1];

free_client:
	free(client);
close_fds:
	close(fds[0]);
	close(fds[1]);
	return rc;
}

static int socket_init(struct handler *handler, struct console *console,
		       struct config *config __attribute__((unused)))
{
	struct socket_handler *sh = to_socket_handler(handler);
	struct sockaddr_un addr;
	size_t addrlen;
	ssize_t len;
	int rc;

	sh->console = console;
	sh->clients = NULL;
	sh->n_clients = 0;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	len = console_socket_path(addr.sun_path, console->console_id);
	if (len < 0) {
		if (errno) {
			warn("Failed to configure socket: %s", strerror(errno));
		} else {
			warn("Socket name length exceeds buffer limits");
		}
		return -1;
	}

	/* Try to take a socket from systemd first */
	if (sd_listen_fds(0) == 1 &&
	    sd_is_socket_unix(SD_LISTEN_FDS_START, SOCK_STREAM, 1,
			      addr.sun_path, len) > 0) {
		sh->sd = SD_LISTEN_FDS_START;
	} else {
		sh->sd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sh->sd < 0) {
			warn("Can't create socket");
			return -1;
		}

		addrlen = sizeof(addr) - sizeof(addr.sun_path) + len;

		rc = bind(sh->sd, (struct sockaddr *)&addr, addrlen);
		if (rc) {
			socket_path_t name;
			console_socket_path_readable(&addr, addrlen, name);
			warn("Can't bind to socket path %s (terminated at first null)",
			     name);
			goto cleanup;
		}

		rc = listen(sh->sd, 1);
		if (rc) {
			warn("Can't listen for incoming connections");
			goto cleanup;
		}
	}

	sh->poller = console_poller_register(console, handler, socket_poll,
					     NULL, sh->sd, POLLIN, NULL);

	return 0;
cleanup:
	close(sh->sd);
	return -1;
}

static void socket_fini(struct handler *handler)
{
	struct socket_handler *sh = to_socket_handler(handler);

	while (sh->n_clients) {
		client_close(sh->clients[0]);
	}

	if (sh->poller) {
		console_poller_unregister(sh->console, sh->poller);
	}

	close(sh->sd);
}

static struct socket_handler socket_handler = {
	.handler = {
		.name		= "socket",
		.init		= socket_init,
		.fini		= socket_fini,
	},
};

console_handler_register(&socket_handler.handler);
