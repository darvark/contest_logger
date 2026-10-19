# Building, exporting, and recovering logs

## Linux build

On Ubuntu, install the build dependencies:

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-tools-dev \
  libqt6sql6-sqlite libyaml-cpp-dev libhamlib-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix package
```

Hamlib is optional. To build explicitly without it:

```sh
cmake -S . -B build-nohamlib -DLOGGER_ENABLE_HAMLIB=OFF
```

The `package` CMake target creates a `.tar.gz` archive. It contains the
application and example contest YAML files; Qt, SQLite, and yaml-cpp runtime
libraries must be installed by the Linux package manager.

The radio integration tests use Hamlib's Dummy rig (model 1), including a local
`rigctld` process for the TCP transport test (NET rigctl model 2). They require
Hamlib at build time; the `rigctld` test is skipped when the utility is not on
`PATH`, and both integration tests are skipped in builds without Hamlib.

## Windows build and package

Use Visual Studio 2022 x64 C++ tools, Qt 6 (MSVC 2022 64-bit), CMake, and
vcpkg. Hamlib's vcpkg port does not support the MSVC `x64-windows` triplet, so
obtain a Windows Hamlib package containing its headers, DLL, and `.def` file.
The script creates an MSVC import library from that definition file, installs
`yaml-cpp` with vcpkg, requires Hamlib at configure time, builds/tests, deploys
Qt and runtime DLL dependencies, checks the Hamlib DLL, and creates the ZIP:

```powershell
.\tools\package-windows.ps1 `
  -VcpkgRoot "$env:VCPKG_ROOT" `
  -HamlibRoot "C:\Hamlib" `
  -QtRoot "C:\Qt\6.8.0\msvc2022_64"
```

For a build without Hamlib, set `LOGGER_ENABLE_HAMLIB=OFF` and leave
`LOGGER_REQUIRE_HAMLIB=OFF`. Release packaging must use the script above:
`LOGGER_REQUIRE_HAMLIB=ON` prevents a silently degraded build, and the installed
package is checked for the Hamlib runtime DLL. Run the package on the actual
target Windows machine before release; successful compilation/package creation
does not verify COM-port access or radio firmware compatibility.

## Hamlib CAT configuration

1. Install Hamlib development/runtime packages and enable it with
   `-DLOGGER_ENABLE_HAMLIB=ON`. CMake detects Hamlib when its headers and library
   are on the standard search path; otherwise set `HAMLIB_INCLUDE_DIR` and
   `HAMLIB_LIBRARY`.
2. Find the radio's numeric Hamlib model ID in the Hamlib rig list/documentation.
   Select **Radio > Radio A/B CAT settings...**, enter that ID, and choose
   either the serial device plus baud rate or a `rigctld` TCP endpoint.
3. On Linux, grant the operator access to the serial device (commonly through
   the `dialout` group, then log in again). On Windows, choose the assigned
   `COM` port. Do not open a serial port directly in another application while
   the logger owns it.
4. To share a rig connection with another application or a Hamlib CW provider,
   run `rigctld` and point the other client at its host/port rather than opening
   the same serial port twice. Restrict the network listener to trusted/local
   clients.
5. Connect and confirm the radio status/frequency before enabling SO2V/SO2R.
   The Mock backend supports software testing without a transceiver. A successful
   Hamlib build is not evidence of hardware compatibility; test CAT, CW, and
   dual-receiver behavior with the exact radio/firmware/interface in use.

## Contest definitions

Contest definitions use YAML schema version 1. Import them using **Contest >
Import YAML definition...** or place examples under `contests/examples/` before
building. Definitions specify contest ID/name, supported bands/modes,
sent/received exchange fields, serial generation, QSO points, multipliers, and
the duplicate key. Field types are `rst`, `serial`, `integer`, `text`, `grid`,
`callsign`, and `enum`; supported multiplier types are `exchange`, `prefix`,
`dxcc`, and `country`. Exchange multipliers use a received field; prefix
multipliers use the WPX prefix derived from the contacted callsign. DXCC and
country multipliers are resolved from the contact's callsign (`country` is an
alias for the CTY DXCC entity). Multipliers can be counted per band and/or mode
with `per_band` and `per_mode`.

The loader rejects malformed YAML, unknown keys, duplicate fields, invalid
regular expressions, unsupported field types, and values outside configured
limits. Conditional received fields and ordered `qso_point_rules` can use band,
mode, received exchange values, or CTY metadata. Sent exchange fields can also
be conditional on the station's CTY location. Available location facts include
`dxcc`/`country`, `continent`, `cq_zone`, `itu_zone`, corresponding
`station.*` facts, and `same_dxcc`, `same_country`, and `same_continent`
comparisons. The application resolves contact and station location facts when
a valid `wl_cty.dat` is loaded; this data is required for geographical
conditions and DXCC/country multipliers. Without it, dependent conditions may
not match and CTY-based multiplier values remain unresolved. `qso_point_rules`
are evaluated in order; the first matching rule supplies QSO points, otherwise
`qso_points` is used. See [CONTEST_DEFINITIONS.md](./CONTEST_DEFINITIONS.md)
for the condition syntax. Start with one of the bundled definitions and run
`ctest --test-dir build --output-on-failure` after changing contest rules.

Bundled starter profiles cover CQ WW CW/SSB, CQ WPX CW/SSB, IARU HF, SP DX,
WAG, ARRL DX CW/SSB/10m, EU DX, and EU HF Championship. They are not certified
implementations of every sponsor rule; verify profile conditions, point
values, and annual rule changes against the current sponsor rules. Enter
station-specific sent exchange values in the QSO form; they persist per contest
and entry context. Standard RST values follow the selected mode (voice 59,
CW/digital 599).

## Main window layout

The operator entry form stays in the center, with the contest log docked below
and the DX bandmap, CW keyer, and contest summary available as tabs on the
right. Use the **Window** menu to show/hide panels or restore the default
arrangement. The window geometry and dock layout are persisted in `QSettings`.
Radio CAT, DX cluster, and CW keyer configuration are available from **Radio**;
operator/station settings are under **Options**.

## Callsign support data and keyboard operation

**File > Import support data** accepts call-history CSV/TXT, `master.cpy`, and
standard multiline `wl_cty.dat`. The app validates each imported file, stores
its path in application settings, and reloads the selected data at startup.
Call history supports callsign completion and exchange suggestions. CTY
prefix/exact-call matches provide country (DXCC entity), continent, and CQ/ITU
zones for call hints, contest scoring conditions, and DXCC/country multipliers;
without CTY data, these location-dependent scoring values cannot be resolved.

Use a call-history header beginning `CALL` or `CALLSIGN` with columns such as
`NAME`, `QTH`, `STATE`, `GRID`, `SECTION`, `SERIAL`, `REPORT`, and `EXCHANGE`.
Headerless whitespace rows are interpreted as `CALL NAME QTH EXCHANGE`.
`master.cpy` accepts one callsign per line with optional trailing columns.
In the bandmap, **Up/Down** selects a spot and **Enter** tunes it. Space moves
through callsign and received-exchange fields and cycles back to callsign.

## Export validation and formats

Select a contest and use **File > Export Cabrillo...** or **File > Export
ADIF...**. Before writing, the exporter validates the selected contest, calls,
UTC timestamps, band/frequency, supported mode, duplicate keys, and configured
exchange fields. Failed validation is shown to the operator and does not
replace an existing file. Successful exports are written through a temporary
file and atomically committed.

Cabrillo output uses the v3 record layout and maps common modes (for example
USB/LSB/SSB to `PH`, RTTY to `RY`, and FT8/FT4/data to `DG`). When a contact
has no CAT frequency, its band designator is used. Cabrillo contest-specific
category headers and each contest sponsor's submission rules should be checked
before submitting the file. ADIF output includes standard contact/time/band/
mode/frequency and sent/received exchange fields; frequencies are omitted when
not recorded. Older QSO records that only contain plain exchange strings are
mapped to contest fields in definition order; strings in `field=value` form
are also recognized. This allows validation and Cabrillo output to use the
original exchange without requiring a database migration.

## SQLite backups and recovery

The database uses SQLite WAL journaling, `synchronous=FULL`, a busy timeout,
and a startup `quick_check`. SQLite replays a valid WAL after an interrupted
process. A corrupt database is not silently replaced: startup reports the
failure.

Use **File > Back up database...** to create and integrity-check an online
snapshot. The application also creates one verified automatic snapshot per
day, retaining the most recent 30 daily backups, under Qt's application data
directory in `backups/`.

To restore a backup, close Contest Logger and run:

```text
contest_logger --restore-backup <path-to-backup.sqlite>
```

The application checks SQLite integrity and the logger schema before replacing
the database. It preserves the prior database (and any WAL/SHM files) beside
the restored database with a `.before-restore-...sqlite` suffix. The restore
command is blocked while another logger instance is running. Restart the
application normally after the success message.

## Hardware integration matrix

CI verifies builds/tests on Linux and Windows, including mock CAT, CW keyer,
and local TCP/Telnet cluster tests. It does not emulate a physical serial port,
radio firmware, Windows sleep/resume behavior, or external PTT/audio switching.
Before field use, test each supported platform with the target transceiver and
USB/serial adapter, both direct CAT and `rigctld`, cluster loss/reconnect, and
system suspend/resume. Record radio model, firmware, Hamlib version, transport,
and results for any hardware-specific issue. Software-side SO2V routing binds
each queued CW message to its selected transmit VFO; Hamlib receives explicit
VFO A/B, while cwdaemon refuses VFO B because its protocol cannot select the
transmit VFO. This does not replace an RF-safety test on the target radio.

Use this record for every supported radio/platform combination; do not mark a
combination supported based only on Hamlib Dummy or `rigctld`:

| Platform / version | Radio / firmware | CAT transport / Hamlib | CAT tune & mode | SO2V focus-to-TX A/B | CW provider / VFO | Disconnect / reconnect | Suspend / resume | Result / evidence |
|---|---|---|---|---|---|---|---|---|
| Pending | Pending | Pending | Pending | Pending | Pending | Pending | Pending | Pending |

For SO2V, verify independently that switching RX focus changes the radio's TX
VFO, that a queued message remains on the VFO selected when it was queued, and
that abort/disconnect never redirects transmission to the other VFO. Record
actual RF/PTT observation and test setup. Mark failures and unsupported
combinations explicitly rather than inferring compatibility from another
model using the same Hamlib backend.
