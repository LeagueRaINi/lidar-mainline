// SPDX-License-Identifier: GPL-2.0-only
// lidar-assistant: the local grammar. It takes one utterance as a speech
// recogniser writes it (any case, punctuation or none, numbers as words or
// digits) and returns the action it asks for, or "unmatched".
//
// How an utterance is matched:
//   1. It is split into lower-case words without accents ("zwölf" ->
//      "zwolf", "dreißig" -> "dreissig": the words in this file are written
//      that way), and number words become numbers ("twenty two" -> 22,
//      "zweiundzwanzig" -> 22, "anderthalb" -> 1.5).
//   2. A key word picks the subject (timer, alarm, volume, ...), which
//      takes what it understands out of the words: durations, times, days,
//      verbs.
//   3. Every word left over has to be a filler ("please", "the"). One
//      unknown word and the utterance is "unmatched": better to hand it on
//      than to act on half of a sentence.
//
// The subjects' words (key words, verbs, fillers) come from the language
// file, "words.*". What is a rule rather than a list is here, once per
// language: number words, durations, times of day ("quarter to eight",
// "halb sieben") and weekdays. A new language needs these rules, the number
// words of speech.cc and its file.
#include "assistant.h"

#include <cstring>

const char *const DAY_NAMES[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };

std::string Action::str() const
{
	std::string s = name;

	for (const auto &arg : args)
		s += " " + arg.first + "=" + arg.second;
	return s;
}

namespace {

struct Tok {
	std::string w;		// lower case, without accents
	std::string orig;	// lower case, as said (for names)
	long n = -1;		// its value, if it is a number
	bool half = false;	// ... and a half
	bool used = false;
};

// "a|b|c" contains the word
bool in(const std::string &w, const char *alts)
{
	for (const char *p = alts; *p;) {
		const char *e = strchr(p, '|');
		size_t len = e ? (size_t)(e - p) : strlen(p);

		if (len == w.size() && !memcmp(p, w.data(), len))
			return true;
		p += len + (e ? 1 : 0);
	}
	return false;
}

// Latin-1 letters (UTF-8 0xC3 0xA0-0xBF) without their accents
const char *fold(unsigned char c)
{
	static const char *const FOLD[32] = {
		"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
		nullptr, "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y",
		nullptr, "y",
	};
	if (c == 0x9F)
		return "ss";
	return c >= 0xA0 && c <= 0xBF ? FOLD[c - 0xA0] : nullptr;
}

std::vector<Tok> tokenize(const std::string &s)
{
	std::vector<Tok> out;
	Tok cur;
	int kind = 0;	// of the token being read: 1 letters, 2 digits

	auto flush = [&] {
		if (kind == 2 && cur.w.size() <= 6)
			cur.n = atol(cur.w.c_str());
		if (kind)
			out.push_back(cur);
		cur = Tok();
		kind = 0;
	};
	auto add = [&](int k, const std::string &w, const std::string &orig) {
		if (kind && kind != k)
			flush();
		kind = k;
		cur.w += w;
		cur.orig += orig;
	};

	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c >= 'a' && c <= 'z') {
			add(1, std::string(1, (char)c), std::string(1, (char)c));
		} else if (c >= '0' && c <= '9') {
			add(2, std::string(1, (char)c), std::string(1, (char)c));
		} else if (c == '\'') {
			// what's -> whats
		} else if (c == 0xE2 && !s.compare(i, 3, "\xE2\x80\x99")) {
			i += 2;	// typographic apostrophe
		} else if (c == 0xC3 && i + 1 < s.size()) {
			unsigned char d = (unsigned char)s[++i];

			if (d >= 0x80 && d <= 0x9E && d != 0x97)
				d += 0x20;	// upper case
			if (fold(d))
				add(1, fold(d), std::string{ (char)0xC3, (char)d });
			else
				flush();
		} else {
			flush();
		}
	}
	flush();
	return out;
}

// Number words ------------------------------------------------------------

const char *const EN_ONES[20] = {
	"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
	"eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
	"eighteen", "nineteen",
};
const char *const EN_TENS[8] = {
	"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
};

// 0-99 in words at t[i]: "seven", "twenty", "twenty two"; -1 if none.
// "seven thirty" stays two numbers.
long en_small(const std::vector<Tok> &t, size_t i, size_t *len)
{
	if (i >= t.size())
		return -1;
	for (int k = 0; k < 20; k++)
		if (t[i].w == EN_ONES[k]) {
			*len = 1;
			return k;
		}
	for (int k = 0; k < 8; k++) {
		if (t[i].w != EN_TENS[k])
			continue;
		*len = 1;
		for (int u = 1; u < 10 && i + 1 < t.size(); u++)
			if (t[i + 1].w == EN_ONES[u]) {
				*len = 2;
				return (k + 2) * 10 + u;
			}
		return (k + 2) * 10;
	}
	return -1;
}

void numbers_en(std::vector<Tok> &t)
{
	std::vector<Tok> out;

	for (size_t i = 0; i < t.size();) {
		size_t len = 0, more = 0;
		long v = en_small(t, i, &len), r;
		bool hundred = i + len < t.size() && t[i + len].w == "hundred";

		if (v < 0 && !hundred) {
			out.push_back(t[i++]);
			continue;
		}
		if (hundred) {	// "a hundred and twenty", "one hundred twenty"
			size_t j = i + ++len;

			v = (v < 0 ? 1 : v) * 100;
			if (j < t.size() && t[j].w == "and" && (r = en_small(t, j + 1, &more)) >= 0) {
				v += r;
				len += 1 + more;
			} else if ((r = en_small(t, j, &more)) >= 0) {
				v += r;
				len += more;
			}
		}
		Tok n = t[i];
		n.n = v;
		out.push_back(n);
		i += len;
	}
	t = out;
}

const char *const DE_UNITS[10] = {
	"", "ein", "zwei", "drei", "vier", "funf", "sechs", "sieben", "acht", "neun",
};
const char *const DE_TENS[8] = {
	"zwanzig", "dreissig", "vierzig", "funfzig", "sechzig", "siebzig", "achtzig", "neunzig",
};

// One German number word: "zwolf", "zweiundzwanzig", "hundertzwanzig"; -1 if
// it is none. "ein" and "eine" are not numbers here (they are articles
// too); durations and times take them as one where a number belongs.
long de_word(const std::string &w)
{
	static const struct {
		const char *w;
		int v;
	} BASE[] = {
		{ "null", 0 }, { "eins", 1 }, { "zwei", 2 }, { "zwo", 2 }, { "drei", 3 },
		{ "vier", 4 }, { "funf", 5 }, { "sechs", 6 }, { "sieben", 7 }, { "acht", 8 },
		{ "neun", 9 }, { "zehn", 10 }, { "elf", 11 }, { "zwolf", 12 },
		{ "dreizehn", 13 }, { "vierzehn", 14 }, { "funfzehn", 15 }, { "sechzehn", 16 },
		{ "siebzehn", 17 }, { "achtzehn", 18 }, { "neunzehn", 19 },
	};

	for (const auto &b : BASE)
		if (w == b.w)
			return b.v;
	for (int k = 0; k < 8; k++)
		if (w == DE_TENS[k])
			return (k + 2) * 10;
	for (int u = 1; u < 10; u++) {	// einundzwanzig
		std::string head = std::string(DE_UNITS[u]) + "und";

		if (w.compare(0, head.size(), head))
			continue;
		for (int k = 0; k < 8; k++)
			if (!w.compare(head.size(), std::string::npos, DE_TENS[k]))
				return (k + 2) * 10 + u;
	}
	size_t h = w.find("hundert");	// [zwei]hundert[und][zwanzig]
	if (h != std::string::npos) {
		long hundreds = h ? -1 : 1;

		for (int u = 1; u < 10; u++)
			if (w.substr(0, h) == DE_UNITS[u])
				hundreds = u;
		if (hundreds < 0)
			return -1;
		std::string rest = w.substr(h + 7);
		if (rest.empty())
			return hundreds * 100;
		if (rest.size() > 3 && !rest.compare(0, 3, "und"))
			rest = rest.substr(3);
		long r = de_word(rest);
		return r >= 0 && r < 100 ? hundreds * 100 + r : -1;
	}
	return -1;
}

void numbers_de(std::vector<Tok> &t)
{
	std::vector<Tok> out;

	for (Tok &tok : t) {
		if (tok.n >= 0)
			continue;
		tok.n = de_word(tok.w);
		if (tok.n >= 0)
			continue;
		// anderthalb, zweieinhalb
		size_t l = tok.w.size();
		if (in(tok.w, "anderthalb|eineinhalb|einundeinhalb"))
			tok.n = 1;
		else if (l > 7 && !tok.w.compare(l - 7, 7, "einhalb"))
			tok.n = de_word(tok.w.substr(0, l - 7));
		tok.half = tok.n >= 0;
	}
	// A recogniser may write the compound apart: "zwei und zwanzig"
	for (size_t i = 0; i < t.size(); i++) {
		bool unit = t[i].w == "ein" || (t[i].n >= 1 && t[i].n <= 9 && !t[i].half);

		if (unit && i + 2 < t.size() && t[i + 1].w == "und" && t[i + 2].n >= 20 &&
		    t[i + 2].n <= 90 && t[i + 2].n % 10 == 0 && de_word(t[i + 2].w) >= 0) {
			Tok n = t[i];
			n.n = (t[i].n < 0 ? 1 : t[i].n) + t[i + 2].n;
			out.push_back(n);
			i += 2;
		} else {
			out.push_back(t[i]);
		}
	}
	t = out;
}

// The words of one utterance and what was taken out of them -------------------

struct P {
	std::vector<Tok> t;

	bool is(size_t i, const char *alts) const
	{
		return i < t.size() && !t[i].used && t[i].n < 0 && in(t[i].w, alts);
	}
	bool num(size_t i) const { return i < t.size() && !t[i].used && t[i].n >= 0; }
	void use(size_t from, size_t to)
	{
		for (size_t i = from; i < to && i < t.size(); i++)
			t[i].used = true;
	}
	bool has(const char *alts) const
	{
		for (size_t i = 0; i < t.size(); i++)
			if (is(i, alts))
				return true;
		return false;
	}
	bool take(const char *alts)	// every such word
	{
		bool any = false;

		for (size_t i = 0; i < t.size(); i++)
			if (is(i, alts))
				t[i].used = any = true;
		return any;
	}
	// The words in a row: "unter der woche", each "a|b"
	bool seq(const char *pattern)
	{
		std::vector<std::string> el;

		for (const char *p = pattern; *p;) {
			const char *e = strchr(p, ' ');
			el.emplace_back(p, e ? (size_t)(e - p) : strlen(p));
			p += el.back().size() + (e ? 1 : 0);
		}
		for (size_t i = 0; i + el.size() <= t.size(); i++) {
			size_t k = 0;

			while (k < el.size() && is(i + k, el[k].c_str()))
				k++;
			if (k == el.size()) {
				use(i, i + k);
				return true;
			}
		}
		return false;
	}
	long first_number() const
	{
		for (size_t i = 0; i < t.size(); i++)
			if (num(i))
				return (long)i;
		return -1;
	}
	// Nothing left but fillers
	bool rest(const char *a, const char *b) const
	{
		for (const Tok &tok : t)
			if (!tok.used && (tok.n >= 0 || !(in(tok.w, a) || in(tok.w, b))))
				return false;
		return true;
	}
};

// Durations -----------------------------------------------------------------

long unit_en(const P &p, size_t i)
{
	return p.is(i, "second|seconds|sec|secs") ? 1 :
	       p.is(i, "minute|minutes|min|mins") ? 60 : p.is(i, "hour|hours") ? 3600 : 0;
}

long unit_de(const P &p, size_t i)
{
	return p.is(i, "sekunde|sekunden") ? 1 :
	       p.is(i, "minute|minuten|min") ? 60 : p.is(i, "stunde|stunden") ? 3600 : 0;
}

// One part of a duration at word i, in seconds, or -1; *end: the word after
long piece_en(const P &p, size_t i, size_t *end)
{
	long u;

	if (p.is(i, "a") && p.is(i + 1, "half|quarter"))	// in a quarter of an hour
		i++;

	if (p.is(i, "half")) {	// half an hour
		i += p.is(i + 1, "a|an") ? 2 : 1;
		*end = i + 1;
		return (u = unit_en(p, i)) ? u / 2 : -1;
	}
	if (p.is(i, "quarter")) {	// a quarter of an hour
		i += p.is(i + 1, "of") ? 2 : 1;
		i += p.is(i, "a|an") ? 1 : 0;
		*end = i + 1;
		return p.is(i, "hour") ? 900 : -1;
	}
	long halves;
	if (p.num(i))
		halves = 2 * p.t[i].n;
	else if (p.is(i, "a|an"))
		halves = 2;
	else
		return -1;
	i++;
	if (p.is(i, "and") && p.is(i + 1, "a") && p.is(i + 2, "half")) {	// two and a half hours
		halves++;
		i += 3;
	}
	if (!(u = unit_en(p, i)))
		return -1;
	i++;
	if (p.is(i, "and") && p.is(i + 1, "a") && p.is(i + 2, "half")) {	// an hour and a half
		halves++;
		i += 3;
	}
	*end = i;
	return halves * u / 2;
}

long piece_de(const P &p, size_t i, size_t *end)
{
	long u;

	if (p.is(i, "ein|eine|einer|einen|einem") &&
	    p.is(i + 1, "halbe|halben|viertel|dreiviertel|viertelstunde|dreiviertelstunde"))
		i++;	// in einer halben Stunde

	*end = i + 1;
	if (p.is(i, "viertelstunde"))
		return 900;
	if (p.is(i, "dreiviertelstunde"))
		return 2700;
	*end = i + 2;
	if (p.is(i, "halbe|halben"))	// eine halbe Stunde
		return (u = unit_de(p, i + 1)) ? u / 2 : -1;
	if (p.is(i, "viertel|dreiviertel") && p.is(i + 1, "stunde"))
		return p.t[i].w == "viertel" ? 900 : 2700;
	long halves;
	if (p.num(i))
		halves = 2 * p.t[i].n + p.t[i].half;
	else if (p.is(i, "ein|eine|einer|einen|einem"))
		halves = 2;
	else
		return -1;
	return (u = unit_de(p, i + 1)) ? halves * u / 2 : -1;
}

// The first duration in the utterance ("one hour and five minutes"), taken
// out of it: seconds, or -1. *at: its first word.
long duration(Lang lang, P &p, size_t *at)
{
	auto piece = lang == EN ? piece_en : piece_de;
	const char *conj = lang == EN ? "and" : "und";

	for (size_t i = 0; i < p.t.size(); i++) {
		size_t end, next;
		long total = piece(p, i, &end), more;

		if (total < 0)
			continue;
		while ((more = piece(p, end + (p.is(end, conj) ? 1 : 0), &next)) >= 0) {
			total += more;
			end = next;
		}
		p.use(i, end);
		*at = i;
		return total;
	}
	return -1;
}

// Times and days ----------------------------------------------------------------

struct When {
	int hour = -1, minute = 0;
	// Part of the day said with it: 0 none, 1 morning, 2 afternoon or
	// evening, 3 night, 4 midday, -1 the time is complete as it is
	int part = 0;
	int repeat = 0;		// weekdays it repeats on, bit 0 Monday
	std::string day;	// today, tomorrow or a weekday
	bool fail = false;
};

int weekday_en(const std::string &w, bool *plural)
{
	static const char *const NAMES[7] = {
		"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
	};
	for (int d = 0; d < 7; d++) {
		if (w == NAMES[d] || w == std::string(NAMES[d]) + "s") {
			*plural = w.back() == 's';
			return d;
		}
	}
	return -1;
}

int weekday_de(const std::string &w, bool *plural)
{
	static const char *const NAMES[8] = {
		"montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag", "sonntag",
		"sonnabend",
	};
	for (int d = 0; d < 8; d++) {
		if (w == NAMES[d] || w == std::string(NAMES[d]) + "s") {
			*plural = w.back() == 's' && w != NAMES[d];
			return d == 7 ? 5 : d;
		}
	}
	return -1;
}

// Weekdays: one ("on monday"), repeating ("every monday", "mondays",
// "monday and friday" after "every") or a range ("monday to friday")
void weekdays(P &p, When &w, int (*weekday)(const std::string &, bool *), const char *every,
	      const char *range)
{
	int mask = 0, count = 0, last = -1;
	bool repeat = false, plural;

	for (size_t i = 0; i < p.t.size(); i++) {
		int d = p.t[i].used || p.t[i].n >= 0 ? -1 : weekday(p.t[i].w, &plural);

		if (d < 0)
			continue;
		if (plural || p.is(i - 1, every))
			repeat = true;
		if (p.is(i - 1, every))
			p.use(i - 1, i);
		int to = p.is(i + 1, range) && i + 2 < p.t.size() && !p.t[i + 2].used &&
			 p.t[i + 2].n < 0 ? weekday(p.t[i + 2].w, &plural) : -1;
		if (to >= 0) {
			for (int k = d;; k = (k + 1) % 7) {
				mask |= 1 << k;
				if (k == to)
					break;
			}
			repeat = true;
			count += 2;
			p.use(i, i + 3);
			continue;
		}
		mask |= 1 << d;
		last = d;
		count++;
		p.use(i, i + 1);
	}
	if (repeat)
		w.repeat |= mask;
	else if (count == 1)
		w.day = DAY_NAMES[last];
	else if (count)
		w.fail = true;	// "monday and tuesday": once each, or every week?
}

// The hour before, for "quarter to": "one" is said for 12:45 as well
int hour_before(long hour)
{
	return hour == 1 ? 12 : (int)((hour + 23) % 24);
}

// A time written as one number: 730, 1930
bool clock_number(long n, When &w)
{
	if (n < 100)
		return false;
	w.hour = (int)(n / 100);
	w.minute = (int)(n % 100);
	return true;
}

void when_en(P &p, When &w)
{
	weekdays(p, w, weekday_en, "every|each", "to|through|thru|until|till");
	if (p.seq("every|each day") || p.take("everyday|daily"))
		w.repeat = 127;
	for (size_t i = 0; i < p.t.size(); i++)	// every morning
		if (p.is(i, "every|each") && p.is(i + 1, "morning|afternoon|evening|night")) {
			w.repeat = 127;
			p.use(i, i + 1);
		}
	if (p.take("weekdays|weekday"))
		w.repeat |= 31;
	if (p.take("weekends|weekend"))
		w.repeat |= 96;
	if (w.repeat)
		p.take("every|each");
	if (p.take("today"))
		w.day = "today";
	if (p.take("tonight")) {
		w.day = "today";
		w.part = 3;
	}
	if (p.take("tomorrow"))
		w.day = "tomorrow";

	if (p.seq("a m") || p.take("am"))
		w.part = 1;
	else if (p.seq("p m") || p.take("pm"))
		w.part = 2;
	else if (p.take("morning"))
		w.part = 1;
	else if (p.take("afternoon|evening"))
		w.part = 2;
	else if (p.take("night"))
		w.part = 3;

	// "quarter past seven", "ten to eight", "five minutes past nine"
	for (size_t i = 1; i < p.t.size() && w.hour < 0; i++) {
		if (!p.is(i, "past|after|to|til|till|before|of") || !p.num(i + 1))
			continue;
		long minutes = -1;
		size_t from = i - 1;

		if (p.is(i - 1, "half"))
			minutes = 30;
		else if (p.is(i - 1, "quarter"))
			minutes = 15;
		else if (p.num(i - 1))
			minutes = p.t[i - 1].n;
		else if (i >= 2 && p.is(i - 1, "minute|minutes") && p.num(i - 2))
			minutes = p.t[from = i - 2].n;
		if (minutes < 1 || minutes > 59)
			continue;
		long hour = p.t[i + 1].n;
		if (p.is(i, "past|after")) {
			w.hour = (int)hour;
			w.minute = (int)minutes;
		} else {
			w.hour = hour_before(hour);
			w.minute = (int)(60 - minutes);
		}
		p.use(from, i + (p.is(i + 2, "oclock") ? 3 : 2));
	}
	// "half seven"
	for (size_t i = 0; i < p.t.size() && w.hour < 0; i++)
		if (p.is(i, "half") && p.num(i + 1)) {
			w.hour = (int)p.t[i + 1].n;
			w.minute = 30;
			p.use(i, i + 2);
		}
	// "seven", "seven o'clock", "seven thirty", "seven oh five"
	long i = w.hour < 0 ? p.first_number() : -1;
	if (i >= 0) {
		size_t end = (size_t)i + 1;

		if (!clock_number(p.t[i].n, w)) {
			w.hour = (int)p.t[i].n;
			if (p.is(end, "oclock")) {
				end++;
			} else if (p.is(end, "oh|o") && p.num(end + 1) && p.t[end + 1].n < 10) {
				w.minute = (int)p.t[end + 1].n;
				end += 2;
			} else if (p.num(end) && p.t[end].n < 60) {
				w.minute = (int)p.t[end++].n;
			}
		}
		p.use((size_t)i, end);
	} else if (w.hour < 0 && p.take("noon|midday")) {
		w.hour = 12;
		w.part = -1;
	} else if (w.hour < 0 && p.take("midnight")) {
		w.hour = 0;
		w.part = -1;
	}
}

void when_de(P &p, When &w)
{
	weekdays(p, w, weekday_de, "jeden|jedem|jeder|immer", "bis");
	if (p.seq("jeden|jedem tag") || p.take("taglich"))
		w.repeat = 127;
	for (size_t i = 0; i < p.t.size(); i++)	// jeden Morgen
		if (p.is(i, "jeden|jedem") && p.is(i + 1, "morgen|vormittag|nachmittag|abend")) {
			w.repeat = 127;
			p.use(i, i + 1);
			if (p.is(i + 1, "morgen")) {
				w.part = 1;
				p.use(i + 1, i + 2);
			}
		}
	if (p.take("werktags|wochentags|werktagen|wochentagen") || p.seq("unter der woche"))
		w.repeat |= 31;
	if (p.take("wochenende|wochenends|wochenenden"))
		w.repeat |= 96;
	if (w.repeat)
		p.take("jeden|jedem|jedes|immer");
	if (p.take("heute"))
		w.day = "today";
	// "morgen" is tomorrow, but the morning after "am" and "heute"; "morgen
	// fruh" and "morgen morgen" are tomorrow morning
	for (size_t i = 0; i < p.t.size(); i++) {
		if (!p.is(i, "morgen"))
			continue;
		p.use(i, i + 1);
		if (i && in(p.t[i - 1].w, "am|heute")) {
			w.part = 1;
		} else {
			w.day = "tomorrow";
			if (p.is(i + 1, "fruh|morgen")) {
				w.part = 1;
				p.use(i + 1, i + 2);
			}
		}
	}
	if (p.take("morgens|fruh|vormittags|vormittag"))
		w.part = 1;
	else if (p.take("nachmittags|nachmittag|abends|abend"))
		w.part = 2;
	else if (p.take("nachts|nacht"))
		w.part = 3;

	// "viertel nach acht", "zehn vor acht", "funf vor halb sieben"
	for (size_t i = 1; i < p.t.size() && w.hour < 0; i++) {
		if (!p.is(i, "nach|vor"))
			continue;
		long minutes = -1;
		size_t from = i - 1;

		if (p.is(i - 1, "viertel"))
			minutes = 15;
		else if (p.num(i - 1))
			minutes = p.t[i - 1].n;
		else if (i >= 2 && p.is(i - 1, "minute|minuten") && p.num(i - 2))
			minutes = p.t[from = i - 2].n;
		if (minutes < 1 || minutes > 59)
			continue;
		bool half = p.is(i + 1, "halb");
		size_t at = i + (half ? 2 : 1);
		if (!p.num(at))
			continue;
		long said = p.t[at].n;
		long t = (half ? (said + 23) % 24 * 60 + 30 : said * 60) +
			 (p.t[i].w == "nach" ? minutes : -minutes);

		t = (t + 1440) % 1440;
		w.hour = (int)(t / 60);
		w.minute = (int)(t % 60);
		if (w.hour == 0 && said == 1)
			w.hour = 12;
		p.use(from, at + (p.is(at + 1, "uhr") ? 2 : 1));
	}
	// "halb sieben", "dreiviertel acht", "viertel acht" (6:30, 7:45, 7:15)
	for (size_t i = 0; i < p.t.size() && w.hour < 0; i++)
		if (p.is(i, "halb|viertel|dreiviertel") && p.num(i + 1)) {
			w.hour = hour_before(p.t[i + 1].n);
			w.minute = p.t[i].w == "halb" ? 30 : p.t[i].w == "viertel" ? 15 : 45;
			p.use(i, i + 2);
		}
	// "sieben", "sieben Uhr", "sieben Uhr dreissig", "ein Uhr"
	for (size_t i = 0; i < p.t.size() && w.hour < 0; i++) {
		bool one = p.is(i, "ein") && p.is(i + 1, "uhr");
		size_t end = i + 1;

		if (!one && !p.num(i))
			continue;
		if (one || !clock_number(p.t[i].n, w)) {
			w.hour = one ? 1 : (int)p.t[i].n;
			if (p.is(end, "uhr"))
				end++;
			if (p.num(end) && p.t[end].n < 60)
				w.minute = (int)p.t[end++].n;
		}
		p.use(i, end);
	}
	if (w.hour < 0 && p.take("mittag|mittags")) {
		w.hour = 12;
		w.part = -1;
	} else if (w.hour < 0 && p.take("mitternacht")) {
		w.hour = 0;
		w.part = -1;
	} else if (p.take("mittag|mittags")) {
		w.part = 4;
	}
}

// The time as hour, minute and h12 in the action. h12: the hour was said
// without the part of the day ("at seven"), so it can be either half.
bool put_time(const When &w, Action &a)
{
	int hour = w.hour;
	bool h12 = false;

	if (hour > 23 || w.minute > 59)
		return false;
	switch (w.part) {
	case 0:
		h12 = hour >= 1 && hour <= 12;
		break;
	case 1:
		hour = hour == 12 ? 0 : hour;
		break;
	case 2:
		hour = hour < 12 ? hour + 12 : hour;
		break;
	case 3:	// "eleven at night", but "two at night"
		hour = hour == 12 ? 0 : hour >= 5 && hour < 12 ? hour + 12 : hour;
		break;
	case 4:	// "ein Uhr mittags"
		hour = hour <= 5 ? hour + 12 : hour;
		break;
	}
	a.args["hour"] = std::to_string(hour);
	a.args["minute"] = std::to_string(w.minute);
	if (h12)
		a.args["h12"] = "1";
	return true;
}

std::string days_of(int mask)
{
	std::string s;

	for (int d = 0; d < 7; d++)
		if (mask & 1 << d)
			s += (s.empty() ? "" : ",") + std::string(DAY_NAMES[d]);
	return s;
}

// Subjects ---------------------------------------------------------------------

// The word lists of the language file ("words.filler = ..."), each as
// "a|b|c" without accents
struct Vocab {
	const char *filler;	// may be left over anywhere
	const char *negation;	// "don't ...": never matched
	const char *articles;	// between "for" and a timer's name
	const char *cancel, *all;
	const char *timer, *timer_list, *timer_filler, *name_intro;
	const char *alarm, *alarm_list, *alarm_filler;
	const char *volume, *volume_up, *volume_down, *volume_get, *percent, *volume_filler;
	const char *mute, *mute_filler;
	const char *time_filler, *date_filler;
	const char *stop, *stop_filler;
	const char *previous, *next, *pause, *resume, *media_filler;
};

const struct {
	const char *key;
	const char *Vocab::*list;
} LISTS[] = {
	{ "filler", &Vocab::filler }, { "negation", &Vocab::negation },
	{ "articles", &Vocab::articles }, { "cancel", &Vocab::cancel }, { "all", &Vocab::all },
	{ "timer", &Vocab::timer }, { "timer_list", &Vocab::timer_list },
	{ "timer_filler", &Vocab::timer_filler }, { "name_intro", &Vocab::name_intro },
	{ "alarm", &Vocab::alarm }, { "alarm_list", &Vocab::alarm_list },
	{ "alarm_filler", &Vocab::alarm_filler }, { "volume", &Vocab::volume },
	{ "volume_up", &Vocab::volume_up }, { "volume_down", &Vocab::volume_down },
	{ "volume_get", &Vocab::volume_get }, { "percent", &Vocab::percent },
	{ "volume_filler", &Vocab::volume_filler }, { "mute", &Vocab::mute },
	{ "mute_filler", &Vocab::mute_filler }, { "time_filler", &Vocab::time_filler },
	{ "date_filler", &Vocab::date_filler }, { "stop", &Vocab::stop },
	{ "stop_filler", &Vocab::stop_filler }, { "previous", &Vocab::previous },
	{ "next", &Vocab::next }, { "pause", &Vocab::pause }, { "resume", &Vocab::resume },
	{ "media_filler", &Vocab::media_filler },
};
const size_t LIST_COUNT = sizeof(LISTS) / sizeof(LISTS[0]);

Vocab vocab;
std::string vocab_text[LIST_COUNT];

// "turn the alarm off", "stell den Wecker ab": the verb and its particle
// stand apart
bool cancels(Lang lang, const Vocab &v, P &p)
{
	const char *verb = lang == EN ? "turn|switch" : "stell|stelle|schalt|schalte";
	const char *particle = lang == EN ? "off" : "ab|aus";
	bool cancel = p.take(v.cancel);

	if (p.has(verb) && p.has(particle)) {
		p.take(verb);
		p.take(particle);
		cancel = true;
	}
	return cancel;
}

bool timer(Lang lang, const Vocab &v, P &p, Action &a)
{
	size_t keyword = 0, at;

	while (keyword < p.t.size() && !p.is(keyword, v.timer))
		keyword++;
	p.take(v.timer);
	long seconds = duration(lang, p, &at);
	bool list = p.take(v.timer_list);
	bool cancel = cancels(lang, v, p);
	bool all = cancel && p.take(v.all);

	// What is left over is the timer's name if it stands where a name
	// does: "pizza timer", "timer for the pizza", "timer called pizza"
	std::string name;
	size_t first = 0, count = 0;
	for (size_t i = 0; i < p.t.size(); i++) {
		const Tok &tok = p.t[i];

		if (tok.used || (tok.n < 0 && (in(tok.w, v.filler) || in(tok.w, v.timer_filler))))
			continue;
		if (tok.n >= 0 || (count && i != first + count) || count == 2)
			return false;
		if (!count++)
			first = i;
		name += (name.empty() ? "" : " ") + tok.orig;
	}
	if (count && first + count != keyword) {
		size_t intro = first;

		while (intro && in(p.t[intro - 1].w, v.articles))
			intro--;
		if (!intro || !in(p.t[intro - 1].w, v.name_intro))
			return false;
	}

	if (cancel) {
		a.name = "timer_cancel";
		if (all)
			a.args["all"] = "1";
	} else if (seconds >= 0) {
		a.name = "timer_set";
		a.args["seconds"] = std::to_string(seconds);
	} else if (list) {
		a.name = "timer_list";
	} else {
		return false;
	}
	if (!name.empty() && !all)
		a.args["name"] = name;
	return true;
}

bool alarm(Lang lang, const Vocab &v, P &p, Action &a)
{
	size_t at;
	When w;

	p.take(v.alarm);
	// "wake me in twenty minutes" is a timer; "five minutes past nine" is
	// no duration
	P q = p;
	long seconds = duration(lang, q, &at);
	if (seconds >= 0 && at && q.t[at - 1].w == "in") {
		p = q;
		a.name = "timer_set";
		a.args["seconds"] = std::to_string(seconds);
		return p.rest(v.filler, v.alarm_filler);
	}
	bool cancel = cancels(lang, v, p);
	if (lang == EN)
		when_en(p, w);
	else
		when_de(p, w);
	bool all = cancel && p.take(v.all);
	bool list = p.take(v.alarm_list);

	if (w.fail)
		return false;
	if (cancel) {
		a.name = "alarm_cancel";
		if (all)
			a.args["all"] = "1";
		else if (w.hour >= 0 && !put_time(w, a))
			return false;
	} else if (w.hour >= 0) {
		a.name = "alarm_set";
		if (!put_time(w, a))
			return false;
		if (w.repeat)
			a.args["repeat"] = days_of(w.repeat);
		else if (!w.day.empty())
			a.args["day"] = w.day;
	} else if (list) {
		a.name = "alarm_list";
	} else {
		return false;
	}
	return p.rest(v.filler, v.alarm_filler);
}

bool volume(Lang lang, const Vocab &v, P &p, Action &a)
{
	long i = p.first_number();

	if (i >= 0) {	// "volume five", "volume to fifty percent"
		long n = p.t[i].n;

		if (p.t[i].half)
			return false;
		p.use((size_t)i, (size_t)i + 1);
		if (p.is((size_t)i + 1, v.percent)) {
			p.use((size_t)i + 1, (size_t)i + 2);
			if (n > 100)
				return false;
			n = (n + 5) / 10;
		}
		if (n > 10)
			return false;
		p.take(v.volume_up);
		p.take(v.volume_down);
		a.name = "volume_set";
		a.args["level"] = std::to_string(n);
		return p.rest(v.filler, v.volume_filler);
	}
	bool up = p.take(v.volume_up), down = p.take(v.volume_down);
	if (!up && !down && p.has(lang == EN ? "loud" : "laut"))	// "too loud", "zu laut"
		down = p.take(lang == EN ? "too" : "zu");
	if (up != down) {
		a.name = "volume_change";
		a.args["direction"] = up ? "up" : "down";
	} else if (!up && p.take(v.volume_get)) {
		a.name = "volume_get";
	} else {
		return false;
	}
	return p.rest(v.filler, v.volume_filler);
}

bool mute(Lang lang, const Vocab &v, P &p, Action &a)
{
	bool on;

	if (lang == EN)
		on = !p.has("unmute|unmuted") && !p.take("off");
	else if (p.has("ton"))	// "Ton aus", "Ton an"
		on = p.take("aus|ab") || !p.take("an|ein");
	else
		on = !p.take("aus|aufheben|beenden|deaktivieren|ausschalten");
	a.name = "volume_mute";
	a.args["mute"] = on ? "on" : "off";
	return p.rest(v.filler, v.mute_filler);
}

// Nothing but "stop", "shut up", "das reicht"
bool stop(const Vocab &v, const P &p, Action &a)
{
	bool said = false;

	for (const Tok &tok : p.t) {
		if (tok.n >= 0)
			return false;
		if (in(tok.w, v.stop))
			said = true;
		else if (!in(tok.w, v.filler) && !in(tok.w, v.stop_filler))
			return false;
	}
	a.name = "stop";
	return said;
}

bool media(Lang lang, const Vocab &v, P &p, Action &a)
{
	const char *command = p.has(v.previous) ? "previous" : p.has(v.next) ? "next" :
			      p.has(v.pause) ? "pause" : p.has(v.resume) ? "resume" : nullptr;

	// "Musik an": "an" alone says too little to count as a command
	if (!command && lang == DE && p.has("musik|wiedergabe|lied") && p.take("an"))
		command = "resume";
	if (!command)
		return false;
	p.take(v.previous);
	p.take(v.next);
	p.take(v.pause);
	p.take(v.resume);
	a.name = "media";
	a.args["command"] = command;
	return p.rest(v.filler, v.media_filler);
}

bool match(Lang lang, const Vocab &v, P &p, Action &a)
{
	// "how much time is left", "wie lange noch"
	bool left = lang == EN ? p.has("left|remaining") && p.has("how") :
		    p.has("ubrig|restzeit") || (p.has("wie") && p.has("lange") && p.has("noch"));
	if (p.has(v.timer) || left)
		return timer(lang, v, p, a);
	if (p.has(v.alarm))
		return alarm(lang, v, p, a);
	if (p.has(v.volume) || (lang == EN && p.has("turn|crank") && p.has("up|down")))
		return volume(lang, v, p, a);
	if (p.has(v.mute) || (lang == DE && p.has("ton") && p.has("aus|ab|an|ein")))
		return mute(lang, v, p, a);

	bool time, date;
	if (lang == EN) {
		time = p.has("time") && p.has("what|whats|tell|current|got|know|have");
		date = (p.has("date|day") && p.has("what|whats|which|tell|todays")) ||
		       (p.has("today") && p.has("what|whats"));
	} else {
		time = p.has("uhrzeit") || (p.has("spat") && p.has("wie")) ||
		       (p.has("uhr") && p.has("wieviel|viel"));
		date = p.has("datum|wochentag|wievielte|wievielten") ||
		       (p.has("tag") && p.has("welcher|welchen|was"));
	}
	if (time) {
		a.name = "time_get";
		return p.rest(v.filler, v.time_filler);
	}
	if (date) {
		a.name = "date_get";
		return p.rest(v.filler, v.date_filler);
	}
	if (stop(v, p, a))
		return true;
	return media(lang, v, p, a);
}

} // namespace

Action parse(Lang lang, const std::string &utterance)
{
	const Vocab &v = vocab;
	Action a;
	P p;

	p.t = tokenize(utterance);
	if (lang == EN) {
		numbers_en(p.t);
	} else {
		// "Pizzatimer" is a timer named "pizza"
		std::vector<Tok> split;
		for (const Tok &tok : p.t) {
			size_t l = tok.w.size();

			if (l > 5 && !tok.w.compare(l - 5, 5, "timer") && tok.w != "kurzzeittimer") {
				Tok name;
				name.w = tok.w.substr(0, l - 5);
				name.orig = tok.orig.substr(0, tok.orig.size() - 5);
				split.push_back(name);
				name.w = name.orig = "timer";
				split.push_back(name);
			} else {
				split.push_back(tok);
			}
		}
		p.t = split;
		numbers_de(p.t);
	}
	if (p.t.empty() || p.has(v.negation) || !match(lang, v, p, a)) {
		a = Action();
		a.name = "unmatched";
	}
	return a;
}

bool set_vocabulary(const std::map<std::string, std::string> &file, std::string *error)
{
	for (size_t i = 0; i < LIST_COUNT; i++) {
		auto entry = file.find(std::string("words.") + LISTS[i].key);

		if (entry == file.end()) {
			*error = std::string("words.") + LISTS[i].key + " is missing";
			return false;
		}
		vocab_text[i].clear();
		for (const Tok &tok : tokenize(entry->second))
			vocab_text[i] += (vocab_text[i].empty() ? "" : "|") + tok.w;
		vocab.*LISTS[i].list = vocab_text[i].c_str();
	}
	return true;
}
