// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-tee: copies 16-bit audio from stdin to stdout, and as much of it as
 * fits into a FIFO, for a second reader that must never hold up the first:
 *
 *   lidar-voice | lidar-tee /run/lidar-stt.pcm | lidar-wakeword
 *   lidar-stt <> /run/lidar-stt.pcm
 *
 * The FIFO is made if it is not there and opened read-write and
 * non-blocking: the copy goes in whether anybody reads it or not, and what
 * does not fit (the reader is slow, absent or still loading) is dropped. It
 * is written in pieces of whole samples of at most PIPE_BUF bytes, each of
 * which goes in whole or not at all, so the reader never gets half a
 * sample. Without a FIFO (another kind of file of that name) only stdout
 * gets the audio.
 *
 * Usage: lidar-tee <fifo>
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const char *p, size_t left)
{
	while (left) {
		ssize_t w = write(fd, p, left);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		left -= (size_t)w;
	}
	return 0;
}

int main(int argc, char **argv)
{
	char buf[PIPE_BUF];
	size_t carry = 0;	/* a half sample for the FIFO */
	struct stat st;
	int fifo;

	if (argc != 2) {
		fprintf(stderr, "usage: lidar-tee <fifo>\n");
		return 2;
	}
	if (mkfifo(argv[1], 0600) && errno != EEXIST)
		fprintf(stderr, "lidar-tee: %s: %s\n", argv[1], strerror(errno));
	fifo = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fifo >= 0 && (fstat(fifo, &st) || !S_ISFIFO(st.st_mode))) {
		fprintf(stderr, "lidar-tee: %s is no FIFO\n", argv[1]);
		close(fifo);
		fifo = -1;
	}
	for (;;) {
		/* room for the carried byte: at most PIPE_BUF to the FIFO */
		ssize_t n = read(STDIN_FILENO, buf + carry, sizeof buf - carry);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0;
		if (write_all(STDOUT_FILENO, buf + carry, (size_t)n))
			return 1;
		if (fifo < 0)
			continue;
		size_t bytes = carry + (size_t)n;
		ssize_t w;

		do
			w = write(fifo, buf, bytes & ~(size_t)1);
		while (w < 0 && errno == EINTR);
		/* EAGAIN: full, this piece is dropped */
		carry = bytes & 1;
		if (carry)
			buf[0] = buf[bytes - 1];
	}
}
