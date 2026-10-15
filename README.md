# Contest Logger

Cross-platform C++20/Qt 6 starting point for an amateur radio contest logger.
It provides the domain model, SQLite persistence with schema migrations,
operator settings, YAML contest definitions, exchange validation, dupe checks,
serial numbering, QSO points, multiplier tracking, live QSO entry, and
asynchronous CAT radio monitoring.

## Requirements

- CMake 3.21 or newer
- C++20 compiler
- Qt 6.2 or newer: Core, Widgets, Sql, and Test
- Qt's SQLite driver plugin (`QSQLITE`)
- yaml-cpp 0.7 or newer
- Hamlib development library for hardware CAT support (optional at build time;
  without it, the application reports that the backend is unsupported)

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The application stores its SQLite database in the platform-specific application
data directory reported by Qt. Operator preferences are stored using `QSettings`.
If Hamlib is installed in a non-standard location, set `HAMLIB_INCLUDE_DIR` and
`HAMLIB_LIBRARY` when configuring CMake.

## Contest-definition YAML

Definitions use schema version 1. Unknown keys, malformed YAML, invalid
regular expressions, duplicate fields, and unsupported exchange/multiplier
types are rejected with a source-qualified diagnostic.

```yaml
schema_version: 1
id: serial-sprint
name: Serial Sprint
description: CW contest with a generated serial exchange.

bands: [80m, 40m, 20m]
modes: [CW]

exchange:
  sent:
    - name: report
      label: Signal report
      type: rst
      default: "599"
    - name: serial
      label: Serial number
      type: serial
      generated: true
      width: 3
  received:
    - name: report
      type: rst
    - name: serial
      type: integer
      min: 1
      max: 9999

scoring:
  qso_points: 2
  serial_start: 1
  multipliers:
    - type: prefix
      per_band: false

dupe_key: [callsign, band, mode]
```

Exchange field types: `rst`, `serial`, `integer`, `text`, `grid`, `callsign`,
and `enum`. Fields support `required`, `generated` (sent serials), `default`,
`pattern` (regular expression), `width`, `min`, and `max`. Enumeration fields
require a non-empty `values` list. Definition files are limited to 1 MiB,
exchange lists to 32 fields, and field widths to 32 characters.
Custom patterns are matched against the whole exchange value.

Multiplier types are `exchange` (requires a received `field`), `prefix` (WPX
rules: portable designators and `/digit` overrides are handled) and the
CTY-resolved types `dxcc`, `cq_zone`, `itu_zone` and `continent`. CTY-resolved
multipliers count only after `wl_cty.dat` is loaded; QSOs whose value cannot be
resolved are listed in the final-score tooltip (marked `*`). Worked multipliers
are rebuilt from the stored log (`ContestEngine::buildContext`/`score`) when
logging, editing and refreshing the summary, with optional per-band scoping.

Generated sent serials continue from the highest serial stored in the log
(never below `serial_start`), so gaps and edited serials are preserved and
imported or older entries do not cause collisions.

Additional bundled starter profiles cover CQ WW CW/SSB, CQ WPX CW/SSB, IARU HF,
SP DX, WAG, ARRL DX CW/SSB/10m, EU DX, and EU HF Championship. They provide
exchange forms and basic scoring only; conditional exchanges, country/prefix
resolution, and edition-specific sponsor rules must be verified before use.
Operator-specific sent exchange values are entered in the QSO form and saved
per contest and entry context. Standard RST defaults follow the selected mode
(voice 59, CW/digital 599).

## Callsign support data

Use **File > Import support data** to load call-history CSV/TXT, `master.cpy`,
and standard `wl_cty.dat`. Imports are validated, file paths are saved in
application settings, and the files are reloaded at startup. Call-history and
master callsigns feed the **Check partial** window (there is no completion
popup in the entry line) and call history suggests matching exchange fields.
CTY prefix matches show country and CQ/ITU zones and are used to resolve
DXCC/zone/continent multipliers.

Call history accepts a header row beginning with `CALL`/`CALLSIGN` and columns
such as `NAME`, `QTH`, `STATE`, `GRID`, `SECTION`, `SERIAL`, `REPORT`, and
`EXCHANGE`. Headerless whitespace rows use `CALL NAME QTH EXCHANGE`. The
master list accepts one callsign per line, optionally followed by other data.
The CTY reader accepts standard multiline CTY.DAT records.

In QSO entry, press **Space** to advance from callsign through received
exchange fields and back to callsign. In the bandmap, use **Up/Down** to select
a spot and **Enter** (or **Tune selected spot**) to populate the active entry
and tune its active radio; double-click remains available.

The project includes 15 definitions in `contests/examples/`, including general
logging, serial/grid examples, and the named contest starter profiles. They
are bundled into the application and installed automatically when missing.
Use **Contest > Import YAML definition...** to add another definition;
selecting a contest previews its bands, modes, exchanges, points, serial start,
multipliers, and duplicate key.

## Current UI and tests

The main window follows the DXLog look and arrangement: a LightSeaGreen log
window in the center with a yellow status line (UTC time, station callsign,
contest, QSO/points/multipliers/score), a monospace log (Nr, UTC, Band, Mode,
kHz, Call, Snt, Rcv, Mult, Pts; new multipliers in red), the QSO-entry panel
below it, an operator message line, and an F1–F8 function-key bar. Red-titled
tool windows surround the log: **Check callsign** (band × mode worked grid),
**Check partial** (white = new, green = worked on another band, red = dupe),
**CW messages / keyer**, **Contest info**, **Bandmap** (sorted by frequency,
nearest spot highlighted, worked spots grey), **Rate**, **Summary** (per-band
totals and final score) and a black **DX Cluster announcements** window.
Windows can be moved, floated, hidden from **Window**, and restored with
**Window > Restore default layout**; geometry and arrangement are saved between
runs. The top menus are **File**, **Contest**, **Operating**, **Radio**,
**Options**, and **Window**. This is a similar operating layout, not a pixel
copy of DXLog.

The entry panel provides callsign/exchange fields and Enter-to-log. The active
panel has a red frame and its header shows VFO/radio, frequency, band and mode.
The badge next to the callsign shows **DUPE**, **NEW MULT** or **NEW**, and the
worked bands for that call. Dupes and validation errors are not shown in modal
dialogs: they appear in the message line, and focus returns to the offending
field. Double-click a log row to edit the contact: the dialog edits the UTC
time, callsign, band/mode (restricted to the contest definition), frequency, and
both sent and received exchange. Changes are validated when saving, duplicates
are checked excluding the edited QSO, errors are listed in the dialog, and
points/exchange text are recalculated. QSO exchange fields are retained as
structured maps in SQLite; older text-only exchanges are migrated.

Keyboard: **F1–F8** send the CW messages of the focused entry panel (CW mode
only; edit them under **Operating > Edit function key**), **Esc** or
**Ctrl+Esc** aborts CW, **Alt+W** wipes the entry, and **Ctrl+Tab** switches
between entry panels A/B.

Radio CAT settings support a Hamlib model ID, serial device and baud rate, or a
TCP/rigctld endpoint. The dialog validates the model, transport and baud rate,
and offers **Test** (a one-off probe reporting frequency, mode, VFO count and
dual-receive capability), **Save & connect**, **Disconnect** and **Save only**.
**Connect automatically at startup** restores the CAT connection on the next
run. Radio operations and one-second polling run on a dedicated
worker thread per radio. The radio frequency selects the entry band. The radio
changes the entry mode only when its own mode changed since the previous poll,
so a mode chosen manually in the form is not overwritten every second. When no
CAT update arrives for more than 5 seconds the entry header shows **CAT STALE**. The Mock radio backend provides independent VFO A
(14.025 MHz) and VFO B (7.025 MHz), and is intended for development and
automated tests without equipment.

SO2V is available only while a connected radio reports two usable VFOs. Enable
**SO2V (dual VFO)** to show independent QSO-entry contexts for VFO A and B;
each context has its own callsign, band, mode, exchange and dupe indicator.
Log a contact with Enter in its callsign field or its **Log** button.
**RX focus A/B** selects the active receiver, updates the focused frequency and
mode display, and is reflected in the radio status. QSO records retain the
frequency of the VFO whose context was logged. A single-VFO radio cannot enable
SO2V. The entry header marks the transmitting context with **◀ TX** (the
focused VFO, or the active radio in SO2R). Radios with two VFOs but without
simultaneous dual receive may still use SO2V; the second context is labelled
**SO2V, no dual RX** and a warning is shown when enabling it.

SO2R uses independent **Radio A** and **Radio B** CAT controllers and settings
(**Radio > Radio A/B CAT settings...**). Enable **SO2R (two radios)** only
after both radios are connected. The two QSO-entry contexts are then routed to
their corresponding radio, and **Focus Radio A/B** selects the active radio
for bandmap tuning. A failure or disconnect on one radio leaves the other
controller running; the disconnected radio's spots can still be copied into
its entry context but will not trigger a tune. Select the **CW radio** explicitly
and configure the keyer for that radio before transmission; changing the
selection invalidates the current CW route until the keyer is reconfigured.
SO2V and SO2R are mutually exclusive. CAT control is not audio switching:
automatic audio routing and hardware transmit/receive switching are not
implemented and require a specific supported external controller.

**Radio > DX cluster...** connects to a TCP/Telnet DX cluster using the
station callsign. The client strips Telnet negotiation, parses standard `DX de`
spot lines (rejecting invalid `HHMMZ` times and dating spots correctly across
midnight), answers the login prompt, records received spots in SQLite (older
spots are pruned automatically), and reconnects with a backoff that grows up to
30 seconds. The bandmap filters by band and hides calls already
worked on that band by default. Double-clicking or pressing Enter on a selected
spot fills the callsign and tunes the active CAT radio (A or B in SO2R) on its
current VFO; Up/Down navigates spots. Spots older than one hour are omitted
from the live bandmap.

The **CW keyer** panel supports a contest-message queue, WPM control and an
abort action (`Esc` / `Ctrl+Esc`). Configure a provider under **Radio > CW keyer...**:
Hamlib (validates `send_morse` support and reports unsupported speed control),
cwdaemon over its UDP protocol, or the mock provider for tests. Hamlib opens its
own configured rig connection; use rigctld if a CAT controller already owns the
radio's serial port. CW message templates accept `{CALL}`, `{MYCALL}`, `{SENT}`
and sent exchange field tokens such as `{SERIAL}`. With cwdaemon (and the mock)
the next queued message is sent only after the keyer confirms the end of
transmission; if no confirmation arrives within the expected time × 3 + 3 s,
the queue is stopped and a warning is shown rather than transmitting blindly.
Hamlib keying is paced by an estimated duration. The CW status line shows
which pacing is active. Abort is sent immediately and clears the remaining queue.

SQLite schema version 5 preserves existing databases; migration 3 adds
structured sent/received exchange columns, migration 4 adds the spot band, and
migration 5 backfills structured exchanges from older text fields and adds
duplicate/spot-time indexes.
Tests cover definition parsing, invalid exchanges, duplicate detection, serial
generation, points, multipliers, database migration and QSO/spot persistence,
mock-radio tuning, spot parsing, malformed input, and cluster reconnect.
Radio tests cover mock CAT connection, independent VFO tuning, RX-focus
switching, independent-radio failure, and polling after a pause. CW tests cover
mock queue cancellation and WPM, cwdaemon UDP commands, and Hamlib detection of
a model without CW text support. Export tests cover Cabrillo/ADIF contents,
validation and preservation of an existing file after invalid export. Radio
integration tests use Hamlib's Dummy model and a local `rigctld` (skipped when
unavailable); cluster tests use a local Telnet simulator. `ui_tests` drives the
main window offscreen: Enter logging, validation errors, dupes, the edit dialog,
table refresh and serial numbering.

**File > Export Cabrillo... / Export ADIF...** validates the selected log before
atomically writing it. **File > Back up database...** writes and verifies a
consistent online SQLite snapshot. Startup creates a daily verified backup and
keeps 30 days of automatic snapshots. SQLite WAL recovery, full synchronous
commits and startup integrity checking support crash recovery; restore a
verified snapshot while the application is closed with
`contest_logger --restore-backup <backup.sqlite>`. The current database is kept
beside the restored copy. See [docs/BUILD_AND_RELEASE.md](./docs/BUILD_AND_RELEASE.md)
for export limitations, platform build/package instructions, Hamlib setup,
YAML contest schema, and the hardware integration test matrix.

GitHub Actions builds and tests Linux and Windows profiles and publishes
platform archives. Windows CI disables Hamlib; packaging Hamlib's Windows
runtime and all hardware-specific testing require a target setup and are
documented separately.
