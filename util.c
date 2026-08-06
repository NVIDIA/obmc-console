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

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "console-server.h"

/* A console fd can be nonblocking even when its producer is a blocking SSH
 * client.  Treating EAGAIN as a completed callback silently discards the
 * remainder of that client's command.  Wait for actual writability and keep
 * the exact suffix instead.  The timeout is per period without progress: any
 * positive write starts the next attempt with a fresh deadline. */
#define WRITE_PROGRESS_TIMEOUT_MS 5000

static int set_progress_deadline(struct timespec *deadline)
{
	if (clock_gettime(CLOCK_MONOTONIC, deadline)) {
		return -1;
	}
	deadline->tv_sec += WRITE_PROGRESS_TIMEOUT_MS / 1000;
	deadline->tv_nsec +=
		(WRITE_PROGRESS_TIMEOUT_MS % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
	return 0;
}

static int progress_time_left_ms(const struct timespec *deadline)
{
	struct timespec now;
	time_t seconds;
	long nanoseconds;
	long long milliseconds;

	if (clock_gettime(CLOCK_MONOTONIC, &now)) {
		return -1;
	}
	seconds = deadline->tv_sec - now.tv_sec;
	nanoseconds = deadline->tv_nsec - now.tv_nsec;
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000L;
	}
	if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
		return 0;
	}
	milliseconds = (long long)seconds * 1000 +
		(nanoseconds + 999999L) / 1000000L;
	return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

int write_buf_to_fd(int fd, const uint8_t *buf, size_t len)
{
	struct pollfd pollfd = {
		.fd = fd,
		.events = POLLOUT,
	};
	struct timespec progress_deadline;
	size_t pos = 0;
	ssize_t rc;

	if (set_progress_deadline(&progress_deadline)) {
		warn("Failed to read monotonic clock");
		return -1;
	}

	while (pos < len) {
		rc = write(fd, buf + pos, len - pos);
		if (rc > 0) {
			pos += (size_t)rc;
			if (set_progress_deadline(&progress_deadline)) {
				warn("Failed to read monotonic clock");
				return -1;
			}
			continue;
		}
		if (rc == 0) {
			warnx("Write made zero progress");
			return -1;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			int poll_rc;
			int timeout_ms;

			do {
				timeout_ms =
					progress_time_left_ms(&progress_deadline);
				if (timeout_ms < 0) {
					warn("Failed to read monotonic clock");
					return -1;
				}
				if (timeout_ms == 0) {
					warnx("Write stalled for %d ms",
					      WRITE_PROGRESS_TIMEOUT_MS);
					return -1;
				}
				pollfd.revents = 0;
				poll_rc = poll(&pollfd, 1, timeout_ms);
			} while (poll_rc < 0 && errno == EINTR);

			if (poll_rc == 0) {
				warnx("Write stalled for %d ms",
				      WRITE_PROGRESS_TIMEOUT_MS);
				return -1;
			}
			if (poll_rc < 0) {
				warn("Write poll error");
				return -1;
			}
			if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				warnx("Write poll reported terminal events 0x%x",
				      pollfd.revents);
				return -1;
			}
			continue;
		}
		warn("Write error");
		return -1;
	}

	return 0;
}
