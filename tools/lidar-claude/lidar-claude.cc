// SPDX-License-Identifier: GPL-2.0-or-later
// lidar-claude: answers one utterance that the assistant's grammar did not
// understand ("unmatched") with Claude, through the Anthropic Messages API,
// with lidar-assistant's actions as tools.
//
// Usage: lidar-claude [-y <cmd>] [-r <dir>] [-e] en|de [<utterance>...]
//        lidar-claude -P en|de
//   en|de       the language of the answer (voice language)
//   <utterance> what was said; without it, one line on standard input
//   -P          print every fixed sentence it can say, one per line, as the
//               say hook gets it (for a speech cache), and exit
//   -y <cmd>    program that speaks a sentence: <cmd> <language> <text>, run
//               and waited for once per sentence, in order (default:
//               $LIDAR_SAY_COMMAND, else print "say <language> <text>")
//   -r <dir>    runtime files (default /run): lidar-assistant.fifo (tools go
//               there), lidar-claude.<pid>.<n> (the result of tool call n
//               comes back on it, a FIFO of this process for that call
//               only), lidar-time.synced (read), lidar-wake.listening (read:
//               a new wake word, which changes it, ends this answer quietly)
//   -e          status lines on stderr instead of the kernel log
//
// The API key comes from "lidar-feature get voice api_key" through a pipe:
// never an argument, a URL or a log line. The tools are what
// "lidar-assistant -S" prints. The answer is streamed (server-sent events)
// and each sentence is spoken as soon as it is complete. A tool_use block
// goes to lidar-assistant as one line on its FIFO,
//   !tool <run dir>/lidar-claude.<pid>.<n> <action> [<name>=<value>]...
// (values percent-encoded); lidar-assistant checks it against its action
// table, carries it out without speaking and writes its result (one line of
// JSON) to that FIFO, which goes back to Claude as the tool_result. A result
// that comes after the 6 s a tool may take finds its FIFO gone and is lost,
// never taken for another call's. At most 4 rounds of tools: the request
// after the fourth allows none. 5 s to connect (name lookup, TCP, TLS), 20 s
// for the whole exchange, not counting the time the say hook takes. With a
// key, "One moment, let me check." is said at once while the request goes
// out; without one, that it was not understood (and, once per boot, where a
// key goes: <run dir>/lidar-claude.hinted). Errors are said briefly. One
// exchange, no memory of earlier ones.
//
// Status lines (no key, no utterance, no answer text) go to /dev/kmsg.
//
// Environment, for tests: LIDAR_CLAUDE_MODEL (default claude-haiku-5-5),
// LIDAR_CLAUDE_URL (default https://api.anthropic.com/v1/messages; plain
// http only to 127.0.0.1 or localhost), LIDAR_CLAUDE_CAFILE (default the
// system's CA bundle).
//
// Exit status: 0 answered, 1 an error was spoken, 2 usage.
#include <cjson/cJSON.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// The API as documented at platform.claude.com/docs: Messages
// (api/messages), streaming (build-with-claude/streaming), tool use
// (agents-and-tools/tool-use/handle-tool-calls), the current Haiku
// (models/haiku-5-5/overview), API versions (api/versioning).
static const char MODEL[] = "claude-haiku-5-5";
static const char API_URL[] = "https://api.anthropic.com/v1/messages";
static const char API_VERSION[] = "2026-10-21";
static const char CA_BUNDLE[] = "/etc/ssl/certs/ca-certificates.crt";
// TOOL_MS: above lidar-assistant's 5 s for the command an action runs
static const long CONNECT_MS = 5000, TOTAL_MS = 20000, TOOL_MS = 6000;
static const int MAX_TOOL_ROUNDS = 4, MAX_TOKENS = 400;
static const size_t MAX_UTTERANCE = 1000, MAX_LINE = 1000, MAX_ERROR_BODY = 16384;

enum Lang { EN, DE };
static const char *const LANG_NAMES[] = { "en", "de" };

// The fixed sentences: when there is no key ("not understood" and, the
// first time after boot, the hint), the filler said while the request goes
// out, and the errors, in both languages
enum Spoken { S_NOT_UNDERSTOOD, S_KEY_HINT, S_ASKING, S_OFFLINE, S_INVALID_KEY, S_BUSY, S_FAILED,
	      S_NO_ANSWER, S_COUNT };
static const char *const SPOKEN[2][S_COUNT] = {
	{ "Sorry, I didn't understand that.",
	  "To ask me anything, add a Claude key in the web interface.",
	  "One moment, let me check.",
	  "I can't reach Claude right now.",
	  "Claude doesn't accept the key. Please check it in the web interface.",
	  "Claude is busy right now. Try again in a moment.",
	  "Something went wrong with Claude.",
	  "Sorry, I don't have an answer to that." },
	{ "Das habe ich leider nicht verstanden.",
	  "Für freie Fragen kannst du in der Weboberfläche einen Claude-Schlüssel eintragen.",
	  "Moment, ich frage nach.",
	  "Ich erreiche Claude gerade nicht.",
	  "Der Claude-Schlüssel wird nicht angenommen. Prüf ihn in der Weboberfläche.",
	  "Claude ist gerade ausgelastet. Versuch es gleich noch mal.",
	  "Da ist bei Claude etwas schiefgegangen.",
	  "Darauf weiß ich leider keine Antwort." },
};

static Lang lang = EN;
static std::string say_command, run_dir = "/run";
static bool log_stderr;
static long start_ms, deadline_ms, first_said_ms;
static int sentences_said;
static char reply_path[256];	// the signal handler removes it
static volatile sig_atomic_t reply_live;	// reply_path is a FIFO of ours
static unsigned tool_calls;

static long now_ms()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static double seconds(long from)
{
	return (now_ms() - from) / 1000.0;
}

// A status line for the kernel log (or stderr with -e)
__attribute__((format(printf, 1, 2))) static void status(const char *fmt, ...)
{
	static int kmsg = -2;
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "lidar-claude: ");

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	n = (int)strlen(line);
	line[n++] = '\n';
	if (kmsg == -2)
		kmsg = log_stderr ? -1 : open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (write(kmsg >= 0 ? kmsg : 2, line, (size_t)n) < 0) {
		// nowhere else to report it
	}
}

// Speaking ---------------------------------------------------------------------

static pid_t say_pending = -1;	// the filler's hook, not waited for yet
static bool listing;		// -P: sentences are printed, not said

// lidar-wake.listening as it was at the start: its time and content (the
// wake's uptime); "" if it was not there
static std::string wake_seen;

static std::string wake_now()
{
	std::string path = run_dir + "/lidar-wake.listening", s;
	struct stat st;
	char buf[64];
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return "";
	ssize_t n = fstat(fd, &st) ? -1 : read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0)
		return "";
	s = std::to_string((long long)st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec) + " ";
	return s.append(buf, (size_t)n);
}

// The wake word was heard again since the start (the file went away after
// 5 s: not a new wake): the user wants something else, the rest of this
// answer is not said. Ends the process.
static void stop_if_woken()
{
	std::string now = wake_now();

	if (now.empty() || now == wake_seen)
		return;
	status("a new wake word: answer dropped");
	if (reply_live)
		unlink(reply_path);
	_exit(0);
}

static void say_wait()
{
	if (say_pending > 0)
		while (waitpid(say_pending, nullptr, 0) < 0 && errno == EINTR)
			;
	say_pending = -1;
}

// One sentence to the say hook, after the one before. The time it takes is
// not the API's: the deadline moves by as much. The filler is not waited
// for (the request goes out meanwhile) and not counted as an answer.
static void say(const std::string &text, bool filler = false)
{
	static std::vector<std::string> listed;

	if (listing) {
		if (std::find(listed.begin(), listed.end(), text) == listed.end()) {
			listed.push_back(text);
			printf("%s\n", text.c_str());
		}
		return;
	}
	stop_if_woken();
	long t0 = now_ms();

	say_wait();
	if (!filler && !sentences_said++)
		first_said_ms = t0;
	if (say_command.empty()) {
		printf("say %s %s\n", LANG_NAMES[lang], text.c_str());
		fflush(stdout);
	} else {
		pid_t pid = fork();
		if (!pid) {
			execlp(say_command.c_str(), say_command.c_str(), LANG_NAMES[lang], text.c_str(),
			       (char *)nullptr);
			_exit(127);
		}
		if (pid > 0 && filler)
			say_pending = pid;
		else if (pid > 0)
			while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
				;
	}
	deadline_ms += now_ms() - t0;
}

// The streamed text, cut into sentences as it arrives: a sentence ends at
// ".", "!" or "?" before a space (not after an initial, an abbreviation or,
// in German, an ordinal number of one or two digits such as "22."), or at a
// line break. Markdown marks are dropped. A sentence that grows past 200
// bytes is cut at its last comma, so speech can start.
class Sentences {
public:
	void add(const std::string &text)
	{
		for (char c : text) {
			if (c == '*' || c == '#' || c == '`' || c == '\r')
				continue;
			if (c == '\n') {
				flush();
			} else if (c == ' ' || c == '\t') {
				if (buf.empty() || buf.back() == ' ')
					continue;
				if (ends_sentence())
					flush();
				else
					buf += ' ';
			} else {
				buf += c;
			}
			if (buf.size() > 200)
				cut_at_comma();
		}
	}

	void flush()
	{
		std::string s = buf;

		buf.clear();
		while (!s.empty() && s.back() == ' ')
			s.pop_back();
		if (!s.compare(0, 2, "- ") || !s.compare(0, 4, "\xe2\x80\xa2 "))	// a list item
			s.erase(0, s[0] == '-' ? 2 : 4);
		if (!s.empty())
			say(s);
	}

	void drop() { buf.clear(); }

private:
	std::string buf;

	bool ends_sentence() const
	{
		static const char *const ABBREVIATIONS[2][16] = {
			{ "mr", "mrs", "ms", "dr", "st", "vs", "jr", "sr", "prof", "approx", nullptr },
			{ "dr", "nr", "st", "bzw", "ca", "vgl", "prof", "hr", "fr", "bspw", "ggf", "inkl",
			  "evtl", "ziff", "abs", nullptr },
		};
		size_t p = buf.size() - 1, start;

		if (p > 0 && strchr("\"')", buf[p]))
			p--;
		if (!strchr(".!?", buf[p]))
			return false;
		if (buf[p] != '.')
			return true;
		start = buf.rfind(' ', p);
		start = start == std::string::npos ? 0 : start + 1;
		while (start < p && strchr("\"'(", buf[start]))
			start++;
		std::string word = buf.substr(start, p - start);
		if (word.empty())
			return false;
		if (word.size() == 1 && isalpha((unsigned char)word[0]))
			return false;	// an initial, "z. B."
		if (word.find('.') != std::string::npos)
			return false;	// "e.g.", "z.B."
		if (lang == DE && word.size() <= 2 && word.find_first_not_of("0123456789") == std::string::npos)
			return false;	// "am 22. November"
		for (char &c : word)
			c = (char)tolower((unsigned char)c);
		for (const char *const *a = ABBREVIATIONS[lang]; *a; a++)
			if (word == *a)
				return false;
		return true;
	}

	void cut_at_comma()
	{
		for (size_t p = buf.size() - 1; p > 40; p--)
			if (buf[p] == ' ' && strchr(",;:", buf[p - 1])) {
				std::string rest = buf.substr(p + 1);

				buf.resize(p);
				flush();
				buf = rest;
				return;
			}
	}
};

static Sentences out;

static void speak(Spoken s)
{
	out.drop();
	out.add(SPOKEN[lang][s]);
	out.flush();
}

// The network ---------------------------------------------------------------------

struct Url {
	bool tls = true;
	std::string host, port, path;
};

static bool parse_url(const std::string &s, Url *u)
{
	size_t p;

	if (!s.compare(0, 8, "https://")) {
		u->tls = true;
		p = 8;
	} else if (!s.compare(0, 7, "http://")) {
		u->tls = false;
		p = 7;
	} else {
		return false;
	}
	size_t slash = s.find('/', p);
	std::string authority = s.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
	u->path = slash == std::string::npos ? "/" : s.substr(slash);
	size_t colon = authority.rfind(':');
	u->host = authority.substr(0, colon);
	u->port = colon == std::string::npos ? (u->tls ? "443" : "80") : authority.substr(colon + 1);
	if (u->host.empty() || u->host.find_first_of("@[] ") != std::string::npos ||
	    u->port.empty() || u->port.find_first_not_of("0123456789") != std::string::npos ||
	    u->path.find_first_of(" \r\n") != std::string::npos)
		return false;
	// the key goes in clear over plain HTTP: only to this machine
	return u->tls || u->host == "127.0.0.1" || u->host == "localhost";
}

// Waits until fd is ready or the time (now_ms) is reached: false then
static bool wait_fd(int fd, short events, long until)
{
	for (;;) {
		long left = until - now_ms();
		struct pollfd p = { fd, events, 0 };

		if (left <= 0)
			return false;
		int r = poll(&p, 1, (int)left);
		if (r > 0 || (r < 0 && errno != EINTR))
			return true;	// the next read or write tells
	}
}

// getaddrinfo() cannot be given a time limit: it runs in a thread that is
// left behind if it takes too long (the process ends soon after).
struct Lookup {
	std::mutex m;
	std::condition_variable done_cv;
	bool done = false;
	int rc = 0;
	struct addrinfo *res = nullptr;
};

static struct addrinfo *resolve(const Url &u, long until, std::string *why)
{
	auto lookup = std::make_shared<Lookup>();
	std::string host = u.host, port = u.port;

	std::thread([lookup, host, port] {
		struct addrinfo hints = {}, *res = nullptr;

		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_ADDRCONFIG;
		int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
		std::lock_guard<std::mutex> g(lookup->m);
		lookup->rc = rc;
		lookup->res = res;
		lookup->done = true;
		lookup->done_cv.notify_all();
	}).detach();
	std::unique_lock<std::mutex> g(lookup->m);
	if (!lookup->done_cv.wait_for(g, std::chrono::milliseconds(std::max(0L, until - now_ms())),
				      [&] { return lookup->done; })) {
		*why = "name lookup timed out";
		return nullptr;
	}
	if (lookup->rc) {
		*why = gai_strerror(lookup->rc);
		return nullptr;
	}
	struct addrinfo *res = lookup->res;
	lookup->res = nullptr;
	return res;
}

static SSL_CTX *ssl_ctx;

static bool tls_init(const char *ca_file, std::string *why)
{
	ssl_ctx = SSL_CTX_new(TLS_client_method());
	if (!ssl_ctx) {
		*why = "no TLS context";
		return false;
	}
	SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
	SSL_CTX_set_options(ssl_ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	if (SSL_CTX_load_verify_locations(ssl_ctx, ca_file, nullptr) != 1) {
		*why = std::string(ca_file) + ": no CA certificates";
		return false;
	}
	return true;
}

// One connection, plain or TLS, non-blocking; every wait ends at the
// exchange's deadline
class Conn {
public:
	~Conn() { close(); }

	bool open(const Url &u, std::string *why)
	{
		long until = std::min(now_ms() + CONNECT_MS, deadline_ms);
		struct addrinfo *res = resolve(u, until, why);
		int left = 0;

		for (struct addrinfo *a = res; a; a = a->ai_next)
			left++;
		for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next, left--) {
			long slot = now_ms() + std::max(0L, until - now_ms()) / left;

			fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
			if (fd < 0)
				continue;
			if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) {
				*why = strerror(errno);
			} else if (!wait_fd(fd, POLLOUT, slot)) {
				*why = "connection timed out";
			} else {
				int err = 0;
				socklen_t len = sizeof(err);

				if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err)
					break;
				*why = strerror(err);
			}
			::close(fd);
			fd = -1;
		}
		if (res)
			freeaddrinfo(res);
		if (fd < 0)
			return false;
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		if (!u.tls)
			return true;
		ssl = SSL_new(ssl_ctx);
		if (!ssl || !SSL_set_fd(ssl, fd) || !SSL_set_tlsext_host_name(ssl, u.host.c_str()) ||
		    !SSL_set1_host(ssl, u.host.c_str())) {
			*why = "TLS setup failed";
			return false;
		}
		for (;;) {
			ERR_clear_error();
			int r = SSL_connect(ssl);
			if (r == 1)
				return true;
			int e = SSL_get_error(ssl, r);
			if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
				if (wait_fd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, until))
					continue;
				*why = "TLS handshake timed out";
			} else if (SSL_get_verify_result(ssl) != X509_V_OK) {
				*why = std::string("certificate: ") +
				       X509_verify_cert_error_string(SSL_get_verify_result(ssl));
			} else {
				const char *reason = ERR_reason_error_string(ERR_peek_error());
				*why = std::string("TLS handshake failed: ") + (reason ? reason : "connection closed");
			}
			close();
			return false;
		}
	}

	bool write_all(const std::string &data)
	{
		for (size_t off = 0; off < data.size();) {
			if (ssl) {
				ERR_clear_error();
				int r = SSL_write(ssl, data.data() + off, (int)std::min(data.size() - off, (size_t)65536));
				if (r > 0) {
					off += (size_t)r;
					continue;
				}
				int e = SSL_get_error(ssl, r);
				if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) &&
				    wait_fd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline_ms))
					continue;
				return false;
			}
			ssize_t r = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
			if (r > 0) {
				off += (size_t)r;
				continue;
			}
			if (r < 0 && (errno == EAGAIN || errno == EINTR) && wait_fd(fd, POLLOUT, deadline_ms))
				continue;
			return false;
		}
		return true;
	}

	// > 0 bytes read, 0 the end, -1 an error or the deadline
	long read(char *buf, size_t len)
	{
		for (;;) {
			if (ssl) {
				ERR_clear_error();
				int r = SSL_read(ssl, buf, (int)len);
				if (r > 0)
					return r;
				int e = SSL_get_error(ssl, r);
				if (e == SSL_ERROR_ZERO_RETURN)
					return 0;
				if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) &&
				    wait_fd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline_ms))
					continue;
				return -1;
			}
			ssize_t r = recv(fd, buf, len, 0);
			if (r >= 0)
				return r;
			if ((errno == EAGAIN || errno == EINTR) && wait_fd(fd, POLLIN, deadline_ms))
				continue;
			return -1;
		}
	}

	void close()
	{
		if (ssl)
			SSL_free(ssl);
		ssl = nullptr;
		if (fd >= 0)
			::close(fd);
		fd = -1;
	}

private:
	int fd = -1;
	SSL *ssl = nullptr;
};

// An HTTP/1.1 response: status and headers, then the body as it arrives,
// chunked or not
class Http {
public:
	explicit Http(Conn &c) : conn(c) {}

	bool head(int *status_code)
	{
		size_t end;

		while ((end = in.find("\r\n\r\n")) == std::string::npos)
			if (in.size() > 32768 || !more())
				return false;
		if (in.compare(0, 7, "HTTP/1.") || in.size() < 12)
			return false;
		*status_code = atoi(in.c_str() + 9);
		for (size_t p = in.find("\r\n") + 2; p < end;) {
			size_t eol = in.find("\r\n", p);
			std::string name = in.substr(p, in.find(':', p) - p), value;

			if (name.size() < eol - p)
				value = in.substr(p + name.size() + 1, eol - p - name.size() - 1);
			for (char &c : name)
				c = (char)tolower((unsigned char)c);
			for (char &c : value)
				c = (char)tolower((unsigned char)c);
			if (name == "transfer-encoding" && value.find("chunked") != std::string::npos)
				chunked = true;
			else if (name == "content-length")
				length = atol(value.c_str());
			p = eol + 2;
		}
		in.erase(0, end + 4);
		return true;
	}

	// Hands the body to sink(data, len) until it ends (1) or sink returns
	// false (0); -1: broken off
	template <class Sink> int body(Sink sink)
	{
		for (;;) {
			if (chunked && chunk_left == 0) {
				size_t eol = in.find("\r\n", crlf_due ? 2 : 0);

				if (eol == std::string::npos) {
					if (!more())
						return -1;
					continue;
				}
				size_t from = crlf_due ? 2 : 0;
				if ((crlf_due && in.compare(0, 2, "\r\n")) || eol == from ||
				    !isxdigit((unsigned char)in[from]))
					return -1;
				chunk_left = (long)strtoul(in.c_str() + from, nullptr, 16);
				in.erase(0, eol + 2);
				crlf_due = true;
				if (chunk_left == 0)
					return 1;
				continue;
			}
			if (!in.empty() && (!chunked || chunk_left > 0)) {
				size_t n = in.size();

				if (chunked || length >= 0)
					n = std::min(n, (size_t)(chunked ? chunk_left : length));
				bool go = sink(in.data(), n);
				in.erase(0, n);
				if (chunked)
					chunk_left -= (long)n;
				else if (length >= 0)
					length -= (long)n;
				if (!go)
					return 0;
				continue;
			}
			if (!chunked && length == 0)
				return 1;
			if (!more())
				return !chunked && length < 0 && at_end ? 1 : -1;
		}
	}

private:
	Conn &conn;
	std::string in;
	bool chunked = false, crlf_due = false, at_end = false;
	long length = -1, chunk_left = 0;

	bool more()
	{
		char buf[4096];
		long n = conn.read(buf, sizeof(buf));

		if (n > 0)
			in.append(buf, (size_t)n);
		at_end = n == 0;
		return n > 0;
	}
};

// Server-sent events: "event:" and "data:" lines, an empty line ends one
class Sse {
public:
	template <class Event> bool feed(const char *p, size_t n, Event event)
	{
		for (const char *end = p + n; p < end; p++) {
			if (*p == '\r')
				continue;
			if (*p != '\n') {
				if (line.size() < (1 << 20))
					line += *p;
				continue;
			}
			if (line.empty()) {
				bool go = data.empty() || event(data);

				data.clear();
				if (!go)
					return false;
			} else if (!line.compare(0, 5, "data:")) {
				if (!data.empty())
					data += '\n';
				data.append(line, line.size() > 5 && line[5] == ' ' ? 6 : 5, std::string::npos);
			}	// "event:" repeats the data's type; ":" is a comment
			line.clear();
		}
		return true;
	}

private:
	std::string line, data;
};

// The exchange ---------------------------------------------------------------------

static const char *str(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

struct Block {
	std::string type, text, id, name, input;	// input: the tool's JSON as it streamed
};

struct Round {
	std::vector<Block> blocks;
	std::string stop_reason, error;
	bool stopped = false;
	int said = 0;	// sentences of this round's text
};

// One event of the stream; false: the stream is over
static bool on_event(Round *r, const std::string &data)
{
	cJSON *j = cJSON_ParseWithLength(data.data(), data.size());
	std::string type = str(j, "type");
	const cJSON *index = cJSON_GetObjectItemCaseSensitive(j, "index");
	Block *b = nullptr;
	bool go = true;

	if (cJSON_IsNumber(index) && index->valueint >= 0 && index->valueint < 64) {
		if ((size_t)index->valueint >= r->blocks.size())
			r->blocks.resize((size_t)index->valueint + 1);
		b = &r->blocks[(size_t)index->valueint];
	}
	if (type == "content_block_start" && b) {
		const cJSON *block = cJSON_GetObjectItemCaseSensitive(j, "content_block");

		b->type = str(block, "type");
		b->id = str(block, "id");
		b->name = str(block, "name");
		b->text = str(block, "text");
		if (b->type == "text") {
			int before = sentences_said;
			out.add(b->text);
			r->said += sentences_said - before;
		}
	} else if (type == "content_block_delta" && b) {
		const cJSON *delta = cJSON_GetObjectItemCaseSensitive(j, "delta");
		std::string kind = str(delta, "type");

		if (kind == "text_delta" && b->type == "text") {
			std::string text = str(delta, "text");
			int before = sentences_said;

			b->text += text;
			out.add(text);
			r->said += sentences_said - before;
		} else if (kind == "input_json_delta" && b->type == "tool_use") {
			b->input += str(delta, "partial_json");
		}
	} else if (type == "message_delta") {
		const char *stop = str(cJSON_GetObjectItemCaseSensitive(j, "delta"), "stop_reason");

		if (*stop)
			r->stop_reason = stop;
	} else if (type == "message_stop") {
		r->stopped = true;
		go = false;
	} else if (type == "error") {
		r->error = str(cJSON_GetObjectItemCaseSensitive(j, "error"), "type");
		if (r->error.empty())
			r->error = "error";
		go = false;
	}	// message_start, content_block_stop, ping and new kinds: nothing to do
	cJSON_Delete(j);
	return go;
}

static Spoken spoken_error(const std::string &type)
{
	if (type == "authentication_error" || type == "permission_error")
		return S_INVALID_KEY;
	if (type == "rate_limit_error" || type == "overloaded_error")
		return S_BUSY;
	return S_FAILED;
}

// One request of the exchange, its answer streamed into speech. false and
// *err: it failed.
static bool request(const Url &url, const std::string &key, const std::string &body, int n, Round *r,
		    Spoken *err)
{
	long t0 = now_ms();
	std::string why;
	Conn conn;

	if (!conn.open(url, &why)) {
		status("request %d: %s: %s", n, url.host.c_str(), why.c_str());
		*err = S_OFFLINE;
		return false;
	}
	std::string host = url.host;
	if (url.port != (url.tls ? "443" : "80"))
		host += ":" + url.port;
	// headers: api/overview "Authentication" (x-api-key, anthropic-version,
	// content-type); the body: api/messages "Create a Message". Built in a
	// string reserved at its full size, so the key is never left behind in
	// a reallocated buffer; wiped after sending.
	std::string head = "POST " + url.path + " HTTP/1.1\r\n"
			   "Host: " + host + "\r\n"
			   "x-api-key: ";
	std::string tail = std::string("\r\n"
			   "anthropic-version: ") + API_VERSION + "\r\n"
			   "content-type: application/json\r\n"
			   "accept: text/event-stream\r\n"
			   "user-agent: lidar-claude\r\n"
			   "content-length: " + std::to_string(body.size()) + "\r\n"
			   "connection: close\r\n\r\n";
	std::string req;
	req.reserve(head.size() + key.size() + tail.size() + body.size());
	req += head;
	req += key;
	req += tail;
	req += body;
	bool sent = conn.write_all(req);
	OPENSSL_cleanse(&req[0], req.size());
	if (!sent) {
		status("request %d: sending failed", n);
		*err = S_OFFLINE;
		return false;
	}
	Http http(conn);
	int code = 0;
	if (!http.head(&code)) {
		status("request %d: no answer after %.1f s", n, seconds(t0));
		*err = S_OFFLINE;
		return false;
	}
	if (code != 200) {
		// api/errors: {"type": "error", "error": {"type": ..., "message": ...}}
		std::string text;
		http.body([&](const char *p, size_t len) {
			text.append(p, std::min(len, MAX_ERROR_BODY - text.size()));
			return text.size() < MAX_ERROR_BODY;
		});
		cJSON *j = cJSON_ParseWithLength(text.data(), text.size());
		std::string type = str(cJSON_GetObjectItemCaseSensitive(j, "error"), "type");
		cJSON_Delete(j);
		status("request %d: HTTP %d %s", n, code, type.empty() ? "(no error type)" : type.c_str());
		*err = code == 401 || code == 403 ? S_INVALID_KEY : code == 429 || code == 529 ? S_BUSY :
		       spoken_error(type);
		return false;
	}
	Sse sse;
	http.body([&](const char *p, size_t len) {
		return sse.feed(p, len, [&](const std::string &data) { return on_event(r, data); });
	});
	if (!r->error.empty()) {
		status("request %d: error event %s after %.1f s", n, r->error.c_str(), seconds(t0));
		*err = spoken_error(r->error);
		return false;
	}
	if (!r->stopped) {
		status("request %d: stream broken off after %.1f s", n, seconds(t0));
		*err = now_ms() >= deadline_ms ? S_OFFLINE : S_FAILED;
		return false;
	}
	status("request %d: %.1f s, stop %s", n, seconds(t0),
	       r->stop_reason.empty() ? "(none)" : r->stop_reason.c_str());
	return true;
}

// Tools ---------------------------------------------------------------------------

// The FIFO lidar-assistant answers one tool call on,
// <run dir>/lidar-claude.<pid>.<n>: made for the call, removed after it
class ReplyFifo {
public:
	~ReplyFifo()
	{
		if (!reply_live)
			return;
		reply_live = 0;
		close(fd);
		unlink(reply_path);
	}

	bool make()
	{
		if (snprintf(reply_path, sizeof(reply_path), "%s/lidar-claude.%d.%u", run_dir.c_str(),
			     (int)getpid(), ++tool_calls) >= (int)sizeof(reply_path))
			return false;
		unlink(reply_path);
		if (mkfifo(reply_path, 0600))
			return false;
		// read-write: never end of file, whoever closes
		fd = open(reply_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			unlink(reply_path);
			return false;
		}
		reply_live = 1;
		return true;
	}

	int fd = -1;
};

static std::string percent_encode(const std::string &v)
{
	std::string s;
	char hex[4];

	for (unsigned char c : v)
		if (c <= ' ' || c == '%' || c == '=' || c == 0x7f) {
			snprintf(hex, sizeof(hex), "%%%02X", c);
			s += hex;
		} else {
			s += (char)c;
		}
	return s;
}

// A tool_use block as a line for lidar-assistant: its input's members as
// name=value, whole numbers in decimal, true/false as 1/0, lists joined by
// commas. lidar-assistant checks names, types and ranges.
static bool tool_line(const Block &b, std::string *line, std::string *why)
{
	static const char NAME[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
	cJSON *in = b.input.empty() ? cJSON_CreateObject() : cJSON_ParseWithLength(b.input.data(), b.input.size());
	const cJSON *members = cJSON_IsObject(in) ? in : nullptr, *item;

	*line = std::string("!tool ") + reply_path + " " + b.name;
	if (b.name.empty() || b.name.size() > 32 || b.name.find_first_not_of(NAME) != std::string::npos)
		*why = "there is no such tool";
	else if (!cJSON_IsObject(in))
		*why = "the input is not a JSON object";
	cJSON_ArrayForEach(item, members) {
		std::string name = item->string ? item->string : "", value;

		if (!why->empty())
			break;
		if (name.empty() || name.size() > 32 || name.find_first_not_of(NAME) != std::string::npos) {
			*why = "there is no parameter of that name";
		} else if (cJSON_IsNumber(item)) {
			if (item->valuedouble != std::floor(item->valuedouble) || std::fabs(item->valuedouble) > 1e9)
				*why = name + " has to be a whole number";
			else
				value = std::to_string((long long)item->valuedouble);
		} else if (cJSON_IsBool(item)) {
			value = cJSON_IsTrue(item) ? "1" : "0";
		} else if (cJSON_IsString(item)) {
			value = item->valuestring;
		} else if (cJSON_IsArray(item)) {
			const cJSON *e;

			cJSON_ArrayForEach(e, item) {
				if (!cJSON_IsString(e) || strchr(e->valuestring, ','))
					*why = name + " has to be a list of names";
				else
					value += (value.empty() ? "" : ",") + std::string(e->valuestring);
			}
		} else if (cJSON_IsNull(item)) {
			continue;	// an optional parameter left out
		} else {
			*why = name + " has a type the tool does not take";
		}
		*line += " " + name + "=" + percent_encode(value);
	}
	cJSON_Delete(in);
	if (why->empty() && line->size() > MAX_LINE)
		*why = "the input is too long";
	*line += '\n';
	return why->empty();
}

// Hands a tool_use block to lidar-assistant -> the tool_result content, its
// spoken answer in *say, *error if it failed
static std::string run_tool(const Block &b, std::string *say_text, bool *error)
{
	std::string line, why, reply, fifo = run_dir + "/lidar-assistant.fifo";
	ReplyFifo reply_fifo;
	char buf[512];
	struct stat st;
	int fd = -1;

	*error = true;
	if (!reply_fifo.make())
		why = "no FIFO for the answer";
	else if (tool_line(b, &line, &why) &&
		 ((fd = open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC)) < 0 || fstat(fd, &st) ||
		  !S_ISFIFO(st.st_mode)))
		why = "the speaker's assistant is not running";
	if (!why.empty()) {
		if (fd >= 0)
			close(fd);
		status("tool %s: %s", b.name.c_str(), why.c_str());
		return "Error: " + why;
	}
	ssize_t n = write(fd, line.data(), line.size());	// one write: one line, whole
	close(fd);
	long until = std::min(now_ms() + TOOL_MS, deadline_ms);
	while (n == (ssize_t)line.size() && reply.find('\n') == std::string::npos && reply.size() < 8192) {
		if (!wait_fd(reply_fifo.fd, POLLIN, until))
			break;
		ssize_t r = read(reply_fifo.fd, buf, sizeof(buf));
		if (r > 0)
			reply.append(buf, (size_t)r);
		else if (r < 0 && errno != EAGAIN && errno != EINTR)
			break;
	}
	if (reply.find('\n') == std::string::npos) {
		status("tool %s: no answer from lidar-assistant", b.name.c_str());
		return "Error: the speaker did not carry it out in time";
	}
	reply.resize(reply.find('\n'));
	cJSON *j = cJSON_Parse(reply.c_str());
	*error = !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok"));
	*say_text = str(j, "say");
	status("tool %s: %s", b.name.c_str(), *error ? str(j, "code") : "ok");
	cJSON_Delete(j);
	return reply;
}

// The tool list: lidar-assistant's actions, already in the API's format
// (name, description, input_schema)
static cJSON *load_tools()
{
	FILE *p = popen("lidar-assistant -S 2>/dev/null", "re");
	std::string text;
	char buf[4096];
	size_t n;

	while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0)
		text.append(buf, n);
	int rc = p ? pclose(p) : -1;
	cJSON *tools = cJSON_ParseWithLength(text.data(), text.size());
	if (rc || !cJSON_IsArray(tools) || !cJSON_GetArraySize(tools)) {
		status("no tools: lidar-assistant -S failed");
		cJSON_Delete(tools);
		return nullptr;
	}
	return tools;
}

// The API key: wiped when it goes
struct Key {
	std::string s;
	~Key()
	{
		if (!s.empty())
			OPENSSL_cleanse(&s[0], s.size());
	}
};

// The API key, read from lidar-feature through a pipe with read(): no stdio
// buffer, one copy (in *key), the buffer wiped
static void read_key(Key *key)
{
	char buf[512];
	size_t len = 0;
	int fds[2];

	if (pipe2(fds, O_CLOEXEC))
		return;
	pid_t pid = fork();
	if (!pid) {
		int null = open("/dev/null", O_WRONLY);

		dup2(fds[1], 1);
		if (null >= 0)
			dup2(null, 2);
		execlp("lidar-feature", "lidar-feature", "get", "voice", "api_key", (char *)nullptr);
		_exit(127);
	}
	close(fds[1]);
	while (pid > 0 && len < sizeof(buf)) {
		ssize_t n = read(fds[0], buf + len, sizeof(buf) - len);

		if (n > 0)
			len += (size_t)n;
		else if (n == 0 || errno != EINTR)
			break;
	}
	close(fds[0]);
	if (pid > 0)
		while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
			;
	const char *eol = (const char *)memchr(buf, '\n', len);
	size_t end = eol ? (size_t)(eol - buf) : len;
	while (end > 0 && strchr("\r \t", buf[end - 1]))
		end--;
	key->s.assign(buf, end);
	OPENSSL_cleanse(buf, sizeof(buf));
}

static std::string system_prompt(Lang l)
{
	std::string s =
		"You are the voice assistant of a smart speaker in someone's home. What you write is read "
		"aloud by a speech synthesizer, so answer in one or two short sentences of plain spoken "
		"text: no markdown, no lists, no emoji, no links. Write numbers, times and units so that "
		"they read naturally aloud. The question comes from speech recognition and may contain "
		"misheard words; answer what was most likely meant. Always answer in ";
	s += l == DE ? "German" : "English";
	s += ". Use the tools for what the speaker itself can do (timers, alarms, volume, music, time "
	     "and date); do not announce a tool call, answer briefly after its result. A tool result's "
	     "\"say\" is the speaker's own answer to it. You have no internet access: for news, weather "
	     "or other current information, say briefly that you cannot know it. ";

	time_t now = time(nullptr);
	struct tm tm;
	char date[80], zone[8];
	const char *tz = getenv("TZ");

	if (access((run_dir + "/lidar-time.synced").c_str(), F_OK) || !localtime_r(&now, &tm) ||
	    !strftime(date, sizeof(date), "%A, %B %d, %Y, %H:%M", &tm) || !strftime(zone, sizeof(zone), "%z", &tm)) {
		s += "The current date and time are not known.";
		return s;
	}
	s += std::string("It is now ") + date + " local time (UTC" + std::string(zone, 3) + ":" + (zone + 3);
	if (tz && *tz)
		s += std::string(", time zone ") + tz;
	s += ").";
	return s;
}

static std::string request_body(const std::string &model, const std::string &system, cJSON *messages,
				cJSON *tools, bool no_more_tools)
{
	cJSON *req = cJSON_CreateObject();

	cJSON_AddStringToObject(req, "model", model.c_str());
	cJSON_AddNumberToObject(req, "max_tokens", MAX_TOKENS);
	cJSON_AddStringToObject(req, "system", system.c_str());
	cJSON_AddItemReferenceToObject(req, "messages", messages);
	if (tools) {
		cJSON_AddItemReferenceToObject(req, "tools", tools);
		if (no_more_tools)
			cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "tool_choice"), "type", "none");
	}
	// short answers soon: no thinking (Haiku 5.5 takes "disabled" at effort
	// high or below), low effort; build-with-claude/effort
	cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "thinking"), "type", "disabled");
	cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "output_config"), "effort", "low");
	cJSON_AddBoolToObject(req, "stream", 1);
	char *s = cJSON_PrintUnformatted(req);
	std::string body = s ? s : "";
	cJSON_free(s);
	cJSON_Delete(req);
	return body;
}

// Invalid UTF-8 would make the request invalid
static std::string utf8(const std::string &s)
{
	std::string o;

	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		size_t len = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 :
			     c >= 0xF0 && c < 0xF5 ? 4 : 0, k = 1;

		while (k < len && i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80)
			k++;
		if (len && k == len && (len > 1 || c >= 0x20)) {
			o.append(s, i, len);
			i += len;
		} else {
			o += c < 0x20 ? ' ' : '?';
			i++;
		}
	}
	return o;
}

static int answer(const std::string &utterance)
{
	Key key;

	wake_seen = wake_now();

	read_key(&key);
	if (key.s.empty()) {
		// no Claude: what the grammar did not understand stays not
		// understood; how to change that is said once per boot
		int first = open((run_dir + "/lidar-claude.hinted").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

		status("no API key set");
		speak(S_NOT_UNDERSTOOD);
		if (first >= 0) {
			close(first);
			speak(S_KEY_HINT);
		}
		return 1;
	}
	for (char c : key.s)
		if (c < 0x21 || c > 0x7e) {
			status("the API key has characters an HTTP header cannot carry");
			speak(S_INVALID_KEY);
			return 1;
		}
	const char *env_url = getenv("LIDAR_CLAUDE_URL"), *env_model = getenv("LIDAR_CLAUDE_MODEL"),
		   *env_ca = getenv("LIDAR_CLAUDE_CAFILE");
	std::string model = env_model && *env_model ? env_model : MODEL, why;
	Url url;
	if (!parse_url(env_url && *env_url ? env_url : API_URL, &url)) {
		status("LIDAR_CLAUDE_URL: not an https URL (http only to this machine)");
		speak(S_FAILED);
		return 1;
	}
	if (url.tls && !tls_init(env_ca && *env_ca ? env_ca : CA_BUNDLE, &why)) {
		status("%s", why.c_str());
		speak(S_FAILED);
		return 1;
	}
	say(SPOKEN[lang][S_ASKING], true);
	cJSON *tools = load_tools();
	std::string system = system_prompt(lang);
	cJSON *messages = cJSON_CreateArray(), *user = cJSON_CreateObject();
	cJSON_AddStringToObject(user, "role", "user");
	cJSON_AddStringToObject(user, "content", utterance.c_str());
	cJSON_AddItemToArray(messages, user);

	std::vector<std::string> tool_says;
	int requests = 0, tools_run = 0, rc = 0;
	for (;;) {
		bool no_more_tools = requests == MAX_TOOL_ROUNDS;

		stop_if_woken();
		std::string body = request_body(model, system, messages, tools, no_more_tools);
		Round r;
		Spoken err;

		if (!request(url, key.s, body, ++requests, &r, &err)) {
			speak(err);
			rc = 1;
			break;
		}
		int before = sentences_said;
		out.flush();
		r.said += sentences_said - before;
		bool wants_tools = false;
		for (const Block &b : r.blocks)
			wants_tools |= b.type == "tool_use";
		if (r.stop_reason != "tool_use" || !wants_tools || no_more_tools || !tools) {
			// nothing said in the end: what the tools answered, or that
			// there is no answer
			if (!r.said && !tool_says.empty())
				for (const std::string &s : tool_says) {
					out.add(s);
					out.flush();
				}
			else if (!sentences_said)
				speak(S_NO_ANSWER);
			break;
		}
		// tool use: agents-and-tools/tool-use/handle-tool-calls, "Handling
		// results from client tools": the answer's content as it was, then
		// a tool_result for each tool_use
		cJSON *assistant = cJSON_CreateObject(), *content = cJSON_CreateArray();
		cJSON *results_msg = cJSON_CreateObject(), *results = cJSON_CreateArray();
		cJSON_AddStringToObject(assistant, "role", "assistant");
		cJSON_AddItemToObject(assistant, "content", content);
		cJSON_AddStringToObject(results_msg, "role", "user");
		cJSON_AddItemToObject(results_msg, "content", results);
		tool_says.clear();
		for (const Block &b : r.blocks) {
			if (b.type == "text" && !b.text.empty()) {
				cJSON *t = cJSON_CreateObject();
				cJSON_AddStringToObject(t, "type", "text");
				cJSON_AddStringToObject(t, "text", b.text.c_str());
				cJSON_AddItemToArray(content, t);
			} else if (b.type == "tool_use") {
				cJSON *t = cJSON_CreateObject(), *input = b.input.empty() ? nullptr :
					cJSON_ParseWithLength(b.input.data(), b.input.size());
				cJSON_AddStringToObject(t, "type", "tool_use");
				cJSON_AddStringToObject(t, "id", b.id.c_str());
				cJSON_AddStringToObject(t, "name", b.name.c_str());
				cJSON_AddItemToObject(t, "input", cJSON_IsObject(input) ? input : cJSON_CreateObject());
				if (input && !cJSON_IsObject(input))
					cJSON_Delete(input);
				cJSON_AddItemToArray(content, t);

				std::string said;
				bool error;
				std::string result = run_tool(b, &said, &error);
				tools_run++;
				if (!error && !said.empty())
					tool_says.push_back(said);
				cJSON *res = cJSON_CreateObject();
				cJSON_AddStringToObject(res, "type", "tool_result");
				cJSON_AddStringToObject(res, "tool_use_id", b.id.c_str());
				cJSON_AddStringToObject(res, "content", result.c_str());
				if (error)
					cJSON_AddBoolToObject(res, "is_error", 1);
				cJSON_AddItemToArray(results, res);
			}
		}
		cJSON_AddItemToArray(messages, assistant);
		cJSON_AddItemToArray(messages, results_msg);
	}
	say_wait();
	cJSON_Delete(messages);
	cJSON_Delete(tools);
	if (sentences_said)
		status("%s after %.1f s: %d request(s), %d tool call(s), %d sentence(s), the first after %.1f s",
		       rc ? "failed" : "answered", seconds(start_ms), requests, tools_run, sentences_said,
		       (first_said_ms - start_ms) / 1000.0);
	return rc;
}

static void on_signal(int)
{
	if (reply_live)
		unlink(reply_path);
	_exit(1);
}

int main(int argc, char **argv)
{
	const char *env_say = getenv("LIDAR_SAY_COMMAND");
	std::string utterance;
	int c;

	if (env_say)
		say_command = env_say;
	while ((c = getopt(argc, argv, "y:r:eP")) != -1) {
		switch (c) {
		case 'y': say_command = optarg; break;
		case 'r': run_dir = optarg; break;
		case 'e': log_stderr = true; break;
		case 'P': listing = true; break;
		default: optind = argc; break;
		}
	}
	if (optind >= argc || (strcmp(argv[optind], "en") && strcmp(argv[optind], "de"))) {
		fprintf(stderr, "usage: lidar-claude [-y <say command>] [-r <run dir>] [-e] en|de [<utterance>...]\n"
				"       lidar-claude -P en|de\n");
		return 2;
	}
	lang = argv[optind][0] == 'd' ? DE : EN;
	if (listing) {
		for (const char *s : SPOKEN[lang]) {
			out.add(s);
			out.flush();
		}
		return 0;
	}
	for (int i = optind + 1; i < argc; i++)
		utterance += (i > optind + 1 ? " " : "") + std::string(argv[i]);
	if (optind + 1 == argc) {
		char line[2048];

		if (fgets(line, sizeof(line), stdin))
			utterance = line;
	}
	utterance = utf8(utterance);
	utterance.erase(0, utterance.find_first_not_of(' '));
	utterance.erase(utterance.find_last_not_of(' ') + 1);
	if (utterance.size() > MAX_UTTERANCE)
		utterance.resize(MAX_UTTERANCE);	// may cut a character: utf8() again
	utterance = utf8(utterance);
	if (utterance.empty()) {
		fprintf(stderr, "lidar-claude: nothing to answer\n");
		return 2;
	}

	struct sigaction sa = {};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	signal(SIGPIPE, SIG_IGN);
	start_ms = now_ms();
	deadline_ms = start_ms + TOTAL_MS;
	return answer(utterance);
}
