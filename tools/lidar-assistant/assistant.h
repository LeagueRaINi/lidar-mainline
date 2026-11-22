// SPDX-License-Identifier: GPL-2.0-only
// lidar-assistant: what the grammar (grammar.cc), the spoken answers
// (speech.cc) and the daemon with its actions (lidar-assistant.cc) share.
#pragma once
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// The rules a language needs in code: number words, durations, times of day
// and weekdays (grammar.cc), number words and the form of a time or date in
// answers (speech.cc). Its wording is in the language file.
enum Lang { EN, DE };

// The language file (lang/en.txt): the answers' phrases ("say.*") and the
// grammar's word lists ("words.*"). One language is loaded at a time.
bool load_language(const std::string &path, std::string *error);
bool set_vocabulary(const std::map<std::string, std::string> &file, std::string *error);

// One command: an action of the table in lidar-assistant.cc and its
// arguments, all as text ("seconds" -> "720"). "unmatched" is what the
// grammar does not understand.
struct Action {
	std::string name;
	std::map<std::string, std::string> args;
	std::string str() const;	// "timer_set name=pizza seconds=720"
};

// The local grammar: one utterance as the speech recogniser writes it
Action parse(Lang lang, const std::string &utterance);

// Weekdays as the actions name them, Monday first
extern const char *const DAY_NAMES[7];

// Fixed phrases of the answers: "say.<name>" in the language file, in this
// order in speech.cc's KEYS
enum Phrase {
	P_VOLUME, P_LOUDER, P_QUIETER, P_MUTED, P_UNMUTED, P_NO_VOLUME,
	P_TIMER_SET, P_TIMER_SET_END, P_TIMER, P_TIMER_CANCELLED,
	P_TIMERS_CANCELLED, P_NO_TIMERS, P_NO_SUCH_TIMER, P_SEVERAL_TIMERS,
	P_LEFT, P_LEFT_END, P_TOO_MANY,
	P_ALARM_SET, P_ALARM_CANCELLED, P_ALARMS_CANCELLED, P_NO_ALARMS,
	P_NO_SUCH_ALARM, P_SEVERAL_ALARMS, P_ALARM_AT, P_TIME_PAST, P_NO_CLOCK,
	P_HOUR, P_HOURS, P_MINUTE, P_MINUTES, P_SECOND, P_SECONDS,
	P_TIME, P_DATE, P_DATE_THE,
	P_TODAY, P_TOMORROW, P_ON_DAY, P_EVERY, P_EVERY_DAY, P_WEEKDAYS,
	P_WEEKENDS, P_AND, P_OH, P_AM, P_PM, P_UHR,
	P_PAUSED, P_RESUMED, P_NEXT, P_PREVIOUS, P_FAILED, P_BAD_ARGS,
	P_COUNT
};

// A spoken answer, built from fixed phrases, number words and names so the
// text-to-speech cache can hold every part but the names. "." ends a
// sentence.
struct Reply {
	Lang lang = EN;
	std::vector<std::string> parts;

	Reply &p(Phrase phrase);
	Reply &word(const std::string &w);	// as given (a timer's name)
	Reply &number(int n);
	Reply &weekday(int day);		// 0 Monday
	Reply &duration(long seconds);		// "one hour and five minutes"
	Reply &clock(int hour, int minute);	// "seven thirty AM", "sieben Uhr dreißig"
	Reply &date(int weekday, int day, int month);	// month 0-11
	Reply &end();				// end of the sentence
	std::string text() const;
};

// Every fixed phrase and number word a Reply can hold, one per line
void list_phrases(Lang lang, FILE *out);
