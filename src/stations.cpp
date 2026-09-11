#include "stations.h"

#include <ctype.h>
#include <string.h>

#include "stations_data.h"

namespace {
constexpr size_t kFoldedMax = 48;

/** Fold case and punctuation so "st albans" finds "St. Albans City". */
size_t fold(const char *value, char *out, size_t size) {
  size_t length = 0;
  bool pendingSpace = false;
  for (const char *c = value; *c && length + 1 < size; ++c) {
    const unsigned char character = static_cast<unsigned char>(*c);
    if (isalnum(character)) {
      if (pendingSpace && length > 0) out[length++] = ' ';
      pendingSpace = false;
      if (length + 1 < size) out[length++] = static_cast<char>(tolower(character));
    } else {
      pendingSpace = true;
    }
  }
  out[length] = '\0';
  return length;
}

int compareCrs(const StationRecord &record, const char *crs) {
  for (int index = 0; index < 3; ++index) {
    const int typed = toupper(static_cast<unsigned char>(crs[index]));
    if (record.crs[index] != typed) return record.crs[index] < typed ? -1 : 1;
  }
  return 0;
}

/** Does a folded name have a WORD starting with the needle? */
bool wordStartsWith(const char *folded, const char *needle, size_t needleLength) {
  for (const char *at = strchr(folded, ' '); at; at = strchr(at + 1, ' ')) {
    if (strncmp(at + 1, needle, needleLength) == 0) return true;
  }
  return false;
}
}  // namespace

size_t stationCount() { return kStationCount; }

const char *stationNameAt(size_t index) {
  if (index >= kStationCount) return nullptr;
  return kStationNames + kStations[index].nameOffset;
}

void stationCrsAt(size_t index, char *crsOut) {
  if (index >= kStationCount) {
    crsOut[0] = '\0';
    return;
  }
  memcpy(crsOut, kStations[index].crs, 3);
  crsOut[3] = '\0';
}

const char *stationName(const char *crs) {
  if (!crs || strlen(crs) != 3) return nullptr;
  // The table is sorted by code, so this is a bisection, not a scan.
  size_t low = 0;
  size_t high = kStationCount;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = compareCrs(kStations[middle], crs);
    if (order == 0) return kStationNames + kStations[middle].nameOffset;
    if (order < 0) low = middle + 1;
    else high = middle;
  }
  return nullptr;
}

const char *stationWithCrsPrefix(const char *prefix, char *crsOut) {
  const size_t length = prefix ? strlen(prefix) : 0;
  if (length == 0 || length > 3) return nullptr;
  for (size_t index = 0; index < kStationCount; ++index) {
    bool matches = true;
    for (size_t letter = 0; letter < length; ++letter) {
      const int typed = toupper(static_cast<unsigned char>(prefix[letter]));
      matches = matches && kStations[index].crs[letter] == typed;
    }
    if (matches) {
      stationCrsAt(index, crsOut);
      return kStationNames + kStations[index].nameOffset;
    }
  }
  return nullptr;
}

size_t searchStations(const char *query, uint16_t *results, size_t limit) {
  char needle[kFoldedMax];
  const size_t needleLength = fold(query ? query : "", needle, sizeof(needle));
  if (needleLength == 0 || limit == 0) return 0;

  // Five passes rather than five buffers: the table is small and
  // flash-resident, and a fixed rank order costs nothing to walk again. The
  // exact-name pass is what the web board gets from its alphabetical list:
  // "st albans" should be St Albans, not St Albans Abbey.
  size_t found = 0;
  for (int rank = 0; rank < 5 && found < limit; ++rank) {
    for (size_t index = 0; index < kStationCount && found < limit; ++index) {
      const StationRecord &record = kStations[index];
      char folded[kFoldedMax];
      fold(kStationNames + record.nameOffset, folded, sizeof(folded));

      bool hit = false;
      switch (rank) {
        case 0:
          hit = needleLength == 3 && compareCrs(record, needle) == 0;
          break;
        case 1:
          hit = strcmp(folded, needle) == 0;
          break;
        case 2:
          hit = strncmp(folded, needle, needleLength) == 0;
          break;
        case 3:
          hit = wordStartsWith(folded, needle, needleLength);
          break;
        default:
          hit = strstr(folded, needle) != nullptr;
          break;
      }
      if (!hit) continue;

      bool already = false;
      for (size_t seen = 0; seen < found; ++seen) {
        already = already || results[seen] == index;
      }
      if (!already) results[found++] = static_cast<uint16_t>(index);
    }
  }
  return found;
}

bool crsPrefixExists(const char *prefix, size_t length) {
  if (length == 0) return kStationCount > 0;
  if (length > 3) return false;
  // Spinning a letter asks this up to 26 times and repairing asks it 78 times,
  // all while a finger is held down, so it bisects the sorted table rather than
  // walking it.
  size_t low = 0;
  size_t high = kStationCount;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    int order = 0;
    for (size_t letter = 0; letter < length && order == 0; ++letter) {
      const int typed = toupper(static_cast<unsigned char>(prefix[letter]));
      if (kStations[middle].crs[letter] != typed) {
        order = kStations[middle].crs[letter] < typed ? -1 : 1;
      }
    }
    if (order == 0) return true;
    if (order < 0) low = middle + 1;
    else high = middle;
  }
  return false;
}

char nextReachableLetter(const char *crs, size_t position, int direction) {
  if (position > 2) return crs[position];
  char candidate[4];
  memcpy(candidate, crs, 3);
  candidate[3] = '\0';
  const int step = direction < 0 ? -1 : 1;
  int letter = candidate[position] - 'A';
  for (int tried = 0; tried < 26; ++tried) {
    letter = (letter + step + 26) % 26;
    candidate[position] = static_cast<char>('A' + letter);
    if (crsPrefixExists(candidate, position + 1)) return candidate[position];
  }
  return crs[position];
}

void repairCrs(char *crs) {
  for (size_t position = 0; position < 3; ++position) {
    if (crsPrefixExists(crs, position + 1)) continue;
    // Keep the letters to the left and take the first continuation from the
    // one already here, so a spin moves as little of the code as it can.
    const char kept = crs[position];
    bool found = false;
    for (int tried = 0; tried < 26 && !found; ++tried) {
      crs[position] = static_cast<char>('A' + (kept - 'A' + tried) % 26);
      found = crsPrefixExists(crs, position + 1);
    }
    if (!found) {
      // Only reachable if the letters to the left continue nowhere, which
      // repairing them in order has already ruled out.
      memcpy(crs, kStations[0].crs, 3);
      crs[3] = '\0';
      return;
    }
  }
}
