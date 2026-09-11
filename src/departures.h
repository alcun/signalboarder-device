#pragma once

#include <stddef.h>

struct Departure {
  char scheduled[6] = "--:--";
  char expected[16] = "NO REPORT";
  char destination[40] = "NO SERVICE";
  char platform[8] = "";
  char callingAt[192] = "";
  bool disrupted = false;
};

struct DepartureBoard {
  char station[40] = "UNKNOWN";
  Departure services[3];
  size_t serviceCount = 0;
};

/**
 * Parse one Signalboarder server response into the board.
 *
 * The server serves THIS struct as JSON: same field names, same order, same
 * widths. It has already talked to the provider, bounded every string and
 * decided `disrupted`, so there is nothing to map here and no second copy of
 * those rules to drift. The board reads only the fields it names, so the server
 * may add more without touching the firmware.
 */
bool parseDepartureBoard(const char *json, size_t length,
                         DepartureBoard &board, char *error, size_t errorSize);
