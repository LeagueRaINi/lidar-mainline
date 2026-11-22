// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-say: hands an answer to lidar-tts and returns at once; the say
 * command of lidar-assistant (-y) and lidar-claude.
 *
 * Usage: lidar-say <language> <text>
 *
 * The parts of the text are LIDAR_SAY_PARTS ("|" between them), as
 * lidar-assistant sets it, else the text is one part. They go as one line
 * "say <language> <parts>" to /run/lidar-tts.fifo (LIDAR_TTS_FIFO for
 * tests), never waiting for it. If lidar-tts does not run, the line
 * "say <language> <text>" goes to stdout instead, as without a say command.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *fifo = getenv("LIDAR_TTS_FIFO"), *parts = getenv("LIDAR_SAY_PARTS");
	char *line;
	struct stat st;
	size_t n;
	int fd;

	if (argc != 3 || !argv[1][0] || strchr(argv[1], ' ')) {
		fprintf(stderr, "usage: lidar-say <language> <text>\n");
		return 2;
	}
	if (!fifo || !*fifo)
		fifo = "/run/lidar-tts.fifo";
	if (!parts || !*parts)
		parts = argv[2];
	n = strlen(argv[1]) + strlen(parts) + 5;
	if (!(line = malloc(n + 2)))
		return 1;
	snprintf(line, n + 1, "say %s %s", argv[1], parts);
	/* one line, written whole (up to PIPE_BUF bytes), cut at a character */
	if (n > PIPE_BUF - 1) {
		n = PIPE_BUF - 1;
		while (n && ((unsigned char)line[n] & 0xc0) == 0x80)
			n--;
	}
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)line[i] < 0x20)
			line[i] = ' ';
	line[n++] = '\n';
	/* only a FIFO somebody reads: without a reader, ENXIO */
	fd = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0 && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && write(fd, line, n) == (ssize_t)n) {
		close(fd);
		return 0;
	}
	if (fd >= 0)
		close(fd);
	printf("say %s %s\n", argv[1], argv[2]);
	/* nobody will speak it: the ring stops showing that an answer comes */
	fd = open("/run/lidar-ring.fifo", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && write(fd, "wake off\n", 9) < 0)
			fprintf(stderr, "lidar-say: ring: %s\n", strerror(errno));
		close(fd);
	}
	return 0;
}
