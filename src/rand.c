/*
 * Cryptographic RNG used by the cipher rekey machinery and any other
 * caller that needs unpredictable bytes.
 *
 * Strategy:
 *   1. getrandom(2) — Linux 3.17+, glibc 2.25+. Preferred.
 *   2. /dev/urandom — fallback for kernels/libcs without getrandom.
 *
 * The original rand.c reached straight into OpenSSL's RAND_bytes; the
 * Phase 1.3 modernization drops that dependency along with libssl. The
 * function signature (u_int8_t buffer, returns nbytes on success or 0
 * on failure) is preserved so call sites in cipher.c and elsewhere are
 * untouched.
 */

#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/random.h>
#include "protocol.h"

static unsigned int
random_bytes_urandom(u_int8_t *buf, unsigned int nbytes)
{
	int fd;
	unsigned int got = 0;

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	while (got < nbytes) {
		ssize_t n = read(fd, buf + got, nbytes - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return 0;
		}
		if (n == 0)
			break;
		got += (unsigned int)n;
	}
	close(fd);

	return got == nbytes ? nbytes : 0;
}

unsigned int
random_bytes(u_int8_t *buf, unsigned int nbytes)
{
	unsigned int got = 0;

	while (got < nbytes) {
		ssize_t n = getrandom(buf + got, nbytes - got, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			/* ENOSYS on pre-3.17 kernels — fall back to /dev/urandom. */
			return random_bytes_urandom(buf, nbytes);
		}
		got += (unsigned int)n;
	}

	return nbytes;
}
