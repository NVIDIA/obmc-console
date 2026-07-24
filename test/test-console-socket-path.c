#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "console-server.h"

/* Helper to calculate expected addrlen */
static ssize_t expected_pathlen(const char *id)
{
	size_t len =
		1ul + strlen(CONSOLE_SOCKET_PREFIX) + strlen(".") + strlen(id);
	return len > SSIZE_MAX ? -1 : (ssize_t)len;
}

/* Test that a valid id returns full addrlen */
static void test_console_socket_path_returns_pathlen(void)
{
	socket_path_t sun_path;
	const char *id = "test-console";
	ssize_t pathlen;

	pathlen = console_socket_path(sun_path, id);

	assert(pathlen > 0);
	assert(pathlen == expected_pathlen(id));
}

/* Test that NULL id returns -1 and sets errno to EINVAL */
static void test_console_socket_path_null_id(void)
{
	socket_path_t sun_path;
	ssize_t len;

	errno = 0;
	len = console_socket_path(sun_path, NULL);

	assert(len == -1);
	assert(errno == EINVAL);
}

/* Test that an id that exceeds buffer limits returns -1 and sets errno to 0 */
static void test_console_socket_path_id_too_long(void)
{
	socket_path_t sun_path;
	/* Create an id that will exceed buffer size */
	char long_id[sizeof(socket_path_t) + 1];
	ssize_t len;

	memset(long_id, 'a', sizeof(long_id) - 1);
	long_id[sizeof(long_id) - 1] = '\0';

	errno = 0;
	len = console_socket_path(sun_path, long_id);

	assert(len == -1);
	assert(errno == 0);
}

/* Test that the socket path contains the expected prefix and id */
static void test_console_socket_path_content(void)
{
	socket_path_t sun_path;
	const char *id = "test-abstract";
	char expected[sizeof(socket_path_t)];
	ssize_t addrlen;

	addrlen = console_socket_path(sun_path, id);
	assert(addrlen > 0);

	/* Build expected path (without NUL prefix) */
	snprintf(expected, sizeof(expected), CONSOLE_SOCKET_PREFIX ".%s", id);

	/* First byte should be NUL for abstract socket */
	assert(sun_path[0] == '\0');
	/* Compare path content after NUL prefix */
	assert(strcmp(sun_path + 1, expected) == 0);
}

int main(void)
{
	test_console_socket_path_returns_pathlen();
	test_console_socket_path_null_id();
	test_console_socket_path_id_too_long();
	test_console_socket_path_content();

	return EXIT_SUCCESS;
}
