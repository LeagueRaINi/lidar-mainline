// SPDX-License-Identifier: GPL-2.0-only
/*
 * le-sandbox: runs LibreEcho's userspace away from the eMMC and, in normal
 * operation, from lidar's own files.
 *
 * Usage: le-sandbox <root> <pidfile> <cmd> [args...]
 *
 * Called by lidar-libreecho in a private mount namespace in which <root> is
 * fully set up (its own /dev without block devices, proc, sys, squashfs
 * images). Then:
 *
 *  1. Every mount outside <root> is detached: devtmpfs with the eMMC nodes,
 *     the host /proc and /sys, the system partition. What is left of the host
 *     is the bare initramfs (one static node: /dev/console).
 *  2. A user namespace with an identity uid/gid map: file permissions are
 *     those of root as before, but capabilities only count for the
 *     namespace's own objects. The kernel refuses mknod, devtmpfs and other
 *     block-device filesystems, module loading, raw I/O, reboot and clock
 *     setting; LibreEcho's own bind and tmpfs mounts still work, and the
 *     mounts made here are locked.
 *  3. A PID namespace: this program's grandchild is PID 1, mounts a fresh
 *     /proc over the host one (so /proc shows LibreEcho only), chroots into
 *     <root>, runs <cmd> and then reaps orphans until it is killed. Killing
 *     it ends every process in the namespace. Its host PID goes to <pidfile>.
 *
 * This keeps an unmodified release where it belongs; it is not a boundary
 * against a deliberately hostile one. The initramfs stays mounted in the
 * sandbox's mount namespace, hidden by the chroot only, and a process
 * inside holds CAP_SYS_CHROOT in its user namespace (LibreEcho's mDNS and
 * AirPlay chroots need it): it can escape the chroot and, as uid 0, write
 * lidar's files there. It finds no block device node and cannot make one,
 * but it could change a lidar tool that host root runs later, and through
 * that reach everything.
 * Open: make <root> the real root of the mount namespace (move it, then
 * pivot_root), so the initramfs is unreachable.
 *
 * Real-time priorities and memory locking are granted through rlimits,
 * since CAP_SYS_NICE and CAP_IPC_LOCK no longer count.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *what)
{
	fprintf(stderr, "le-sandbox: %s: %s\n", what, strerror(errno));
	exit(1);
}

static int under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return !strncmp(path, dir, n) && (path[n] == '/' || path[n] == '\0');
}

/* detach every mount that is neither <root>, below it, nor on its path */
static void detach_host_mounts(const char *root)
{
	char *mp[256];
	int n = 0;
	struct mntent *m;
	FILE *f = setmntent("/proc/self/mounts", "r");

	if (!f)
		die("mounts");
	while ((m = getmntent(f)) && n < 256) {
		if (!strcmp(m->mnt_dir, "/") || under(m->mnt_dir, root) ||
		    under(root, m->mnt_dir))
			continue;
		mp[n++] = strdup(m->mnt_dir);
	}
	endmntent(f);
	/* children first; MNT_DETACH takes whatever is left below */
	while (n--) {
		if (umount2(mp[n], MNT_DETACH) < 0 && errno != EINVAL &&
		    errno != ENOENT)
			die(mp[n]);
		free(mp[n]);
	}
}

static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0 || write(fd, s, strlen(s)) != (ssize_t)strlen(s))
		die(path);
	close(fd);
}

static void wait_byte(int fd)
{
	char c;

	if (read(fd, &c, 1) != 1)
		exit(1);
}

int main(int argc, char **argv)
{
	const struct rlimit rt = { 95, 95 }, nice = { 40, 40 },
			    lock = { RLIM_INFINITY, RLIM_INFINITY };
	int ready[2], mapped[2], done[2];
	char path[64], proc[4096];
	pid_t child;

	if (argc < 4) {
		fprintf(stderr, "usage: le-sandbox <root> <pidfile> <cmd> [args...]\n");
		return 2;
	}
	if (setrlimit(RLIMIT_RTPRIO, &rt) || setrlimit(RLIMIT_NICE, &nice) ||
	    setrlimit(RLIMIT_MEMLOCK, &lock))
		die("rlimit");
	snprintf(proc, sizeof(proc), "%s/proc", argv[1]);
	if (pipe(ready) || pipe(mapped) || pipe(done))
		die("pipe");
	/* A private copy of the caller's mounts: detaching the host mounts
	   below must never touch the namespace that called us. */
	if (unshare(CLONE_NEWNS) < 0 ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("private mount namespace");

	child = fork();
	if (child < 0)
		die("fork");
	if (child == 0) {
		pid_t init, cmd;

		close(ready[0]);
		close(mapped[1]);
		close(done[0]);
		if (unshare(CLONE_NEWUSER) < 0)
			die("user namespace");
		write(ready[1], "u", 1);
		wait_byte(mapped[0]);	/* EOF if the parent failed */
		close(ready[1]);
		close(mapped[0]);
		/* the new mount namespace belongs to the user namespace; the UTS
		   one lets LibreEcho set its own hostname */
		if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS) < 0)
			die("mount/pid/uts namespace");
		init = fork();
		if (init < 0)
			die("fork");
		if (init > 0) {
			FILE *f = fopen(argv[2], "w");

			if (!f || fprintf(f, "%d\n", init) < 0 || fclose(f)) {
				/* nothing may run that stop cannot find */
				kill(init, SIGKILL);
				die(argv[2]);
			}
			_exit(0);
		}

		/* PID 1 of the namespace */
		if (mount("proc", proc, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
			  NULL) < 0)
			die("proc");
		if (chroot(argv[1]) < 0 || chdir("/") < 0)
			die("chroot");
		cmd = fork();
		if (cmd < 0)
			die("fork");
		if (cmd == 0) {
			close(done[1]);
			execv(argv[3], argv + 3);
			die(argv[3]);
		}
		waitpid(cmd, NULL, 0);
		write(done[1], "d", 1);
		close(done[1]);
		/* from now on the kernel reaps orphaned daemons itself */
		signal(SIGCHLD, SIG_IGN);
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		for (;;)
			pause();
	}

	close(ready[1]);
	close(mapped[0]);
	close(done[1]);
	wait_byte(ready[0]);
	snprintf(path, sizeof(path), "/proc/%d/uid_map", child);
	write_file(path, "0 0 4294967295\n");
	snprintf(path, sizeof(path), "/proc/%d/gid_map", child);
	write_file(path, "0 0 4294967295\n");
	/* the child still shares this mount namespace and copies it next */
	detach_host_mounts(argv[1]);
	write(mapped[1], "m", 1);
	waitpid(child, NULL, 0);
	/* returns once <cmd> has finished; the namespace keeps running */
	wait_byte(done[0]);
	return 0;
}
