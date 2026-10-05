/*
 * tz_names.h - the rule for a time zone the grid knows only by name (D100).
 *
 * The grid's zone is two things (D67): the name an admin chose ("America/Chicago") and the POSIX
 * rule handhelds turn UTC into local time with ("CST6CDT,M3.2.0,M11.1.0"). The rule is made by the
 * admin page's browser, which has the world's zone data; an AP has none. But a grid can hold a name
 * and no rule - settings saved before the rule existed, carried forward - and a grid that takes its
 * time from a GPS never sends anyone back to the admin page to save it again. Its handhelds then
 * show UTC for ever. This short table closes that: the common zones, name to rule.
 *
 * It is a fallback and nothing more. A rule the admin page saved always wins, a name not listed
 * here stays unknown, and nothing here is written to the settings.
 */
#pragma once

/* The POSIX rule for an IANA zone name, or NULL when the name is not in the table. */
const char *tz_rule_for_name(const char *name);
