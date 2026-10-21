// SPDX-License-Identifier: MIT
/*
 * fs_read - read samples from /dev/fakesensor with poll()
 *
 *   gcc -O2 -I../driver -o fs_read fs_read.c
 *   sudo ./fs_read [count]
 *
 * Prints value, sequence number, interval between hard-IRQ timestamps,
 * and flags sequence gaps (missed samples) and device-side overruns.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "fakesensor.h"

int main(int argc, char **argv)
{
	long n = argc > 1 ? strtol(argv[1], NULL, 0) : 50;
	struct pollfd pfd;
	struct fs_sample s[8];
	unsigned long long prev_ts = 0;
	int prev_seq = -1;

	pfd.fd = open("/dev/fakesensor", O_RDONLY | O_NONBLOCK);
	if (pfd.fd < 0) {
		perror("open /dev/fakesensor");
		return 1;
	}
	pfd.events = POLLIN;

	while (n > 0) {
		if (poll(&pfd, 1, 5000) <= 0) {
			fprintf(stderr, "timeout: no data in 5 s (check DRDY wiring / CTRL)\n");
			return 1;
		}
		ssize_t r = read(pfd.fd, s, sizeof(s));
		if (r < 0) {
			if (errno == EAGAIN)
				continue;
			perror("read");
			return 1;
		}
		for (size_t i = 0; i < r / sizeof(s[0]) && n > 0; i++, n--) {
			double dt_ms = prev_ts ? (s[i].ts_ns - prev_ts) / 1e6 : 0;
			int gap = prev_seq >= 0 ? (unsigned char)(s[i].seq - prev_seq - 1) : 0;

			printf("val=%6d seq=%3u dt=%8.3f ms%s%s\n",
			       s[i].val, s[i].seq, dt_ms,
			       gap ? "  <-- MISSED" : "",
			       (s[i].status & 2) ? "  <-- DEV OVR" : "");
			prev_ts = s[i].ts_ns;
			prev_seq = s[i].seq;
		}
	}
	close(pfd.fd);
	return 0;
}
