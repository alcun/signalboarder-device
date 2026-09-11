#include "departures.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string.h>

namespace {
int failures = 0;

std::string readFixture(const char *path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool parseFixture(const char *name, DepartureBoard &board, char *error) {
  const std::string path = std::string("test/fixtures/") + name;
  const std::string json = readFixture(path.c_str());
  return parseDepartureBoard(json.data(), json.size(), board, error, 64);
}
}  // namespace

int main() {
  DepartureBoard board;
  char error[64] = "";

  expect(parseFixture("on-time.json", board, error), "on-time fixture parses");
  expect(strcmp(board.station, "NEW BRIGHTON") == 0, "station uppercased");
  expect(board.serviceCount == 3, "three services retained");
  expect(strcmp(board.services[0].destination, "LIVERPOOL CENTRAL") == 0,
         "destination mapped");
  expect(strcmp(board.services[0].scheduled, "13:08") == 0, "scheduled mapped");
  expect(strcmp(board.services[0].platform, "2") == 0, "platform mapped");
  expect(!board.services[0].disrupted, "on-time service is not disrupted");
  expect(strcmp(board.services[0].callingAt,
                "CALLING AT:   WALLASEY GROVE ROAD,   BIRKENHEAD NORTH,   LIVERPOOL CENTRAL") == 0,
         "calling points flattened for the marquee");

  expect(parseFixture("disrupted.json", board, error),
         "disrupted fixture parses");
  expect(board.services[0].disrupted, "late service is disrupted");
  expect(board.services[1].disrupted, "cancelled service is disrupted");
  // disrupted is READ, never recomputed. A board that re-derives it is a
  // second copy of the rule, which is the drift the server prevents.
  expect(strcmp(board.services[1].expected, "CANCELLED") == 0,
         "cancelled label retained");

  // A station with no trains due is an answer, not a failure: the panel says
  // NO SERVICES rather than DATA ERROR.
  expect(parseFixture("no-services.json", board, error),
         "empty board accepted");
  expect(board.serviceCount == 0, "empty board carries no services");
  expect(strcmp(board.station, "NEW BRIGHTON") == 0,
         "empty board still names its station");

  // Negative control. This is the raw PROVIDER payload the firmware used to
  // parse directly. It is valid JSON, so if the board still understood it this
  // suite would pass while the device silently bypassed the server contract.
  expect(!parseFixture("provider-shape.json", board, error),
         "raw provider payload is no longer understood");
  // And rejected for the right reason. It used to be caught by having no
  // services; now that an empty board is valid, only the shape check stands
  // between the firmware and the payload it must never parse again.
  expect(strcmp(error, "not a Signalboarder board") == 0,
         "provider payload rejected on shape, not on emptiness");

  expect(!parseFixture("malformed.json", board, error),
         "malformed JSON rejected");
  expect(error[0] != '\0', "malformed error reported");

  if (failures) return 1;
  std::cout << "All Signalboarder departure parser fixtures passed.\n";
  return 0;
}
