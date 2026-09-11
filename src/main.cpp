/*
 * Signalboarder AMOLED firmware.
 *
 * Panel initialization and pin assignments are adapted from Waveshare's
 * ESP32-S3-AMOLED-1.91 LVGL example (MIT, Copyright (c) 2024 Waveshare).
 * The QSPI command framing follows Espressif's Apache-2.0 esp_lcd_sh8601
 * component. See references/README.md for the pinned source archive.
 *
 * Direct framebuffer rendering, with no LVGL dependency.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>

#include "driver/spi_master.h"
#include "departures.h"
#include "dot_matrix_font.h"
#include "stations.h"
#include "web_font.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"

namespace {
constexpr uint32_t kSerialBaud = 115200;
constexpr int kWidth = 536;
constexpr int kHeight = 240;
constexpr spi_host_device_t kLcdHost = SPI2_HOST;
constexpr int kCs = 6;
constexpr int kClock = 47;
constexpr int kData0 = 18;
constexpr int kData1 = 7;
constexpr int kData2 = 48;
constexpr int kData3 = 5;
constexpr int kReset = 17;
constexpr int kTouchSda = 40;
constexpr int kTouchScl = 39;
constexpr int kTouchInterrupt = 41;
constexpr uint8_t kTouchAddress = 0x38;
// BOOT, where the board has one. Not every board will, which is why it is the
// second way into setup rather than the only one.
constexpr int kSetupButton = 0;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t color = static_cast<uint16_t>(
      ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
  // The ESP32 framebuffer is little-endian while the panel consumes RGB565
  // most-significant byte first.
  return static_cast<uint16_t>((color << 8) | (color >> 8));
}
constexpr uint16_t kBlack = rgb565(0, 0, 0);
// Keep these in lockstep with web/src/styles/board.css.
constexpr uint16_t kYellow = rgb565(255, 176, 0);    // #ffb000
constexpr uint16_t kRed = rgb565(255, 68, 54);       // #ff4436
constexpr uint16_t kDimYellow = rgb565(122, 84, 0);  // #7a5400

esp_lcd_panel_io_handle_t panelIo = nullptr;
uint16_t *framebuffer = nullptr;
Preferences preferences;
bool shouldSaveConfig = false;
char stationCode[4] = "NBN";

constexpr char kHostedApiUrl[] = "https://signalboarder.alcun.dev";
constexpr char kLegacyApiUrl[] = "https://api.signal.alcun.dev";
constexpr char kLegacySignalboarderApiUrl[] =
    "https://api.signalboarder.alcun.dev";

// A reusable build never opts into somebody else's service. ./flash provisions
// an explicit origin; a direct PlatformIO build opens setup with an empty one.
#ifdef SIGNALBOARDER_PROVISIONED_API_URL
char apiBase[128] = SIGNALBOARDER_PROVISIONED_API_URL;
#else
char apiBase[128] = "";
#endif
constexpr uint32_t kRefreshIntervalMs = 60000;
// A board that boots before the router finishes coming back after a power cut
// must not sit dead until somebody resets it. It keeps asking for the network
// it already knows, quietly, for as long as it takes.
constexpr uint32_t kReconnectEveryMs = 30000;
uint32_t lastReconnectAt = 0;
bool networkReady = false;
// True until the first departure board is drawn. Startup speaks through the
// one line under the wordmark rather than through a screen of its own.
bool booting = true;
constexpr uint32_t kSetupHoldMs = 700;
constexpr uint32_t kIdleTouchPollMs = 40;
constexpr uint32_t kEditorTouchPollMs = 40;
// The FT3168 reports a finger in flickers rather than continuously: during a
// real press most polls still come back with no points at all, and TP_INT
// never asserts. A gap shorter than this is the same touch, which is what lets
// a hold accumulate at all.
constexpr uint32_t kTouchGraceMs = 250;
uint32_t lastRefreshAt = 0;
uint32_t setupHoldStartedAt = 0;
uint32_t buttonHoldStartedAt = 0;
// Only true once BOOT has been seen released. A pin that is already low at
// startup is not a button being pressed, it is a board that does not have one,
// and treating it as a press drops the panel into the setup portal forever.
bool setupButtonArmed = false;
uint32_t lastTouchPollAt = 0;
bool setupGestureHandled = false;
bool touchWasLive = false;
uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;
uint32_t lastTouchSeenAt = 0;

void renderStatus(const char *title, const char *detail);
void showBootLine(const char *line);
bool flushRows(int top, int height);
bool configureNetwork(bool forcePortal = false);
void onNetworkUp();
// Nothing paints the board while another screen owns the panel. The network
// coming back mid-edit painted departures over the station editor, which still
// held the touches: the panel said one thing and the firmware believed another.
bool boardVisible();
void startBoardServer();
void stopBoardServer();
void refreshDepartures();
DepartureBoard currentBoard;
bool hasCurrentBoard = false;
bool currentBoardStale = false;
uint32_t lastBoardFrameAt = 0;

enum class UiMode {
  Board,
  StationEditor,
};
UiMode uiMode = UiMode::Board;
// The code being spun. Always three letters, and always a real station.
char editedStation[4] = "NBN";

// Compact 5x7 uppercase font. Each byte is one five-pixel column, LSB at top.
const uint8_t kFont[][5] = {
    {0,0,0,0,0}, {0x00,0x00,0x5f,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7f,0x14,0x7f,0x14}, {0x24,0x2a,0x7f,0x2a,0x12},
    {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50},
    {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1c,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1c,0x00}, {0x14,0x08,0x3e,0x08,0x14},
    {0x08,0x08,0x3e,0x08,0x08}, {0x00,0x50,0x30,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3e,0x51,0x49,0x45,0x3e},
    {0x00,0x42,0x7f,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1e}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08},
    {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

void fillRect(int x, int y, int w, int h, uint16_t color) {
  for (int row = max(0, y); row < min(kHeight, y + h); ++row) {
    for (int col = max(0, x); col < min(kWidth, x + w); ++col) {
      framebuffer[row * kWidth + col] = color;
    }
  }
}

// The web board uses regular, bold and tall cuts of Daniel Hart's Dot Matrix.
// The panel keeps its tiny bitmap face but gives it the same three roles: the
// regular call above, a heavier first row, and narrow/tall time numerals.
const DotGlyph &dotGlyph(char character, const DotGlyph *font) {
  const DotGlyph &glyph = font[character - 32];
  if (font == kDotTall && character != ' ') {
    bool empty = true;
    for (uint8_t row : glyph.rows) empty = empty && row == 0;
    if (empty) return kDotRegular[character - 32];
  }
  return glyph;
}

int dotTextWidth(const char *text, int pitch, const DotGlyph *font) {
  int width = 0;
  for (; *text; ++text) {
    char c = *text;
    if (c < 32 || c > 90) c = ' ';
    width += dotGlyph(c, font).advance * pitch;
  }
  return width;
}

void drawDotText(int x, int y, const char *text, int pitchX, int pitchY,
                 uint16_t color, const DotGlyph *font,
                 int clipLeft = 0, int clipRight = kWidth) {
  const int dot = max(1, min(pitchX, pitchY) - 1);
  for (; *text; ++text) {
    char c = *text;
    if (c < 32 || c > 90) c = ' ';
    const DotGlyph &glyph = dotGlyph(c, font);
    for (int row = 0; row < 9; ++row) {
      for (int col = 0; col < 8; ++col) {
        if (glyph.rows[row] & (1U << col)) {
          const int dotX = x + col * pitchX;
          if (dotX >= clipLeft && dotX + dot <= clipRight) {
            fillRect(dotX, y + row * pitchY, dot, dot, color);
          }
        }
      }
    }
    x += glyph.advance * pitchX;
  }
}

void drawText(int x, int y, const char *text, int scale, uint16_t color) {
  drawDotText(x, y, text, scale, scale, color, kDotRegular);
}

void drawTallText(int x, int y, const char *text, int scaleX, int scaleY,
                  uint16_t color) {
  drawDotText(x, y, text, scaleX, scaleY, color, kDotTall);
}

void copyUpper(char *destination, size_t size, const char *source) {
  if (size == 0) return;
  size_t index = 0;
  while (source && source[index] && index + 1 < size) {
    destination[index] = toupper(static_cast<unsigned char>(source[index]));
    ++index;
  }
  destination[index] = '\0';
}

esp_err_t sendCommand(uint8_t command, const void *data = nullptr, size_t size = 0) {
  const uint32_t framed = (0x02UL << 24) | (static_cast<uint32_t>(command) << 8);
  return esp_lcd_panel_io_tx_param(panelIo, framed, data, size);
}

bool initDisplay() {
  pinMode(kReset, OUTPUT);
  digitalWrite(kReset, LOW);
  delay(20);
  digitalWrite(kReset, HIGH);
  delay(120);

  spi_bus_config_t bus = {};
  bus.data0_io_num = kData0;
  bus.data1_io_num = kData1;
  bus.sclk_io_num = kClock;
  bus.data2_io_num = kData2;
  bus.data3_io_num = kData3;
  bus.max_transfer_sz = kWidth * kHeight * sizeof(uint16_t);
  if (spi_bus_initialize(kLcdHost, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = kCs;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = 40 * 1000 * 1000;
  io.trans_queue_depth = 1;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = true;
  if (esp_lcd_new_panel_io_spi(
          static_cast<esp_lcd_spi_bus_handle_t>(kLcdHost), &io, &panelIo) != ESP_OK) {
    return false;
  }

  const uint8_t madctl = 0xf0;
  const uint8_t rgb565Mode = 0x55;
  const uint8_t columns[] = {0x00, 0x00, 0x02, 0x17};
  const uint8_t rows[] = {0x00, 0x00, 0x00, 0xef};
  const uint8_t dark = 0x00;
  const uint8_t bright = 0xff;
  if (sendCommand(0x11) != ESP_OK) return false;
  delay(120);
  if (sendCommand(0x36, &madctl, 1) != ESP_OK) return false;
  if (sendCommand(0x3a, &rgb565Mode, 1) != ESP_OK) return false;
  if (sendCommand(0x2a, columns, sizeof(columns)) != ESP_OK) return false;
  if (sendCommand(0x2b, rows, sizeof(rows)) != ESP_OK) return false;
  if (sendCommand(0x51, &dark, 1) != ESP_OK) return false;
  delay(10);
  if (sendCommand(0x29) != ESP_OK) return false;
  delay(10);
  return sendCommand(0x51, &bright, 1) == ESP_OK;
}

// A whole frame is 257KB over QSPI. A spin changes two narrow bands of the
// editor, so it sends those rows and nothing else: the difference is what makes
// spinning keep up with a finger.
bool flushRows(int top, int height) {
  const int first = max(0, top);
  const int last = min(kHeight, top + height) - 1;
  if (last < first) return true;
  const uint8_t columns[] = {0x00, 0x00, 0x02, 0x17};
  const uint8_t rows[] = {static_cast<uint8_t>(first >> 8),
                          static_cast<uint8_t>(first & 0xff),
                          static_cast<uint8_t>(last >> 8),
                          static_cast<uint8_t>(last & 0xff)};
  if (sendCommand(0x2a, columns, sizeof(columns)) != ESP_OK) return false;
  if (sendCommand(0x2b, rows, sizeof(rows)) != ESP_OK) return false;
  const uint32_t writeMemory = (0x32UL << 24) | (0x2cUL << 8);
  const size_t bytes =
      static_cast<size_t>(kWidth) * (last - first + 1) * sizeof(uint16_t);
  return esp_lcd_panel_io_tx_color(panelIo, writeMemory,
                                   framebuffer + static_cast<size_t>(first) * kWidth,
                                   bytes) == ESP_OK;
}

bool flushDisplay() { return flushRows(0, kHeight); }

void renderBoard() {
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(0, 0, kWidth, 4, kYellow);
  fillRect(0, 49, kWidth, 2, kDimYellow);
  fillRect(0, 142, kWidth, 2, kDimYellow);
  fillRect(0, 236, kWidth, 4, kYellow);

  drawText(14, 14, "NEW BRIGHTON", 3, kYellow);
  drawText(428, 14, "14:32", 3, kYellow);

  drawText(14, 70, "14:38", 4, kYellow);
  drawText(150, 62, "LIVERPOOL", 3, kYellow);
  drawText(150, 88, "CENTRAL", 3, kYellow);
  drawText(407, 82, "ON TIME", 2, kYellow);

  drawText(14, 165, "14:53", 4, kYellow);
  drawText(150, 157, "LIVERPOOL", 3, kYellow);
  drawText(150, 183, "CENTRAL", 3, kYellow);
  drawText(407, 177, "2 MIN LATE", 2, kRed);
}

void drawDestination(int x, int y, const char *destination, int clipRight,
                     bool bold = false) {
  // Keep the complete model value and clip it only where the expected-status
  // column actually begins. A fixed 16-character buffer cut the final L from
  // LIVERPOOL CENTRAL despite there being ample pixels for it on this panel.
  char text[40];
  copyUpper(text, sizeof(text), destination);
  drawDotText(x, y, text, 2, 2, kYellow,
              bold ? kDotBold : kDotRegular, x, clipRight);
}

void renderDepartures(const DepartureBoard &board, bool stale = false) {
  booting = false;
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(12, 29, kWidth - 24, 1, kDimYellow);

  char station[22];
  copyUpper(station, sizeof(station), board.station);
  const int stationWidth = dotTextWidth(station, 3, kDotRegular);
  drawDotText((kWidth - stationWidth) / 2, 4, station, 3, 3, kYellow,
              kDotRegular);
  char clock[9] = "--:--:--";
  struct tm now = {};
  if (getLocalTime(&now, 10)) strftime(clock, sizeof(clock), "%H:%M:%S", &now);
  char date[12] = "";
  if (now.tm_year) strftime(date, sizeof(date), "%a %d %b", &now);
  for (char &letter : date) letter = toupper(letter);
  drawDotText(kWidth - 10 - dotTextWidth(date, 2, kDotRegular), 8, date,
              2, 2, kYellow, kDotRegular);
  if (stale) drawDotText(12, 8, "STALE", 2, 2, kRed, kDotRegular);

  const int rowY[] = {36, 84, 116};
  if (board.serviceCount == 0) {
    // The web board says "No services" for this. Same words here.
    const char *empty = "NO SERVICES";
    const int emptyWidth = dotTextWidth(empty, 3, kDotRegular);
    drawDotText((kWidth - emptyWidth) / 2, 76, empty, 3, 3, kDimYellow,
                kDotRegular);
  }
  for (size_t index = 0; index < board.serviceCount; ++index) {
    const Departure &service = board.services[index];
    if (index > 0) {
      drawDotText(12, rowY[index] + 3, index == 1 ? "2ND" : "3RD",
                  1, 2, kDimYellow, kDotRegular);
    }
    drawDotText(50, rowY[index], service.scheduled, 2, 2, kYellow, kDotTall);

    if (service.platform[0]) {
      drawDotText(145, rowY[index] + 2, service.platform, 2, 2, kYellow,
                  index == 0 ? kDotBold : kDotRegular);
    }
    const int expectedWidth = dotTextWidth(service.expected, 2, kDotTall);
    const int expectedX = kWidth - 12 - expectedWidth;
    drawDestination(170, rowY[index], service.destination, expectedX - 10,
                    index == 0);
    drawDotText(expectedX, rowY[index],
                service.expected, 2, 2,
                service.disrupted ? kRed : kYellow, kDotTall);
  }

  if (board.services[0].callingAt[0]) {
    constexpr int kCallingLeft = 50;
    constexpr int kCallingRight = kWidth - 12;
    constexpr int kCallingPitch = 1;
    const int textWidth = dotTextWidth(board.services[0].callingAt,
                                       kCallingPitch, kDotBold);
    constexpr int kCallingGap = 36;
    const int travel = textWidth + kCallingGap;
    const int x = kCallingLeft - static_cast<int>((millis() / 45) % travel);
    drawDotText(x, 58, board.services[0].callingAt, kCallingPitch, 2,
                kYellow, kDotBold, kCallingLeft, kCallingRight);
    // Keep a second copy one full cycle behind the first. With only one copy,
    // the whole marquee went blank at the wrap and looked as though scrolling
    // had stalled before the text restarted at the right edge.
    drawDotText(x + travel, 58, board.services[0].callingAt, kCallingPitch, 2,
                kYellow, kDotBold, kCallingLeft, kCallingRight);
  }

  // As on the web platform board, the clock is a primary board element rather
  // than header chrome. The seconds use the regular face at a smaller pitch.
  char hoursMinutes[] = {clock[0], clock[1], ':', clock[3], clock[4], '\0'};
  char seconds[] = {clock[6], clock[7], '\0'};
  const int clockWidth = dotTextWidth(hoursMinutes, 5, kDotTall);
  const int clockX = (kWidth - clockWidth) / 2;
  drawTallText(clockX, 158, hoursMinutes, 5, 6, kYellow);
  drawDotText(clockX + clockWidth + 8, 184, seconds, 3, 3, kYellow,
              kDotRegular);
  flushDisplay();
}

// The station editor: three letters, spun one at a time, always a station.
//
// The panel is 44mm by 20mm. A keyboard on it would be 5mm keys against a 9mm
// fingertip, so the letters keep the old editor's three tall columns, which are
// 10mm by 7mm and work. What the old editor got wrong was not its shape: it
// saved whichever three letters you stopped on, so one wrong letter left the
// board asking a server about a station that does not exist, with nothing on
// the panel to say so. The station list is on the device now, so a spin only
// offers letters some station continues from and the name of where you are is
// always on screen. There is no invalid state to be stuck in.
constexpr int kLetterColumns[] = {52, 178, 304};
constexpr int kColumnWidth = 126;
constexpr int kActionLeft = 410;
constexpr uint32_t kSpinRepeatDelayMs = 300;
constexpr uint32_t kSpinRepeatEveryMs = 110;
uint32_t editorHoldStartedAt = 0;
uint32_t lastSpinAt = 0;
// The finger that opens the editor is still on the glass when the editor
// appears, right over the first letter's `+`. Nothing is accepted until it
// lifts, and the same applies leaving: one touch does one thing.
bool editorAwaitingRelease = false;

// The panel has no wrapping, so pick the largest scale the name fits in.
void drawFittedName(int x, int y, const char *name, int room, uint16_t color) {
  char upper[41];
  copyUpper(upper, sizeof(upper), name);
  int scale = 3;
  while (scale > 1 && dotTextWidth(upper, scale, kDotRegular) > room) --scale;
  // Two is the floor. A name still too wide there loses its tail, which is
  // better than losing the rest of the panel.
  drawDotText(x, y, upper, scale, scale, color, kDotRegular, x, x + room);
}

// The two bands a spin changes: the name, and the row of letters. The `+` and
// `-` marks and the right-hand actions never move, so they are drawn once.
constexpr int kNameBandTop = 8;
constexpr int kNameBandHeight = 38;
constexpr int kLetterBandTop = 88;
constexpr int kLetterBandHeight = 80;
// Everything left of the actions, so redrawing a band cannot erase CANCEL.
constexpr int kEditorBandWidth = 404;

void drawEditorBands() {
  fillRect(0, kNameBandTop, kEditorBandWidth, kNameBandHeight, kBlack);
  fillRect(0, kLetterBandTop, kEditorBandWidth, kLetterBandHeight, kBlack);

  // The name, not a label: it is the only thing here that says whether the
  // three letters are the ones you wanted.
  const char *name = stationName(editedStation);
  drawFittedName(16, 12, name ? name : editedStation, 388, kYellow);
  for (int index = 0; index < 3; ++index) {
    const char letter[] = {editedStation[index], '\0'};
    drawText(kLetterColumns[index], 92, letter, 8, kYellow);
  }
}

void renderStationEditor() {
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(0, 0, kWidth, 4, kYellow);
  fillRect(0, 236, kWidth, 4, kYellow);
  for (int index = 0; index < 3; ++index) {
    drawText(kLetterColumns[index] + 26, 50, "+", 4, kDimYellow);
    drawText(kLetterColumns[index] + 26, 178, "-", 4, kDimYellow);
  }
  drawText(424, 62, "SAVE", 2, kYellow);
  drawText(424, 112, "CANCEL", 2, kDimYellow);
  drawText(424, 174, "WI-FI", 2, kDimYellow);
  drawEditorBands();
  flushDisplay();
}

/** A spin repaints the two bands it changed, not the panel. */
void renderEditorLetters() {
  drawEditorBands();
  flushRows(kNameBandTop, kNameBandHeight);
  flushRows(kLetterBandTop, kLetterBandHeight);
}

void openStationEditor() {
  strlcpy(editedStation, stationCode, sizeof(editedStation));
  // A board saved by an older firmware can be holding three letters that name
  // nothing. Start from the nearest real station rather than from those.
  repairCrs(editedStation);
  uiMode = UiMode::StationEditor;
  // The touch that opened the editor must not also count as its first tap.
  touchWasLive = true;
  editorAwaitingRelease = true;
  editorHoldStartedAt = 0;
  renderStationEditor();
  Serial.printf("Station editor opened at %s.\n", editedStation);
}

// Leaving is the same rule the other way about: the touch that left the editor
// must not be read by the board as the start of the gesture that reopens it.
void returnToBoard() {
  uiMode = UiMode::Board;
  setupHoldStartedAt = 0;
  setupGestureHandled = true;
}

void closeStationEditor() {
  returnToBoard();
  if (hasCurrentBoard) renderDepartures(currentBoard, currentBoardStale);
}

void spinEditedLetter(int index, int direction) {
  editedStation[index] = nextReachableLetter(editedStation, index, direction);
  // The letters after this one may no longer continue anywhere. Keep the ones
  // that still do.
  repairCrs(editedStation);
  renderEditorLetters();
}

void saveEditedStation() {
  strlcpy(stationCode, editedStation, sizeof(stationCode));
  preferences.begin("signalboarder", false);
  preferences.putString("station", stationCode);
  preferences.end();
  Serial.printf("Station editor saved: %s (%s)\n", stationCode,
                stationName(stationCode));
  returnToBoard();
  hasCurrentBoard = false;
  refreshDepartures();
}

/** The letter column and direction under a touch, or false for neither. */
bool spinUnderTouch(uint16_t x, uint16_t y, int &index, int &direction) {
  if (x >= kActionLeft) return false;
  index = min(2, static_cast<int>(x / kColumnWidth));
  if (y < 86) {
    direction = 1;
    return true;
  }
  if (y > 158) {
    direction = -1;
    return true;
  }
  return false;
}

void handleEditorTap(uint16_t x, uint16_t y) {
  int index = 0;
  int direction = 0;
  if (spinUnderTouch(x, y, index, direction)) {
    spinEditedLetter(index, direction);
    return;
  }
  if (x < kActionLeft) return;

  if (y < 105) {
    saveEditedStation();
  } else if (y < 158) {
    closeStationEditor();
  } else {
    if (configureNetwork(true)) {
      strlcpy(editedStation, stationCode, sizeof(editedStation));
      repairCrs(editedStation);
      returnToBoard();
      hasCurrentBoard = false;
      refreshDepartures();
    } else {
      renderStationEditor();
    }
  }
}

/** Holding a + or - keeps spinning, so a distant letter is not 12 taps. */
void handleEditorHold(uint16_t x, uint16_t y) {
  int index = 0;
  int direction = 0;
  if (!spinUnderTouch(x, y, index, direction)) return;
  if (millis() - editorHoldStartedAt < kSpinRepeatDelayMs) return;
  if (millis() - lastSpinAt < kSpinRepeatEveryMs) return;
  lastSpinAt = millis();
  spinEditedLetter(index, direction);
}

bool fetchDepartures(DepartureBoard &board) {
  // The board talks to a Signalboarder server, never to a provider. The
  // National Rail key, the caching and the whole Darwin mapping live there, so
  // the device holds no secret and a provider change never needs a reflash.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const String url = String(apiBase) + "/v1/departures/" + stationCode +
                     "?rows=3";
  if (!http.begin(client, url)) {
    Serial.println("Signalboarder: HTTPS setup failed.");
    return false;
  }
  http.setUserAgent("Signalboarder-Device/1.0");
  http.setTimeout(15000);
  const int status = http.GET();
  Serial.printf("Signalboarder: HTTP %d\n", status);
  if (status != HTTP_CODE_OK) {
    // 400 is a well-formed CRS the server does not know, which is a mistyped
    // station rather than an outage. Saying SERVER ERROR to someone who typed
    // three wrong letters sends them looking for the wrong fault.
    if ((status == 400 || status == 404) && boardVisible()) {
      renderStatus("NO STATION", stationCode);
    }
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();
  char parseError[64] = "";
  if (!parseDepartureBoard(body.c_str(), body.length(), board, parseError,
                           sizeof(parseError))) {
    Serial.printf("Signalboarder: JSON error: %s\n", parseError);
    return false;
  }
  Serial.printf("Signalboarder: parsed %u services for %s.\n",
                static_cast<unsigned>(board.serviceCount), board.station);
  // Zero services is a station with no trains due, which the panel says in
  // words. Reporting it as a failed fetch put DATA ERROR on the screen for a
  // board that had answered perfectly.
  return true;
}

bool boardVisible() { return uiMode == UiMode::Board; }

// Everything that only makes sense with a network, whether it arrived during
// setup or half an hour later.
void onNetworkUp() {
  networkReady = true;
  startBoardServer();
  if (booting) {
    showBootLine("SETTING THE CLOCK");
  } else if (boardVisible()) {
    char address[41];
    snprintf(address, sizeof(address), "SIGNALBOARDER.LOCAL  %s",
             WiFi.localIP().toString().c_str());
    renderStatus("CONNECTED", address);
  }
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2", "pool.ntp.org",
               "time.cloudflare.com");
  struct tm localTime = {};
  if (getLocalTime(&localTime, 10000)) {
    Serial.printf("British time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  localTime.tm_year + 1900, localTime.tm_mon + 1,
                  localTime.tm_mday, localTime.tm_hour, localTime.tm_min,
                  localTime.tm_sec);
  } else {
    Serial.println("WARNING: NTP synchronization timed out.");
  }
  refreshDepartures();
}

void refreshDepartures() {
  DepartureBoard board;
  // No server means every fetch would build "/v1/departures/NBN" and fail as a
  // network fault, which reads as broken hardware rather than missing setup.
  if (apiBase[0] == '\0') {
    if (boardVisible()) renderStatus("NO SERVER", "HOLD TO OPEN SETUP");
    return;
  }
  if (booting) {
    showBootLine("FETCHING DEPARTURES");
  } else if (!hasCurrentBoard && boardVisible()) {
    renderStatus("UPDATING", stationCode);
  }
  if (fetchDepartures(board)) {
    currentBoard = board;
    hasCurrentBoard = true;
    currentBoardStale = false;
    if (boardVisible()) renderDepartures(currentBoard);
  } else {
    if (hasCurrentBoard) {
      currentBoardStale = true;
      if (boardVisible()) renderDepartures(currentBoard, true);
    } else if (boardVisible()) {
      renderStatus("DATA ERROR", "RETRYING IN 60 SECONDS");
    }
  }
  lastRefreshAt = millis();
}

void renderStatus(const char *title, const char *detail) {
  fillRect(0, 0, kWidth, kHeight, kBlack);
  fillRect(0, 0, kWidth, 4, kYellow);
  fillRect(0, 236, kWidth, 4, kYellow);
  drawText(18, 68, title, 5, kYellow);
  drawText(20, 132, detail, 2, kDimYellow);
  flushDisplay();
}

// One screen for the whole startup.
//
// The board used to flash through four of them - the train, then CONNECTING,
// then CONNECTED, then UPDATING - before the departures arrived, and it read as
// a rough loop rather than a thing waking up. So the wordmark is drawn once and
// stays put, and the only thing that ever moves after that is the line beneath
// it. The train earns its keep by revealing the word as it passes, rather than
// being a splash the board then throws away.
constexpr char kWordmark[] = "SIGNALBOARDER";
constexpr int kWordmarkScale = 5;
constexpr int kWordmarkY = 80;
constexpr int kBootLineY = 152;
constexpr int kBootLineBandTop = 148;
constexpr int kBootLineBandHeight = 22;

void drawBootFrame() {
  fillRect(0, 0, kWidth, 4, kYellow);
  fillRect(0, 236, kWidth, 4, kYellow);
}

/** The line under the wordmark, and nothing else, so the word never flickers. */
void showBootLine(const char *line) {
  fillRect(0, kBootLineBandTop, kWidth, kBootLineBandHeight, kBlack);
  const int width = dotTextWidth(line, 2, kDotRegular);
  drawDotText((kWidth - width) / 2, kBootLineY, line, 2, 2, kDimYellow,
              kDotRegular);
  flushRows(kBootLineBandTop, kBootLineBandHeight);
}

void renderBootScreen(const char *line) {
  fillRect(0, 0, kWidth, kHeight, kBlack);
  drawBootFrame();
  const int wordWidth = dotTextWidth(kWordmark, kWordmarkScale, kDotRegular);
  drawDotText((kWidth - wordWidth) / 2, kWordmarkY, kWordmark, kWordmarkScale,
              kWordmarkScale, kYellow, kDotRegular);
  const int lineWidth = dotTextWidth(line, 2, kDotRegular);
  drawDotText((kWidth - lineWidth) / 2, kBootLineY, line, 2, 2, kDimYellow,
              kDotRegular);
  flushDisplay();
}

void renderBootAnimation() {
  const int wordWidth = dotTextWidth(kWordmark, kWordmarkScale, kDotRegular);
  const int wordX = (kWidth - wordWidth) / 2;
  const int wordBottom = kWordmarkY + 9 * kWordmarkScale;
  constexpr int kTrainWidth = 52;
  constexpr int kTrainHeight = 16;

  // The train runs the full width along the wordmark's baseline, and the word
  // exists only behind it: everything to the train's left has been revealed.
  for (int frame = 0; frame <= 24; ++frame) {
    const int trainX = -kTrainWidth + frame * ((kWidth + kTrainWidth) / 24);
    fillRect(0, 0, kWidth, kHeight, kBlack);
    drawBootFrame();
    fillRect(22, wordBottom + 12, kWidth - 44, 2, kDimYellow);
    drawDotText(wordX, kWordmarkY, kWordmark, kWordmarkScale, kWordmarkScale,
                kYellow, kDotRegular, 0, trainX);
    fillRect(trainX, wordBottom - 4, kTrainWidth, kTrainHeight, kYellow);
    fillRect(trainX + 8, wordBottom, 8, 6, kBlack);
    fillRect(trainX + 23, wordBottom, 8, 6, kBlack);
    fillRect(trainX + 38, wordBottom, 8, 6, kBlack);
    flushDisplay();
    delay(28);
  }
  renderBootScreen("STARTING");
}

void saveConfig() {
  shouldSaveConfig = true;
}

// The setup portal's half of station choice, for boards with no touch panel
// and for anyone who would rather search by name than know the code.
//
// The list is already on the device, so the phone never needs the internet it
// does not have while joined to SIGNALBOARDER-SETUP: it asks the board, and the
// board searches its own table with the same ranking as the web site.
WiFiManager *activeManager = nullptr;

constexpr char kStationPickerHtml[] = R"HTML(
<br/><label for='sbq'>Find a station</label>
<input id='sbq' type='search' autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='Glasgow Central or GLC'>
<div id='sbr'></div>
<script>
(function(){
  var box=document.getElementById('sbq'),out=document.getElementById('sbr'),timer;
  function choose(name,crs){
    var field=document.getElementById('station');
    if(field)field.value=crs;
    out.textContent=name+' ('+crs+') selected';
    box.value=name;
  }
  function show(list){
    out.textContent='';
    if(!list.length){out.textContent='No station of that name.';return;}
    list.forEach(function(station){
      var option=document.createElement('button');
      option.type='button';
      option.textContent=station[0]+'  '+station[1];
      option.style.cssText='display:block;width:100%;text-align:left;margin:2px 0';
      option.onclick=function(){choose(station[0],station[1]);};
      out.appendChild(option);
    });
  }
  box.addEventListener('input',function(){
    clearTimeout(timer);
    timer=setTimeout(function(){
      if(!box.value){out.textContent='';return;}
      fetch('/stations?q='+encodeURIComponent(box.value))
        .then(function(response){return response.json();})
        .then(show)
        .catch(function(){out.textContent='Search unavailable. Type the code below.';});
    },150);
  });
})();
</script>
)HTML";

String stationSearchJson(const String &query) {
  uint16_t matches[8];
  const size_t found = searchStations(query.c_str(), matches,
                                      sizeof(matches) / sizeof(matches[0]));
  String body = "[";
  for (size_t index = 0; index < found; ++index) {
    char crs[4] = "";
    stationCrsAt(matches[index], crs);
    if (index > 0) body += ',';
    // No station name contains a quote or a backslash, which is why this can be
    // built by hand.
    body += "[\"";
    body += stationNameAt(matches[index]);
    body += "\",\"";
    body += crs;
    body += "\"]";
  }
  body += "]";
  return body;
}

void handleStationSearch() {
  if (!activeManager || !activeManager->server) return;
  WebServer &server = *activeManager->server;
  server.send(200, "application/json", stationSearchJson(server.arg("q")));
}

// WiFiManager calls this after it has built its web server and before it
// registers its own routes, which is the one moment a route can be added.
void bindStationSearch() {
  if (!activeManager || !activeManager->server) return;
  activeManager->server->on("/stations", handleStationSearch);
}

// The board's own page, on your network, for the case the panel cannot cover:
// a board with no touch screen that is already connected, where there is
// nothing to hold and no reason to make anyone unplug anything. Browse to
// http://signalboarder.local (or the address on the CONNECTED screen), search
// for a station, and the panel changes.
//
// It searches the same table the panel and the setup portal search, so all
// three agree about what a station is.
WebServer boardServer(80);
bool boardServerRunning = false;

constexpr char kControlPage[] = R"HTML(<!doctype html>
<html lang='en'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Signalboarder</title>
<style>
 :root{color-scheme:dark}
 @font-face{font-family:'Dot Matrix';src:url('/f/r.woff2') format('woff2');font-display:block}
 @font-face{font-family:'Dot Matrix Bold';src:url('/f/b.woff2') format('woff2');font-display:block}
 body{background:#000;color:#ffb000;font:16px/1.5 system-ui,sans-serif;margin:0 auto;padding:2.5rem 1.25rem 4rem;max-width:34rem}
 h1,p.now,dt,dd,button,input,label{font-family:'Dot Matrix',ui-monospace,monospace;text-transform:uppercase;letter-spacing:.04em}
 p.now{font-family:'Dot Matrix Bold','Dot Matrix',ui-monospace,monospace}
 input::placeholder{color:#7a5400}
 small{text-transform:none}
 h1{font-size:.85rem;letter-spacing:.18em;color:#7a5400;margin:2rem 0 .35rem}
 h1:first-child{margin-top:0}
 p.now{font-size:2rem;line-height:1.1;margin:0 0 .75rem}
 label{display:block;font-size:.9rem;color:#7a5400;margin:0 0 .35rem}
 input{width:100%;box-sizing:border-box;background:#000;color:#ffb000;border:2px solid #7a5400;border-radius:.25rem;padding:.6rem .7rem;font:inherit}
 input:focus{outline:none;border-color:#ffb000}
 button{display:block;width:100%;text-align:left;background:#000;color:#ffb000;border:2px solid #7a5400;border-radius:.25rem;padding:.6rem .7rem;font:inherit;margin:.4rem 0;cursor:pointer}
 button:hover{border-color:#ffb000}
 button.danger{color:#ff4436;border-color:#5a1a15}
 button.danger:hover{border-color:#ff4436}
 dl{display:grid;grid-template-columns:auto 1fr;gap:.15rem .9rem;margin:0;font-size:.95rem}
 dt{color:#7a5400}
 dd{margin:0;overflow-wrap:anywhere}
 .row{display:flex;gap:.5rem}
 .row button{width:auto;margin:0}
 small{color:#7a5400;display:block;margin:.4rem 0 0}
</style></head><body>
<h1>Now showing</h1>
<p class='now' id='now'>&nbsp;</p>

<label for='q'>Change station</label>
<input id='q' type='search' autocomplete='off' spellcheck='false' placeholder='Glasgow Central or GLC'>
<div id='results'></div>
<small id='status'>&nbsp;</small>

<h1>This board</h1>
<dl>
 <dt>Network</dt><dd id='ssid'>&nbsp;</dd>
 <dt>Address</dt><dd id='ip'>&nbsp;</dd>
 <dt>Signal</dt><dd id='rssi'>&nbsp;</dd>
 <dt>Up for</dt><dd id='uptime'>&nbsp;</dd>
</dl>

<h1>Signalboarder server</h1>
<label for='api'>The board reads departures from here, and never from a provider</label>
<div class='row'><input id='api' type='url' autocomplete='off' spellcheck='false'><button id='saveapi'>Save</button></div>
<small id='apistatus'>&nbsp;</small>

<h1>Wi-Fi</h1>
<button class='danger' id='portal'>Open the setup portal</button>
<small>The board leaves this network and starts <b>SIGNALBOARDER-SETUP</b> for
three minutes, so this page goes away until it rejoins. Join that network from a
phone to choose a different Wi-Fi.</small>

<script>
 var box=document.getElementById('q'),out=document.getElementById('results'),
     now=document.getElementById('now'),status=document.getElementById('status'),
     api=document.getElementById('api'),apistatus=document.getElementById('apistatus'),timer;
 function refresh(){
   fetch('/now').then(function(r){return r.json()}).then(function(s){
     now.textContent=s.name+' ('+s.crs+')';
     document.getElementById('ssid').textContent=s.ssid;
     document.getElementById('ip').textContent=s.ip;
     document.getElementById('rssi').textContent=s.rssi+' dBm';
     document.getElementById('uptime').textContent=s.uptime;
     if(document.activeElement!==api)api.value=s.api;
   }).catch(function(){});
 }
 function choose(name,crs){
   status.textContent='Setting '+name+'...';
   fetch('/set?crs='+crs,{method:'POST'}).then(function(r){return r.json()}).then(function(s){
     status.textContent=s.ok?('The board is showing '+s.name+'.'):(s.error||'Refused.');
     out.textContent='';box.value='';refresh();});
 }
 box.addEventListener('input',function(){
   clearTimeout(timer);
   timer=setTimeout(function(){
     if(!box.value){out.textContent='';return}
     fetch('/stations?q='+encodeURIComponent(box.value))
       .then(function(r){return r.json()}).then(function(list){
         out.textContent='';
         if(!list.length){out.textContent='No station of that name.';return}
         list.forEach(function(st){
           var b=document.createElement('button');b.type='button';
           b.textContent=st[0]+'  '+st[1];
           b.onclick=function(){choose(st[0],st[1])};out.appendChild(b);});
       }).catch(function(){out.textContent='Search unavailable.'});
   },150);
 });
 document.getElementById('saveapi').onclick=function(){
   apistatus.textContent='Saving...';
   fetch('/api?url='+encodeURIComponent(api.value),{method:'POST'})
     .then(function(r){return r.json()}).then(function(s){
       apistatus.textContent=s.ok?('Reading departures from '+s.api+'.'):(s.error||'Refused.');
       refresh();});
 };
 document.getElementById('portal').onclick=function(){
   if(!confirm('The board will leave this network and start SIGNALBOARDER-SETUP. Continue?'))return;
   fetch('/portal',{method:'POST'}).then(function(){
     document.body.innerHTML='<h1>Setup portal open</h1><p class=now>Join '+
       'SIGNALBOARDER-SETUP from a phone.</p>';});
 };
 refresh();
 setInterval(refresh,5000);
</script></body></html>)HTML";

void handleControlRoot() {
  boardServer.send(200, "text/html", kControlPage);
}

void handleControlNow() {
  const char *name = stationName(stationCode);
  const uint32_t seconds = millis() / 1000;
  char uptime[24];
  if (seconds < 3600) {
    snprintf(uptime, sizeof(uptime), "%um", seconds / 60);
  } else {
    snprintf(uptime, sizeof(uptime), "%uh %um", seconds / 3600,
             (seconds % 3600) / 60);
  }
  String body = "{\"crs\":\"";
  body += stationCode;
  body += "\",\"name\":\"";
  body += name ? name : stationCode;
  body += "\",\"ssid\":\"";
  body += WiFi.SSID();
  body += "\",\"ip\":\"";
  body += WiFi.localIP().toString();
  body += "\",\"rssi\":";
  body += WiFi.RSSI();
  body += ",\"uptime\":\"";
  body += uptime;
  body += "\",\"api\":\"";
  body += apiBase;
  body += "\"}";
  boardServer.send(200, "application/json", body);
}

void handleControlSearch() {
  boardServer.send(200, "application/json",
                   stationSearchJson(boardServer.arg("q")));
}

// A page you happen to visit must not be able to retune your board. Anything
// that changes the device takes a POST, and a POST carrying an Origin has to
// carry OUR origin: that is what a cross-site form or fetch cannot forge. A
// request with no Origin at all is a script or curl, not a browser being
// steered by somebody else's page.
bool sameOriginRequest() {
  if (!boardServer.hasHeader("Origin")) return true;
  const String origin = boardServer.header("Origin");
  const String host = boardServer.hasHeader("Host") ? boardServer.header("Host")
                                                    : String();
  return host.length() > 0 &&
         (origin == "http://" + host || origin == "https://" + host);
}

bool refuseCrossSite() {
  if (sameOriginRequest()) return false;
  Serial.printf("Refused a cross-site request from %s\n",
                boardServer.header("Origin").c_str());
  boardServer.send(403, "application/json",
                   "{\"ok\":false,\"error\":\"Cross-site request refused.\"}");
  return true;
}

void handleControlSet() {
  if (refuseCrossSite()) return;
  char typed[4] = "";
  copyUpper(typed, sizeof(typed), boardServer.arg("crs").c_str());
  if (!isKnownCrs(typed)) {
    boardServer.send(400, "application/json",
                     "{\"ok\":false,\"error\":\"No station has that code.\"}");
    return;
  }
  strlcpy(stationCode, typed, sizeof(stationCode));
  preferences.begin("signalboarder", false);
  preferences.putString("station", stationCode);
  preferences.end();
  Serial.printf("Station set from the network: %s (%s)\n", stationCode,
                stationName(stationCode));
  String body = "{\"ok\":true,\"name\":\"";
  body += stationName(stationCode);
  body += "\"}";
  boardServer.send(200, "application/json", body);
  hasCurrentBoard = false;
  refreshDepartures();
}

void handleControlApi() {
  if (refuseCrossSite()) return;
  String url = boardServer.arg("url");
  url.trim();
  // A trailing slash would produce //v1/departures, which some proxies redirect
  // and HTTPClient does not follow by default.
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    boardServer.send(
        400, "application/json",
        "{\"ok\":false,\"error\":\"A server URL starts with http:// or https://.\"}");
    return;
  }
  if (url.length() >= static_cast<int>(sizeof(apiBase))) {
    boardServer.send(400, "application/json",
                     "{\"ok\":false,\"error\":\"That URL is too long.\"}");
    return;
  }
  strlcpy(apiBase, url.c_str(), sizeof(apiBase));
  preferences.begin("signalboarder", false);
  preferences.putString("api_base", apiBase);
  preferences.end();
  Serial.printf("Server set from the network: %s\n", apiBase);
  String body = "{\"ok\":true,\"api\":\"";
  body += apiBase;
  body += "\"}";
  boardServer.send(200, "application/json", body);
  hasCurrentBoard = false;
  refreshDepartures();
}

// Answered first, opened afterwards: the portal takes the network down with it,
// so a reply written after it starts would never reach the browser.
bool portalRequested = false;

void handleControlPortal() {
  if (refuseCrossSite()) return;
  boardServer.send(200, "application/json", "{\"ok\":true}");
  portalRequested = true;
}

void startBoardServer() {
  if (boardServerRunning || WiFi.status() != WL_CONNECTED) return;
  // A name is worth more than an address nobody wrote down.
  if (MDNS.begin("signalboarder")) MDNS.addService("http", "tcp", 80);
  boardServer.on("/", handleControlRoot);
  boardServer.on("/now", handleControlNow);
  boardServer.on("/stations", handleControlSearch);
  // Reading is a GET; changing the board is a POST.
  boardServer.on("/set", HTTP_POST, handleControlSet);
  boardServer.on("/api", HTTP_POST, handleControlApi);
  boardServer.on("/portal", HTTP_POST, handleControlPortal);
  // The page has to look like the board, and a font fetched from the internet
  // would only arrive when the viewer has some. Ten kilobytes, served here.
  boardServer.on("/f/r.woff2", []() {
    boardServer.sendHeader("Cache-Control", "max-age=604800");
    boardServer.send_P(200, "font/woff2",
                       reinterpret_cast<const char *>(kDotMatrixRegular),
                       kDotMatrixRegularLength);
  });
  boardServer.on("/f/b.woff2", []() {
    boardServer.sendHeader("Cache-Control", "max-age=604800");
    boardServer.send_P(200, "font/woff2",
                       reinterpret_cast<const char *>(kDotMatrixBold),
                       kDotMatrixBoldLength);
  });
  // The web server drops unlisted headers unless it is told to keep them.
  const char *wanted[] = {"Origin", "Host"};
  boardServer.collectHeaders(wanted, 2);
  boardServer.begin();
  boardServerRunning = true;
  Serial.printf("Board page at http://signalboarder.local or http://%s\n",
                WiFi.localIP().toString().c_str());
}

// The setup portal wants port 80 for itself.
void stopBoardServer() {
  if (!boardServerRunning) return;
  boardServer.stop();
  MDNS.end();
  boardServerRunning = false;
}

bool configureNetwork(bool forcePortal) {
  stopBoardServer();
  shouldSaveConfig = false;
  preferences.begin("signalboarder", false);
  const bool hasBoardConfig = preferences.isKey("station");
  if (preferences.isKey("station")) {
    preferences.getString("station", stationCode, sizeof(stationCode));
  }
  if (preferences.isKey("api_base")) {
    preferences.getString("api_base", apiBase, sizeof(apiBase));
  }
  // A reflash does not clear Preferences. Move boards provisioned before the
  // clean-cut rename onto the unified host without making their owners reopen
  // the portal. Never touch a self-hosted origin.
  if (strcmp(apiBase, kLegacyApiUrl) == 0 ||
      strcmp(apiBase, kLegacySignalboarderApiUrl) == 0) {
    strlcpy(apiBase, kHostedApiUrl, sizeof(apiBase));
    preferences.putString("api_base", apiBase);
    Serial.printf("Signalboarder: migrated server to %s\n", apiBase);
  }

  WiFiManager manager;
  activeManager = &manager;
  manager.setClass("invert");
  manager.setTitle("Signalboarder setup");
  manager.setConnectTimeout(20);
  manager.setConfigPortalTimeout(180);
  manager.setSaveConfigCallback(saveConfig);
  manager.setWebServerCallback(bindStationSearch);

  WiFiManagerParameter picker(kStationPickerHtml);
  WiFiManagerParameter station(
      "station", "Station CRS code", stationCode, 3,
      "maxlength='3' autocapitalize='characters' style='text-transform:uppercase'");
  WiFiManagerParameter server(
      "api_base", "Signalboarder server URL", apiBase, sizeof(apiBase) - 1,
      "maxlength='127' placeholder='https://signalboarder.alcun.dev'");
  manager.addParameter(&picker);
  manager.addParameter(&station);
  manager.addParameter(&server);

  // Waveshare's factory demo can leave bsp_esp_demo saved in WiFi NVS. A
  // A factory-fresh board should open its own portal immediately, not spend
  // twenty seconds trying that inaccessible network.
  if (!hasBoardConfig && WiFi.SSID() == "bsp_esp_demo") {
    manager.resetSettings();
  }

  const bool openingPortal = forcePortal || !hasBoardConfig;
  if (booting) {
    showBootLine(openingPortal ? "JOIN SIGNALBOARDER-SETUP" : "JOINING WI-FI");
  } else if (openingPortal) {
    renderStatus("SETUP", "JOIN WI-FI: SIGNALBOARDER-SETUP");
  } else {
    renderStatus("CONNECTING", stationCode);
  }
  const bool connected = forcePortal
      ? manager.startConfigPortal("SIGNALBOARDER-SETUP")
      : manager.autoConnect("SIGNALBOARDER-SETUP");
  if (!connected) {
    preferences.end();
    activeManager = nullptr;
    if (!forcePortal) renderStatus("NO WI-FI", "RESET TO TRY SETUP AGAIN");
    return false;
  }

  if (shouldSaveConfig) {
    // Refuse three letters no station answers to rather than saving them and
    // leaving the board asking for a station that does not exist.
    char typed[4] = "";
    copyUpper(typed, sizeof(typed), station.getValue());
    if (isKnownCrs(typed)) {
      strlcpy(stationCode, typed, sizeof(stationCode));
    } else {
      Serial.printf("Signalboarder: %s is not a station; keeping %s.\n", typed,
                    stationCode);
    }
    strlcpy(apiBase, server.getValue(), sizeof(apiBase));
    // A trailing slash would produce //v1/departures, which some proxies
    // redirect and HTTPClient does not follow by default.
    size_t last = strlen(apiBase);
    while (last > 0 && apiBase[last - 1] == '/') apiBase[--last] = '\0';
    preferences.putString("station", stationCode);
    preferences.putString("api_base", apiBase);
  }
  preferences.end();
  activeManager = nullptr;
  return true;
}

bool readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(kTouchAddress);
  Wire.write(0x02);
  // Two transactions with a stop between them, NOT a repeated start. The
  // repeated-start form sends the read through the core's
  // i2cWriteReadNonStop path, which answers ESP_ERR_INVALID_STATE on this
  // controller and leaves the panel deaf to touch. The idle warnings this was
  // meant to silence are a separate fault and are still open.
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(kTouchAddress, static_cast<uint8_t>(5)) != 5) return false;
  const uint8_t points = Wire.read() & 0x0f;
  if (points == 0) return false;
  const uint8_t xHigh = Wire.read();
  const uint8_t xLow = Wire.read();
  const uint8_t yHigh = Wire.read();
  const uint8_t yLow = Wire.read();

  // Waveshare's example swaps the controller axes and flips panel Y.
  const uint16_t reportedY = ((xHigh & 0x0f) << 8) | xLow;
  const uint16_t reportedX = ((yHigh & 0x0f) << 8) | yLow;
  // Reads off this bus come back corrupted now and again. Clamping such a read
  // put a touch in the corner, which in the editor is the SAVE button, so an
  // out-of-range answer is thrown away rather than pinned to an edge.
  if (reportedX > kWidth || reportedY > kHeight) return false;
  x = reportedX;
  y = kHeight - reportedY;
  return true;
}

void renderTouch(uint16_t x, uint16_t y) {
  renderBoard();
  fillRect(346, 4, 186, 43, kBlack);
  char coordinates[16];
  snprintf(coordinates, sizeof(coordinates), "X%03u Y%03u", x, y);
  drawText(354, 14, coordinates, 2, kRed);
  fillRect(max(0, static_cast<int>(x) - 8), y, 17, 2, kRed);
  fillRect(x, max(0, static_cast<int>(y) - 8), 2, 17, kRed);
}
}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  const uint32_t waitStartedAt = millis();
  while (!Serial && millis() - waitStartedAt < 3000) delay(10);

  Serial.println("\nSignalboarder device");

  Serial.println("Initializing AMOLED display...");
  framebuffer = static_cast<uint16_t *>(heap_caps_malloc(
      kWidth * kHeight * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!framebuffer) {
    Serial.println("ERROR: framebuffer allocation failed.");
    return;
  }
  if (!initDisplay()) {
    Serial.println("ERROR: display initialization failed.");
    return;
  }
  renderBootAnimation();
  // A failed connect is no longer the end of setup. The touch panel and the
  // button are brought up either way, and the loop keeps asking for the
  // network, so a board that outlived its router recovers on its own.
  if (configureNetwork()) {
    Serial.printf("WiFi connected: %s, IP %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    Serial.printf("Station: %s, server: %s\n",
                  stationCode, apiBase[0] ? apiBase : "NOT SET");
    onNetworkUp();
  } else {
    lastReconnectAt = millis();
    booting = false;
    renderStatus("NO WI-FI", "RETRYING EVERY 30 SECONDS");
  }

  // BOOT is the way into setup on a board with no touch panel.
  pinMode(kSetupButton, INPUT_PULLUP);
  Serial.printf("Setup button idles %s.\n",
                digitalRead(kSetupButton) == LOW ? "LOW: no usable button"
                                                 : "HIGH");

  Wire.begin(kTouchSda, kTouchScl, 300000);
  pinMode(kTouchInterrupt, INPUT_PULLUP);
  Wire.beginTransmission(kTouchAddress);
  Wire.write(0x00);
  Wire.write(0x00);
  if (Wire.endTransmission() == 0) {
    Serial.println("FT3168 touch controller ready.");
  } else {
    Serial.println("ERROR: FT3168 touch controller not found.");
  }

  // A board flashed before the station list, or saved by an older firmware,
  // can be holding three letters no station answers to. Say so and offer the
  // way out rather than looping on a request that can never succeed.
  if (!isKnownCrs(stationCode)) {
    Serial.printf("Signalboarder: saved station %s is not a station.\n",
                  stationCode);
    openStationEditor();
  }
}

void loop() {
  // Keep asking for the saved network. WiFi.begin() with no arguments reuses
  // the credentials already in NVS, so this never opens a portal and never
  // blocks: the panel stays live and the touch gesture still works while the
  // board waits for its router.
  if (WiFi.status() != WL_CONNECTED) {
    if (networkReady) {
      networkReady = false;
      stopBoardServer();
      Serial.println("Wi-Fi: lost the network.");
    }
    if (millis() - lastReconnectAt >= kReconnectEveryMs) {
      lastReconnectAt = millis();
      Serial.println("Wi-Fi: retrying the saved network.");
      WiFi.mode(WIFI_STA);
      WiFi.begin();
    }
  } else if (!networkReady) {
    Serial.printf("Wi-Fi: joined %s as %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    onNetworkUp();
  }

  // Answer the network first: a page that only responds between touch polls
  // feels broken.
  startBoardServer();
  if (boardServerRunning) boardServer.handleClient();
  if (portalRequested) {
    portalRequested = false;
    Serial.println("Setup portal requested from the network.");
    if (configureNetwork(true)) {
      hasCurrentBoard = false;
      refreshDepartures();
    }
    return;
  }

  const uint32_t touchPollMs =
      uiMode == UiMode::Board ? kIdleTouchPollMs : kEditorTouchPollMs;
  if (millis() - lastTouchPollAt < touchPollMs) {
    if (uiMode == UiMode::Board &&
        millis() - lastRefreshAt >= kRefreshIntervalMs) {
      refreshDepartures();
    }
    delay(10);
    return;
  }
  lastTouchPollAt = millis();

  uint16_t x = 0;
  uint16_t y = 0;
  // TP_INT reads high throughout a press on this panel, so it cannot gate the
  // read: gated on it, the board answered no touch at all.
  const bool touched = readTouch(x, y);
  if (touched) {
    lastTouchX = x;
    lastTouchY = y;
    lastTouchSeenAt = millis();
  }
  const bool touchLive =
      lastTouchSeenAt != 0 && millis() - lastTouchSeenAt < kTouchGraceMs;
  if (uiMode == UiMode::StationEditor) {
    if (editorAwaitingRelease) {
      if (!touchLive) editorAwaitingRelease = false;
      touchWasLive = touchLive;
      delay(10);
      return;
    }
    if (touched && !touchWasLive) {
      editorHoldStartedAt = millis();
      lastSpinAt = millis();
      handleEditorTap(lastTouchX, lastTouchY);
    } else if (touchLive) {
      handleEditorHold(lastTouchX, lastTouchY);
    } else {
      editorHoldStartedAt = 0;
    }
    touchWasLive = touchLive;
    delay(10);
    return;
  }

  const uint32_t boardFrameInterval = currentBoard.services[0].callingAt[0]
      ? 125
      : 1000;
  if (hasCurrentBoard && millis() - lastBoardFrameAt >= boardFrameInterval) {
    lastBoardFrameAt = millis();
    renderDepartures(currentBoard, currentBoardStale);
  }

  // Hold BOOT for the same result the touch gesture gives: a way into setup
  // that does not need the panel. It counts only from a release, so a pin held
  // low by the board itself never opens anything.
  const bool buttonDown = digitalRead(kSetupButton) == LOW;
  if (!buttonDown) setupButtonArmed = true;
  const bool setupHeld = setupButtonArmed && buttonDown && !touchLive;
  if (setupHeld) {
    if (buttonHoldStartedAt == 0) buttonHoldStartedAt = millis();
    if (millis() - buttonHoldStartedAt >= kSetupHoldMs) {
      buttonHoldStartedAt = 0;
      setupButtonArmed = false;
      Serial.println("Setup button held: opening the portal.");
      if (configureNetwork(true)) {
        hasCurrentBoard = false;
        refreshDepartures();
      } else if (hasCurrentBoard) {
        renderDepartures(currentBoard, true);
      }
      return;
    }
  } else {
    buttonHoldStartedAt = 0;
  }

  if (touched && !touchWasLive) {
    Serial.printf("Touch %u,%u.\n", lastTouchX, lastTouchY);
  }

  const bool stationTouched = touchLive && lastTouchX < 350 && lastTouchY < 58;
  if (stationTouched) {
    if (setupHoldStartedAt == 0) setupHoldStartedAt = millis();
    if (!setupGestureHandled &&
        millis() - setupHoldStartedAt >= kSetupHoldMs) {
      setupGestureHandled = true;
      openStationEditor();
    }
  } else {
    setupHoldStartedAt = 0;
    setupGestureHandled = false;
  }
  touchWasLive = touchLive;

  if (millis() - lastRefreshAt >= kRefreshIntervalMs) refreshDepartures();
  delay(10);
}
