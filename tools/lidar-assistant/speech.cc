// SPDX-License-Identifier: GPL-2.0-only
// lidar-assistant: the spoken answers. An answer is a row of parts: fixed
// phrases, weekday and month names (all from the language file, "say.*"),
// number words (rules below, English and German), and names the user gave
// (timers). Everything but the names is listed by list_phrases(), so it can
// be synthesised ahead and played from a cache.
#include "assistant.h"

#include <cerrno>
#include <cstring>
#include <set>

// The language file's keys ("say.volume"), in the order of enum Phrase,
// then the weekdays from Monday and the months
static const char *const KEYS[] = {
	"volume", "louder", "quieter", "muted", "unmuted", "no_volume",
	"timer_set", "timer_set_end", "timer", "timer_cancelled",
	"timers_cancelled", "no_timers", "no_such_timer", "several_timers",
	"left", "left_end", "too_many",
	"alarm_set", "alarm_cancelled", "alarms_cancelled", "no_alarms",
	"no_such_alarm", "several_alarms", "alarm_at", "time_past", "no_clock",
	"hour", "hours", "minute", "minutes", "second", "seconds",
	"time", "date", "date_the",
	"today", "tomorrow", "on_day", "every", "every_day", "weekdays",
	"weekends", "and", "oh", "am", "pm", "uhr",
	"paused", "resumed", "next", "previous", "failed", "bad_args",
	"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
	"january", "february", "march", "april", "may", "june", "july", "august",
	"september", "october", "november", "december",
};
enum { WEEKDAY = P_COUNT, MONTH = WEEKDAY + 7, TEXTS = MONTH + 12 };
static_assert(sizeof(KEYS) / sizeof(KEYS[0]) == TEXTS, "KEYS does not follow enum Phrase");

static std::string texts[TEXTS];

static std::string trim(const std::string &s)
{
	size_t from = s.find_first_not_of(" \t\r\n"), to = s.find_last_not_of(" \t\r\n");

	return from == std::string::npos ? "" : s.substr(from, to - from + 1);
}

bool load_language(const std::string &path, std::string *error)
{
	std::map<std::string, std::string> file;
	FILE *f = fopen(path.c_str(), "r");
	char line[1024];

	if (!f) {
		*error = path + ": " + strerror(errno);
		return false;
	}
	for (int n = 1; fgets(line, sizeof(line), f); n++) {
		std::string l = trim(line);
		size_t eq = l.find('=');

		if (l.empty() || l[0] == '#')
			continue;
		if (eq == std::string::npos) {
			*error = path + ":" + std::to_string(n) + ": no \"key = text\"";
			fclose(f);
			return false;
		}
		std::string key = trim(l.substr(0, eq)), text = trim(l.substr(eq + 1));
		if (!key.compare(0, 6, "words.") && file.count(key))
			file[key] += " " + text;	// word lists add up
		else
			file[key] = text;
	}
	fclose(f);
	for (int i = 0; i < TEXTS; i++) {
		auto entry = file.find(std::string("say.") + KEYS[i]);

		if (entry == file.end()) {
			*error = path + ": say." + KEYS[i] + " is missing";
			return false;
		}
		texts[i] = entry->second;
	}
	if (!set_vocabulary(file, error)) {
		*error = path + ": " + *error;
		return false;
	}
	return true;
}

// Number words 0-99; larger numbers do not occur (hours up to 24, minutes
// and seconds up to 59, days up to 31) and would come out as digits.
static const char *const EN_ONES[20] = {
	"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
	"eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
	"eighteen", "nineteen",
};
static const char *const EN_TENS[10] = {
	"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
};
static const char *const EN_ORDINALS[20] = {
	"zeroth", "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth",
	"ninth", "tenth", "eleventh", "twelfth", "thirteenth", "fourteenth", "fifteenth",
	"sixteenth", "seventeenth", "eighteenth", "nineteenth",
};
static const char *const DE_ONES[20] = {
	"null", "eins", "zwei", "drei", "vier", "fünf", "sechs", "sieben", "acht", "neun", "zehn",
	"elf", "zwölf", "dreizehn", "vierzehn", "fünfzehn", "sechzehn", "siebzehn", "achtzehn",
	"neunzehn",
};
static const char *const DE_TENS[10] = {
	"", "", "zwanzig", "dreißig", "vierzig", "fünfzig", "sechzig", "siebzig", "achtzig",
	"neunzig",
};
static const char *const DE_ORDINALS[13] = {
	"nullte", "erste", "zweite", "dritte", "vierte", "fünfte", "sechste", "siebte", "achte",
	"neunte", "zehnte", "elfte", "zwölfte",
};

// German "one" before a noun: "ein Uhr", "eine Minute"
enum One { EINS, EIN, EINE };

static std::string cardinal(Lang lang, int n, One one = EINS)
{
	if (n < 0 || n > 99)
		return std::to_string(n);
	if (lang == EN) {
		if (n < 20)
			return EN_ONES[n];
		return std::string(EN_TENS[n / 10]) + (n % 10 ? std::string("-") + EN_ONES[n % 10] : "");
	}
	if (n == 1)
		return one == EINS ? "eins" : one == EIN ? "ein" : "eine";
	if (n < 20)
		return DE_ONES[n];
	if (n % 10 == 0)
		return DE_TENS[n / 10];
	return std::string(n % 10 == 1 ? "ein" : DE_ONES[n % 10]) + "und" + DE_TENS[n / 10];
}

static std::string ordinal(Lang lang, int n)
{
	if (n < 0 || n > 99)
		return std::to_string(n);
	if (lang == EN) {
		if (n < 20)
			return EN_ORDINALS[n];
		if (n % 10 == 0) {	// twenty -> twentieth
			std::string t = EN_TENS[n / 10];
			return t.substr(0, t.size() - 1) + "ieth";
		}
		return std::string(EN_TENS[n / 10]) + "-" + EN_ORDINALS[n % 10];
	}
	if (n < 13)
		return DE_ORDINALS[n];
	return cardinal(DE, n) + (n < 20 ? "te" : "ste");
}

Reply &Reply::word(const std::string &w)
{
	if (!w.empty())
		parts.push_back(w);
	return *this;
}

Reply &Reply::p(Phrase phrase) { return word(texts[phrase]); }
Reply &Reply::number(int n) { return word(cardinal(lang, n)); }
Reply &Reply::weekday(int day) { return word(texts[WEEKDAY + day % 7]); }
Reply &Reply::end() { return word("."); }

Reply &Reply::duration(long seconds)
{
	static const Phrase UNITS[3][2] = {
		{ P_HOUR, P_HOURS }, { P_MINUTE, P_MINUTES }, { P_SECOND, P_SECONDS },
	};
	long v[3] = { seconds / 3600, seconds / 60 % 60, seconds % 60 };
	int left = (v[0] != 0) + (v[1] != 0) + (v[2] != 0);

	if (!left)	// "zero seconds"
		left = 1, v[2] = 0;
	for (int i = 0; i < 3; i++) {
		if (!v[i] && !(i == 2 && seconds <= 0))
			continue;
		word(cardinal(lang, (int)v[i], EINE));
		p(UNITS[i][v[i] != 1]);
		if (--left == 1)
			p(P_AND);
	}
	return *this;
}

Reply &Reply::clock(int hour, int minute)
{
	if (lang == DE) {
		word(cardinal(DE, hour, EIN));
		p(P_UHR);
		if (minute)
			number(minute);
		return *this;
	}
	number(hour % 12 ? hour % 12 : 12);
	if (minute && minute < 10)
		p(P_OH);
	if (minute)
		number(minute);
	return p(hour < 12 ? P_AM : P_PM);
}

Reply &Reply::date(int wday, int day, int month)
{
	p(P_DATE);
	weekday(wday);
	if (lang == EN)
		return word(texts[MONTH + month % 12]).word(ordinal(EN, day));
	return p(P_DATE_THE).word(ordinal(DE, day)).word(texts[MONTH + month % 12]);
}

// Parts joined by spaces; a sentence starts with a capital and ends with "."
std::string Reply::text() const
{
	std::string s;
	bool start = true;

	for (const std::string &part : parts) {
		if (part == ".") {
			if (!start)
				s += ".";
			start = true;
			continue;
		}
		if (!s.empty())
			s += " ";
		s += part;
		if (start && part[0] >= 'a' && part[0] <= 'z')
			s[s.size() - part.size()] = (char)(part[0] - 'a' + 'A');
		start = false;
	}
	return s;
}

void list_phrases(Lang lang, FILE *out)
{
	std::set<std::string> seen;
	auto put = [&](const std::string &s) {
		if (!s.empty() && seen.insert(s).second)
			fprintf(out, "%s\n", s.c_str());
	};

	for (const std::string &text : texts)
		put(text);
	// volume levels, hours, minutes and seconds, with the German forms of
	// "one" before a noun; days of the month
	for (int i = 0; i < 60; i++)
		put(cardinal(lang, i));
	put(cardinal(lang, 1, EIN));
	put(cardinal(lang, 1, EINE));
	for (int i = 1; i <= 31; i++)
		put(ordinal(lang, i));
}
