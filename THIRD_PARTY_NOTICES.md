# Third-party notices

Signalboarder's firmware is MIT (`LICENSE`). It builds against the following,
which keep their own terms.

## Libraries

| Component | Version | Licence |
|---|---|---|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.2 | MIT |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | 2.0.17 | MIT |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) (via pioarduino) | platform 55.03.37 | LGPL-2.1 (core), Apache-2.0 (ESP-IDF components) |

## Panel driver

The QSPI initialisation sequence for the SH8601 follows Espressif's
Apache-2.0 `esp_lcd_sh8601` component, and the panel timings follow Waveshare's
MIT ESP32-S3-AMOLED-1.91 example (Copyright (c) 2024 Waveshare). Both are
credited in `src/main.cpp` at the code they informed.

## Typeface

The panel's glyph tables in `src/dot_matrix_font.h` and the woff2 subsets in
`src/web_font.h`, which the board serves to its own page, both come from Daniel
Hart's [Dot Matrix Typeface](https://github.com/DanielHartUK/Dot-Matrix-Typeface),
pinned at `9d6d28877b368023ead04139be9e4e2e9172dfab` and licensed by its author
under the SIL Open Font License 1.1. The complete text is in `OFL.txt`. Upstream
SHA-256:

    230b56470879bf6eebeed29127d22594d923a9a93fd598a25044da976914a596  Dot Matrix Regular.ttf
    318c30a32b6365234902803104db833b683c0d93f1fe678a45148d9c5b3249b4  Dot Matrix Bold Tall.ttf

The glyph tables and woff2 files are modified subsets. The upstream project
declares no Reserved Font Names. Retain this notice and `OFL.txt` when redistributing these assets.

## Station list

`src/stations_data.h` is generated from the web board's
`web/public/stations.json`, which is built from
[Dav Wheat's uk-railway-stations](https://github.com/davwheat/uk-railway-stations)
and [Trainline EU's stations](https://github.com/trainline-eu/stations). Both
are licensed under the
[Open Database License](https://opendatacommons.org/licenses/odbl/1-0/).

The generated station database is distributed under the ODbL with attribution
to these sources. Retain the attribution and licence when redistributing it.

## Data

Departure data comes from National Rail via a Signalboarder server. It is not
redistributed here and is not covered by this licence. See the server
repository, and the terms of your own Rail Data Marketplace subscription.
