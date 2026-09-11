# Firmware development

## Hardware

The firmware targets the Waveshare ESP32-S3-AMOLED-1.91 with a 536 × 240
landscape display and FT3168 touch controller. The PlatformIO environment uses
16 MB flash and 8 MB PSRAM. Display, touch and BOOT pin assignments are in
`src/main.cpp`.

Rendering uses a framebuffer and the ESP32 LCD interface directly, without
LVGL. Other displays require changes to the pins, initialisation and layout.
The non-touch configuration has not been tested on hardware.

## Departure data

The device requests a Signalboarder server, not the National Rail provider.
`src/departures.h` defines the local representation: a station name and up to
three services with scheduled time, expected time, destination, platform,
calling points and disruption status.

The parser reads normalised API fields. It uppercases display text and joins
calling-point names into a scrolling line. An empty service list is valid;
raw provider JSON is rejected. JSON property order is not significant.

Server fixtures test provider input. Device fixtures test normalised output.
Run both suites when changing the shared response format.

## Configuration

Wi-Fi, station and server settings are stored in non-volatile memory.
The local web page and setup portal serve their own station data and fonts.
Station selection therefore works without internet access.

Settings changes use POST requests. Browser requests with an Origin header
must match the device's host. The local page has no authentication; devices
on the same network can change settings.

The firmware retries saved Wi-Fi every thirty seconds. The setup portal opens
when the device cannot join its network and can also be opened with BOOT.

## Station data

Generate the station header from the browser repository's dataset:

```sh
python3 scripts/generate_stations.py ../signalboarder/web/public/stations.json \
  > src/stations_data.h
```

Regenerate after updating the browser station list. Keep the source attribution
and licence notice in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Known limitations

- Display positions are fixed for the supported resolution.
- The touch controller may emit an idle I²C warning or fail its initial probe
  while subsequent touch reads still work.
- Other panels and the non-touch path require hardware validation.

Build and host-test commands are in [README.md](README.md).
