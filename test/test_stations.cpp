#include "stations.h"

#include <iostream>
#include <string.h>

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool firstResultIs(const char *query, const char *crs) {
  uint16_t results[10];
  const size_t found = searchStations(query, results, 10);
  if (found == 0) return false;
  char code[4] = "";
  stationCrsAt(results[0], code);
  return strcmp(code, crs) == 0;
}
}  // namespace

int main() {
  expect(stationCount() > 2000, "the table holds the national list");

  const char *newBrighton = stationName("NBN");
  expect(newBrighton != nullptr && strcmp(newBrighton, "New Brighton") == 0,
         "a known code names its station");
  expect(stationName("nbn") != nullptr, "lookup accepts lower case");
  expect(stationName("ZZZ") == nullptr, "an unknown code names nothing");
  expect(stationName("NB") == nullptr, "a partial code is not a code");
  expect(isKnownCrs("GLC"), "Glasgow Central is known");

  char code[4] = "";
  const char *prefixed = stationWithCrsPrefix("NB", code);
  expect(prefixed != nullptr && code[0] == 'N' && code[1] == 'B',
         "a half-typed code finds where it is heading");
  expect(stationWithCrsPrefix("ZZ", code) == nullptr,
         "a half-typed code with no station finds nothing");

  // Same ranking as the web board: exact code, then starts-with, then a word
  // starting with it, then contains.
  expect(firstResultIs("GLC", "GLC"), "an exact code wins");
  expect(firstResultIs("glasgow central", "GLC"), "a full name finds its code");
  expect(firstResultIs("st albans", "SAC"), "punctuation and case fold away");
  expect(firstResultIs("new brighton", "NBN"), "New Brighton is searchable");

  uint16_t results[10];
  expect(searchStations("", results, 10) == 0, "an empty query matches nothing");
  expect(searchStations("liverpool", results, 10) > 1,
         "a shared name returns several");
  expect(searchStations("liverpool", results, 3) <= 3, "the limit is honoured");
  expect(searchStations("zzzzzz", results, 10) == 0, "nonsense matches nothing");

  // Spinning a letter must never arrive at three letters naming nothing.
  expect(crsPrefixExists("N", 1), "N starts some station");
  expect(crsPrefixExists("NBN", 3), "a whole code is its own prefix");
  expect(!crsPrefixExists("QZ", 2), "QZ starts nothing");

  char spun[4] = "NBN";
  const char up = nextReachableLetter(spun, 0, 1);
  spun[0] = up;
  expect(crsPrefixExists(spun, 1), "spinning up lands on a reachable letter");
  spun[0] = nextReachableLetter(spun, 0, -1);
  expect(strcmp(spun, "NBN") == 0, "spinning back returns to where it started");

  char repaired[4] = "QZQ";
  repairCrs(repaired);
  expect(isKnownCrs(repaired), "an impossible code repairs to a real station");

  char kept[4] = "NBN";
  repairCrs(kept);
  expect(strcmp(kept, "NBN") == 0, "a real code is left alone");

  // Every letter a spin can reach, at every position, still names a station
  // once the letters after it are repaired.
  char walk[4] = "AAA";
  repairCrs(walk);
  for (size_t position = 0; position < 3; ++position) {
    for (int step = 0; step < 26; ++step) {
      walk[position] = nextReachableLetter(walk, position, 1);
      repairCrs(walk);
      expect(isKnownCrs(walk), "every spin leaves a real station");
    }
  }

  if (failures == 0) std::cout << "stations: all checks passed\n";
  return failures == 0 ? 0 : 1;
}
