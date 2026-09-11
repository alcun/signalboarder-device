#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * The station list, resident in flash, shared by both ways of choosing one.
 *
 * The panel names a code as it is typed; the setup portal searches by name for
 * boards with no touch panel. Both read THIS table, generated from the web
 * board's own `public/stations.json` by `scripts/generate_stations.py`, so a
 * code the panel accepts is a code the server can answer for.
 *
 * The ranking below is deliberately the same as `web/src/scripts/stations.ts`:
 * an exact code first, then names starting with the query, then names with a
 * WORD starting with it, then anything containing it.
 */

/** Total stations in the table. */
size_t stationCount();

/** Name for a code, or nullptr when the provider does not know it. */
const char *stationName(const char *crs);

inline bool isKnownCrs(const char *crs) { return stationName(crs) != nullptr; }

/**
 * First station whose code starts with one or two typed letters, so the panel
 * can show where a half-typed code is heading. Writes the code into `crsOut`,
 * which needs four bytes. Returns its name, or nullptr for no match.
 */
const char *stationWithCrsPrefix(const char *prefix, char *crsOut);

/** Name and code at a table index, for reading search results. */
const char *stationNameAt(size_t index);
void stationCrsAt(size_t index, char *crsOut);

/**
 * Best matches for a name or code fragment, most useful first. Fills `results`
 * with table indices and returns how many were written.
 */
size_t searchStations(const char *query, uint16_t *results, size_t limit);

/** Is there any station whose code starts with these letters? */
bool crsPrefixExists(const char *prefix, size_t length);

/**
 * The next letter for one position that keeps the code reachable.
 *
 * Spinning a letter should only ever offer letters some station actually
 * continues from, which is what stops the panel arriving at three letters that
 * name nothing. Wraps around, and returns the letter already there when no
 * other one works.
 */
char nextReachableLetter(const char *crs, size_t position, int direction);

/**
 * Pull a three-letter code onto a real station, keeping every letter it can.
 *
 * Used after a letter changes: `NBN` with its first letter spun to `O` keeps
 * `B` and `N` if some `OBN` exists, and otherwise takes the first continuation
 * that does. Codes handed to it are always three letters and always leave as a
 * station.
 */
void repairCrs(char *crs);
