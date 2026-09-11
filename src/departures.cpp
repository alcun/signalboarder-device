#include "departures.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace {
void copyUpper(char *destination, size_t size, const char *source) {
  if (size == 0) return;
  size_t index = 0;
  while (source && source[index] && index + 1 < size) {
    destination[index] =
        static_cast<char>(toupper(static_cast<unsigned char>(source[index])));
    ++index;
  }
  destination[index] = '\0';
}

void setError(char *error, size_t size, const char *message) {
  if (size == 0) return;
  snprintf(error, size, "%s", message);
}
}  // namespace

bool parseDepartureBoard(const char *json, size_t length,
                         DepartureBoard &board, char *error,
                         size_t errorSize) {
  board = DepartureBoard{};
  JsonDocument response;
  const DeserializationError jsonError =
      deserializeJson(response, json, length);
  if (jsonError) {
    setError(error, errorSize, jsonError.c_str());
    return false;
  }

  // A Signalboarder board has a station and a services ARRAY. The raw provider
  // payload has neither: it says locationName and trainServices. Checking the
  // SHAPE is what keeps that payload rejected now that an empty array has to be
  // accepted, because a station with no trains is an answer, not a failure.
  if (!response["station"].is<const char *>() ||
      !response["services"].is<JsonArrayConst>()) {
    setError(error, errorSize, "not a Signalboarder board");
    return false;
  }

  copyUpper(board.station, sizeof(board.station),
            response["station"] | "UNKNOWN");
  for (JsonObject service : response["services"].as<JsonArray>()) {
    if (board.serviceCount == 3) break;
    Departure &departure = board.services[board.serviceCount++];
    copyUpper(departure.scheduled, sizeof(departure.scheduled),
              service["scheduled"] | "--:--");
    copyUpper(departure.expected, sizeof(departure.expected),
              service["expected"] | "NO REPORT");
    copyUpper(departure.destination, sizeof(departure.destination),
              service["destination"] | "UNKNOWN");
    copyUpper(departure.platform, sizeof(departure.platform),
              service["platform"] | "");
    size_t used = 0;
    for (const char *stop : service["callingAt"].as<JsonArray>()) {
      if (!stop || !stop[0]) continue;
      // At the marquee's one-pixel horizontal pitch a normal single space is
      // almost invisible. Three spaces preserve the web board's clear pause
      // between consecutive station names on the physical panel.
      const char *separator = used == 0 ? "CALLING AT:   " : ",   ";
      const size_t remaining = sizeof(departure.callingAt) - used;
      const int written = snprintf(departure.callingAt + used,
                                   remaining,
                                   "%s%s", separator, stop);
      if (written < 0) break;
      const size_t appended = static_cast<size_t>(written) < remaining
          ? static_cast<size_t>(written)
          : remaining - 1;
      used += appended;
      if (used + 1 >= sizeof(departure.callingAt)) break;
    }
    for (char &letter : departure.callingAt) {
      letter = static_cast<char>(toupper(static_cast<unsigned char>(letter)));
    }
    // The server decided this. Recomputing it here is how the two drift.
    departure.disrupted = service["disrupted"] | false;
  }

  if (errorSize) error[0] = '\0';
  return true;
}
