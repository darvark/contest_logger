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

Use Visual Studio 2022 C++ tools, Qt 6 (MSVC 2022 64-bit), CMake, and vcpkg.
Install yaml-cpp and configure/build/test/package from PowerShell:

```powershell
vcpkg install yaml-cpp:x64-windows-static
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DLOGGER_ENABLE_HAMLIB=OFF `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix package
windeployqt --release --compiler-runtime package\bin\contest_logger.exe
Compress-Archive -Path package\* -DestinationPath ContestLogger-windows-x64.zip
```

The GitHub Actions workflow runs the same Linux/Windows build and test matrix,
publishes test results, and stores installable archives as workflow artifacts.
The Windows CI profile omits Hamlib because a supported prebuilt Hamlib runtime
is not part of the project dependency setup; hardware CAT on Windows still
requires a matching Hamlib build and deployment of its runtime DLLs.

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
`callsign`, and `enum`; supported multiplier types are `exchange` and `prefix`.

The loader rejects malformed YAML, unknown keys, duplicate fields, invalid
regular expressions, unsupported field types, and values outside configured
limits. Start with one of the bundled definitions and run
`ctest --test-dir build --output-on-failure` after changing contest rules.
Prefix/DXCC resolution is not inferred automatically; callers must provide
resolved multiplier values.

Bundled starter profiles cover CQ WW CW/SSB, CQ WPX CW/SSB, IARU HF, SP DX,
WAG, ARRL DX CW/SSB/10m, EU DX, and EU HF Championship. They are not certified
implementations of every sponsor rule: conditional exchanges, country scoring,
and annual rule changes require operator review. Enter station-specific sent
exchange values in the QSO form; they persist per contest and entry context.
Standard RST values follow the selected mode (voice 59, CW/digital 599).

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
Call history supports callsign completion and exchange suggestions; CTY prefix
matches show country and zones as hints only, not automatic DXCC scoring.

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
and results for any hardware-specific issue.
