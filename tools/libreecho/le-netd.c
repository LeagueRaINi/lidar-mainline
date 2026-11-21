// SPDX-License-Identifier: GPL-2.0-only
/*
 * le-netd: LibreEcho's network daemon interface, answered by lidar.
 *
 * LibreEcho's web service and first-run setup talk to a network daemon over
 * /run/libreecho/network.sock (one JSON object per line: {"v":1,"id":N,
 * "cmd":...,"args":{...}} in, {"v":1,"id":N,"ok":true,"data":{...}} or
 * {"ok":false,"error":...} out). Its own networkd drives wpa_supplicant and
 * DHCP itself, which the sandbox cannot (no capabilities on the host's
 * network). This daemon runs on the host instead, and lidar's Wi-Fi does the
 * work: lidar-wifi for status, scans and joining, the network saved in
 * lidar's "wifi ssid" and "wifi passphrase" settings.
 *
 *   status      lidar-wifi status, the default route and DNS server
 *   scan        lidar-wifi scan
 *   connect     {"ssid","psk","security"}: WPA2-Personal only. The network
 *               lidar is already connected to is accepted as it is; another
 *               is saved and joined, and the previous one restored if that
 *               fails within 90 s
 *   disconnect  lidar-wifi stop
 *   recovery_*  refused: LibreEcho's recovery access point is not available
 *
 * Every client gets its own process, so a connect that takes a while does
 * not hold up status requests; connects take a lock. Commands run without a
 * shell (network names reach them as arguments), and the passphrase is
 * never logged. It also keeps <dir>/dns/resolv.conf a copy of lidar's DNS
 * configuration, for the sandbox's /etc/resolv.conf.
 *
 * le-netd runs as root outside the sandbox, so <dir> must be a directory
 * the sandbox is not given to write: nothing it creates there (the socket,
 * connect.lock, dns/resolv.conf) may be a path LibreEcho can replace with
 * a symlink in normal operation. The sandbox gets the socket and a
 * read-only view of <dir>/dns as bind mounts. The files are opened without
 * following symlinks all the same. (A process that escapes the sandbox's
 * chroot reaches <dir> like the rest of the initramfs; see le-sandbox.c.)
 *
 * Usage: le-netd <dir>   (socket <dir>/network.sock; <dir>/dns must exist)
 */
#define _GNU_SOURCE
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MSG_MAX		4096
#define JOIN_TIMEOUT	90	/* seconds; LibreEcho waits 120 */
#define SCAN_MAX	12	/* networks LibreEcho keeps (LE_MAX_WIFI) */
#define SCAN_BYTES	3800	/* of a reply line; LibreEcho reads 4096 */
#define IFACE		"wlan0"

static const char *dir;

/* ---- running lidar's tools ---- */

/*
 * argv's exit status, or -1; with out, its output too (up to size - 1 bytes,
 * NUL-terminated). Without, stdout is /dev/null and there is no pipe: the
 * daemons "lidar-wifi restart" leaves behind would keep one open. The pipe
 * is close-on-exec, so only the child's stdout refers to it.
 */
static int run(char *const argv[], char *out, size_t size)
{
	int pipefd[2] = { -1, -1 }, status;
	size_t used = 0;
	pid_t pid;

	if (out) {
		if (!size)
			return -1;
		out[0] = '\0';
		if (pipe2(pipefd, O_CLOEXEC) < 0)
			return -1;
	}
	pid = fork();
	if (pid < 0) {
		if (out) {
			close(pipefd[0]);
			close(pipefd[1]);
		}
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		dup2(null, 0);
		dup2(out ? pipefd[1] : null, 1);
		dup2(null, 2);
		if (null > 2)
			close(null);
		execvp(argv[0], argv);
		_exit(127);
	}
	if (!out)
		goto reap;
	close(pipefd[1]);
	for (;;) {
		char buf[512];
		ssize_t n = read(pipefd[0], buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (used + 1 < size) {
			size_t k = (size_t)n < size - 1 - used ? (size_t)n : size - 1 - used;

			memcpy(out + used, buf, k);
			used += k;
			out[used] = '\0';
		}
	}
	close(pipefd[0]);
reap:
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* a "lidar-feature get wifi <key>" value, without the newline */
static void setting(const char *key, char *out, size_t size)
{
	char *argv[] = { "lidar-feature", "get", "wifi", (char *)key, NULL };

	if (run(argv, out, size) != 0)
		out[0] = '\0';
	out[strcspn(out, "\n")] = '\0';
}

static int set_setting(const char *key, const char *value)
{
	char *set[] = { "lidar-feature", "set", "wifi", (char *)key, (char *)value, NULL };
	char *unset[] = { "lidar-feature", "unset", "wifi", (char *)key, NULL };

	return run(value && *value ? set : unset, NULL, 0);
}

static int wifi(const char *cmd)
{
	char *argv[] = { "lidar-wifi", (char *)cmd, NULL };

	return run(argv, NULL, 0);
}

/* value of "key=" in lidar-wifi's key=value lines, empty if none */
static void field(const char *text, const char *key, char *out, size_t size)
{
	size_t k = strlen(key);

	out[0] = '\0';
	for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		if (!strncmp(p, key, k) && p[k] == '=') {
			size_t n = strcspn(p + k + 1, "\n");

			if (n >= size)
				n = size - 1;
			memcpy(out, p + k + 1, n);
			out[n] = '\0';
			return;
		}
	}
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' :
	       c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/*
 * A network name as iw prints it (lidar-wifi passes it on), back to its
 * bytes, in place. iw (print_ssid_escaped) writes every byte that is not
 * printable ASCII, every backslash and a leading or trailing space as \xNN
 * and nothing else escaped, so a backslash in its output always starts
 * such an escape (there is no "\\" form). \x00 is left as it is: the name
 * is a C string here.
 */
static void unescape_ssid(char *s)
{
	char *o = s;

	while (*s) {
		if (s[0] == '\\' && s[1] == 'x' && hexval(s[2]) >= 0 && hexval(s[3]) >= 0 &&
		    (hexval(s[2]) || hexval(s[3]))) {
			*o++ = (char)(hexval(s[2]) << 4 | hexval(s[3]));
			s += 4;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
}

static int percent(int dbm)
{
	if (dbm <= -100)
		return 0;
	if (dbm >= -30)
		return 100;
	return (dbm + 100) * 100 / 70;
}

static void first_line(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");

	out[0] = '\0';
	if (f) {
		if (!fgets(out, (int)size, f))
			out[0] = '\0';
		fclose(f);
	}
	out[strcspn(out, "\n")] = '\0';
}

/* gateway of the default route through wlan0, from /proc/net/route */
static void gateway(char *out, size_t size)
{
	FILE *f = fopen("/proc/net/route", "r");
	char line[256], ifn[32];
	unsigned dest, gw, flags;

	out[0] = '\0';
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %x %x %x", ifn, &dest, &gw, &flags) == 4 &&
		    !strcmp(ifn, IFACE) && dest == 0 && (flags & 2)) {
			snprintf(out, size, "%u.%u.%u.%u", gw & 255, gw >> 8 & 255,
				 gw >> 16 & 255, gw >> 24);
			break;
		}
	fclose(f);
}

static void nameserver(char *out, size_t size)
{
	FILE *f = fopen("/etc/resolv.conf", "r");
	char line[256];

	out[0] = '\0';
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "nameserver %63s", out) == 1)
			break;
	fclose(f);
	out[size - 1] = '\0';
}

/* ssid: 32 bytes, each of which iw may print as four characters */
struct wifi_state {
	char state[16], ssid[32 * 4 + 1], ip[64], signal[16];
};

static void wifi_state(struct wifi_state *w)
{
	char *argv[] = { "lidar-wifi", "status", NULL };
	char text[2048];

	if (run(argv, text, sizeof(text)) != 0)
		text[0] = '\0';
	field(text, "state", w->state, sizeof(w->state));
	field(text, "ssid", w->ssid, sizeof(w->ssid));
	unescape_ssid(w->ssid);
	field(text, "ipv4", w->ip, sizeof(w->ip));
	w->ip[strcspn(w->ip, "/")] = '\0';
	field(text, "signal", w->signal, sizeof(w->signal));
}

/* ---- commands ---- */

static cJSON *cmd_status(void)
{
	struct wifi_state w;
	char gw[32], dns[64], mac[32];
	cJSON *d = cJSON_CreateObject(), *r;
	int connected;

	wifi_state(&w);
	connected = !strcmp(w.state, "connected");
	gateway(gw, sizeof(gw));
	nameserver(dns, sizeof(dns));
	first_line("/sys/class/net/" IFACE "/address", mac, sizeof(mac));
	cJSON_AddStringToObject(d, "interface", IFACE);
	cJSON_AddStringToObject(d, "state", connected ? "connected" : "disconnected");
	cJSON_AddStringToObject(d, "connectivity", !connected ? "disconnected" :
				w.ip[0] && gw[0] ? "healthy" : "degraded");
	cJSON_AddStringToObject(d, "ssid", connected ? w.ssid : "");
	cJSON_AddStringToObject(d, "ip", w.ip);
	cJSON_AddStringToObject(d, "gateway", gw);
	cJSON_AddStringToObject(d, "dns", dns);
	if (connected && w.signal[0]) {
		cJSON_AddNumberToObject(d, "signal", percent(atoi(w.signal)));
		cJSON_AddNumberToObject(d, "rssi_dbm", atoi(w.signal));
	}
	cJSON_AddStringToObject(d, "mac", mac);
	cJSON_AddStringToObject(d, "mode", "client");
	r = cJSON_AddObjectToObject(d, "recovery");
	cJSON_AddStringToObject(r, "mode", "off");
	cJSON_AddBoolToObject(r, "available", 0);
	cJSON_AddBoolToObject(r, "enabled", 0);
	return d;
}

/*
 * lidar-wifi scan: signal<TAB>bands<TAB>security<TAB>SSID per network,
 * strongest first. The reply has to fit LibreEcho's line buffer, and it
 * keeps SCAN_MAX networks at most.
 */
static cJSON *cmd_scan(void)
{
	char *argv[] = { "lidar-wifi", "scan", NULL };
	static char text[16384];
	cJSON *d = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(d, "networks");
	char *line, *save = NULL;
	size_t bytes = 128;	/* the reply around the list */
	int count = 0;

	if (run(argv, text, sizeof(text)) != 0)
		return d;
	for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[4], *p = line;
		const char *sec, *band;
		int i, dbm;
		cJSON *n;
		char *one;

		for (i = 0; i < 4 && p; i++) {
			f[i] = p;
			p = i < 3 ? strchr(p, '\t') : NULL;
			if (p)
				*p++ = '\0';
		}
		if (i < 4)
			continue;
		unescape_ssid(f[3]);
		if (!f[3][0])
			continue;
		dbm = atoi(f[0]);
		sec = !strcmp(f[2], "wpa2/wpa3") ? "wpa3-transition" :
		      !strcmp(f[2], "wpa3") ? "wpa3-only" : f[2];
		/* the strongest band first is not known: 5 GHz if offered */
		band = strstr(f[1], "5") ? "5 GHz" : strstr(f[1], "6") ? "6 GHz" :
		       strstr(f[1], "2.4") ? "2.4 GHz" : "unknown";
		n = cJSON_CreateObject();
		cJSON_AddStringToObject(n, "ssid", f[3]);
		cJSON_AddStringToObject(n, "security", sec);
		cJSON_AddNumberToObject(n, "signal", percent(dbm));
		cJSON_AddNumberToObject(n, "rssi_dbm", dbm);
		cJSON_AddStringToObject(n, "band", band);
		/* lidar-wifi joins WPA2-Personal networks only */
		cJSON_AddBoolToObject(n, "wpa2_attempt",
				      !strcmp(f[2], "wpa2") || !strcmp(f[2], "wpa2/wpa3"));
		one = cJSON_PrintUnformatted(n);
		if (!one || bytes + strlen(one) + 1 > SCAN_BYTES) {
			free(one);
			cJSON_Delete(n);
			break;
		}
		bytes += strlen(one) + 1;
		free(one);
		cJSON_AddItemToArray(list, n);
		if (++count == SCAN_MAX)
			break;
	}
	return d;
}

static int joined(const char *ssid)
{
	struct wifi_state w;

	wifi_state(&w);
	return !strcmp(w.state, "connected") && w.ip[0] && !strcmp(w.ssid, ssid);
}

static time_t monotonic(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec;
}

/* WPA2 passphrase: 8-63 printable ASCII characters */
static int psk_ok(const char *psk)
{
	size_t n = strlen(psk);

	for (size_t i = 0; i < n; i++)
		if (psk[i] < 0x20 || psk[i] > 0x7e)
			return 0;
	return n >= 8 && n <= 63;
}

static const char *cmd_connect(cJSON *args)
{
	const cJSON *js = cJSON_GetObjectItemCaseSensitive(args, "ssid");
	const cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "psk");
	const cJSON *jsec = cJSON_GetObjectItemCaseSensitive(args, "security");
	char old_ssid[64], old_pass[80], lock[256];
	const char *ssid, *psk, *err = NULL;
	time_t deadline;
	int fd;

	if (!cJSON_IsString(js) || !js->valuestring[0])
		return "no network name";
	ssid = js->valuestring;
	psk = cJSON_IsString(jp) ? jp->valuestring : "";
	if (cJSON_IsString(jsec) && !strcmp(jsec->valuestring, "open"))
		return "open networks are not supported (lidar joins WPA2-Personal networks)";
	if (!psk_ok(psk))
		return "invalid passphrase (8-63 printable ASCII characters)";
	{
		char *argv[] = { "lidar-feature", "enabled", "wifi", NULL };

		if (run(argv, NULL, 0) != 0)
			return "lidar's wifi service is off";
	}
	snprintf(lock, sizeof(lock), "%s/connect.lock", dir);
	fd = open(lock, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return "busy";
	if (flock(fd, LOCK_EX) < 0) {
		close(fd);
		return "busy";
	}
	setting("ssid", old_ssid, sizeof(old_ssid));
	setting("passphrase", old_pass, sizeof(old_pass));
	/* the network lidar is on: nothing to change */
	if (!strcmp(old_ssid, ssid) && !strcmp(old_pass, psk) && joined(ssid))
		goto out;
	fprintf(stderr, "le-netd: joining a new network\n");
	if (set_setting("ssid", ssid) != 0) {
		err = "invalid network name (1-32 bytes)";
		goto out;
	}
	if (set_setting("passphrase", psk) != 0) {
		set_setting("ssid", old_ssid);
		err = "invalid passphrase (8-63 printable ASCII characters)";
		goto out;
	}
	/* 90 s in all, the restart and every status query included */
	deadline = monotonic() + JOIN_TIMEOUT;
	wifi("restart");
	do {
		sleep(1);
		if (joined(ssid))
			goto out;
	} while (monotonic() < deadline);
	fprintf(stderr, "le-netd: no connection, back to the previous network\n");
	set_setting("ssid", old_ssid);
	set_setting("passphrase", old_pass);
	wifi("restart");
	err = "could not join the network";
out:
	memset(old_pass, 0, sizeof(old_pass));
	close(fd);
	return err;
}

/* ---- protocol ---- */

static void reply(int fd, cJSON *id, cJSON *data, const char *err)
{
	cJSON *r = cJSON_CreateObject();
	char *text;

	cJSON_AddNumberToObject(r, "v", 1);
	cJSON_AddNumberToObject(r, "id", cJSON_IsNumber(id) ? id->valuedouble : 0);
	cJSON_AddBoolToObject(r, "ok", !err);
	if (err)
		cJSON_AddStringToObject(r, "error", err);
	else
		cJSON_AddItemToObject(r, "data", data ? data : cJSON_CreateObject());
	text = cJSON_PrintUnformatted(r);
	if (text) {
		size_t n = strlen(text);

		text[n] = '\n';
		for (size_t done = 0; done < n + 1;) {
			ssize_t w = write(fd, text + done, n + 1 - done);

			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
				break;
			done += (size_t)w;
		}
		free(text);
	}
	cJSON_Delete(r);
}

static void handle(int fd, const char *line)
{
	cJSON *req = cJSON_Parse(line), *id, *cmd;
	const char *c;

	if (!req) {
		reply(fd, NULL, NULL, "bad request");
		return;
	}
	id = cJSON_GetObjectItemCaseSensitive(req, "id");
	cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
	c = cJSON_IsString(cmd) ? cmd->valuestring : "";
	if (!strcmp(c, "status"))
		reply(fd, id, cmd_status(), NULL);
	else if (!strcmp(c, "scan"))
		reply(fd, id, cmd_scan(), NULL);
	else if (!strcmp(c, "connect"))
		reply(fd, id, NULL, cmd_connect(cJSON_GetObjectItemCaseSensitive(req, "args")));
	else if (!strcmp(c, "disconnect"))
		reply(fd, id, NULL, wifi("stop") == 0 ? NULL : "lidar-wifi stop failed");
	else if (!strncmp(c, "recovery", 8))
		reply(fd, id, NULL, "the recovery access point is not available on lidar");
	else
		reply(fd, id, NULL, "unknown command");
	cJSON_Delete(req);
}

/* one client: requests until it hangs up or idles for a minute */
static void serve(int fd)
{
	char buf[MSG_MAX + 1];
	size_t used = 0;

	signal(SIGCHLD, SIG_DFL);	/* run() waits for its children */
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		char *nl;
		ssize_t n;

		if (poll(&p, 1, 60000) <= 0)
			break;
		n = read(fd, buf + used, MSG_MAX - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
		buf[used] = '\0';
		while ((nl = memchr(buf, '\n', used))) {
			*nl = '\0';
			handle(fd, buf);
			used -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, used);
			buf[used] = '\0';
		}
		if (used == MSG_MAX)
			break;	/* longer than the protocol allows */
	}
	close(fd);
}

/*
 * <dir>/dns/resolv.conf follows /etc/resolv.conf (lidar's DHCP client
 * writes it). Replaced by rename; the temporary file is always a new one.
 */
static void copy_resolv(void)
{
	char path[256], tmp[256], buf[4096];
	ssize_t n = -1;
	int in = open("/etc/resolv.conf", O_RDONLY), out;

	if (in >= 0) {
		n = read(in, buf, sizeof(buf));
		close(in);
	}
	if (n < 0)
		n = 0;
	snprintf(path, sizeof(path), "%s/dns/resolv.conf", dir);
	snprintf(tmp, sizeof(tmp), "%s/dns/resolv.conf.new", dir);
	unlink(tmp);
	out = open(tmp, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (out < 0)
		return;
	if (write(out, buf, (size_t)n) == n && close(out) == 0)
		rename(tmp, path);
	else
		close(out);
}

int main(int argc, char **argv)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int s;

	if (argc != 2) {
		fprintf(stderr, "usage: le-netd <dir>\n");
		return 2;
	}
	dir = argv[1];
	if (snprintf(a.sun_path, sizeof(a.sun_path), "%s/network.sock", dir) >=
	    (int)sizeof(a.sun_path))
		return 1;
	unlink(a.sun_path);
	s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    chmod(a.sun_path, 0666) < 0 || listen(s, 8) < 0) {
		perror("le-netd: network.sock");
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);	/* clients' processes reap themselves */
	signal(SIGPIPE, SIG_IGN);
	for (time_t copied = 0;;) {
		struct pollfd p = { s, POLLIN, 0 };
		time_t now = time(NULL);
		int c;

		/* every 10 s, however busy the socket is */
		if (now - copied >= 10) {
			copy_resolv();
			copied = now;
		}
		if (poll(&p, 1, 10000) <= 0)
			continue;
		c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
		if (c < 0)
			continue;
		if (fork() == 0) {
			close(s);
			serve(c);
			_exit(0);
		}
		close(c);
	}
}
