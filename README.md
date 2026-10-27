# Contest Logger

Wieloplatformowy logger zawodów krótkofalarskich dla systemów Linux i Windows,
napisany w C++20 i Qt 6. Obsługuje lokalne logi SQLite, definicje zawodów YAML,
walidację wymiany, sprawdzanie duplikatów, punktację i mnożniki, wprowadzanie
QSO z klawiatury, łączność CAT z radiem oraz opcjonalną synchronizację
centralnego logu przez niezabezpieczony TCP w zaufanej sieci LAN/VPN.

**[Instrukcja użytkownika](docs/USER_GUIDE.md)** zawiera zrzuty ekranu, opis
konfiguracji, skróty klawiszowe i znane ograniczenia. **[Architektura
aplikacji](docs/ARCHITECTURE.md)** opisuje moduły i ich zależności. Przegląd
zaimplementowanych funkcji znajduje się w [FUNKCJONALNOSCI.md](FUNKCJONALNOSCI.md).

## Wymagania

- CMake 3.21 lub nowszy i kompilator C++20.
- Qt 6.2 lub nowszy: Core, Widgets, Sql, Network, SerialPort, LinguistTools
  oraz Test (moduł Test jest potrzebny do budowania testów).
- Wtyczka sterownika SQLite dla Qt (`QSQLITE`).
- yaml-cpp 0.7 lub nowszy.
- Biblioteka deweloperska Hamlib do sprzętowej obsługi CAT i CW (opcjonalna
  podczas budowania; bez niej backend Hamlib jest niedostępny).

## Budowanie i testy

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Przy pierwszym uruchomieniu aplikacja tworzy katalog danych wyznaczony przez
Qt. Znajdują się w nim baza `contest-logger.sqlite`, konfiguracja
`config/ContestLogger/ContestLogger.ini` (obsługiwana przez `QSettings`) oraz
katalog `contests/` z kopiami wbudowanych definicji YAML. Domyślny katalog w
systemie Linux to `~/.local/share/ContestLogger/ContestLogger/`, a w Windows:
`%APPDATA%\ContestLogger\ContestLogger\`. Istniejące ustawienia są jednorazowo
kopiowane do pliku INI, bez usuwania oryginału. Brakujące wbudowane definicje
są odtwarzane przy uruchomieniu; istniejące pliki nie są nadpisywane.

Jeśli Hamlib znajduje się w niestandardowym katalogu, podczas konfiguracji
CMake podaj `HAMLIB_INCLUDE_DIR` i `HAMLIB_LIBRARY`. Szczegółowe instrukcje
budowania i pakowania dla Linuxa i Windows znajdują się w
[docs/BUILD_AND_RELEASE.md](docs/BUILD_AND_RELEASE.md).

## Definicje zawodów YAML

Definicje używają schematu w wersji 1. Nieznane klucze, błędny YAML,
niepoprawne wyrażenia regularne, powtórzone pola oraz nieobsługiwane typy
wymiany lub mnożników są odrzucane z komunikatem wskazującym źródło błędu.

```yaml
schema_version: 1
id: serial-sprint
name: Serial Sprint
description: Zawody CW z automatycznie nadawanym numerem seryjnym.

bands: [80m, 40m, 20m]
modes: [CW]

exchange:
  sent:
    - name: report
      label: Raport sygnału
      type: rst
      default: "599"
    - name: serial
      label: Numer seryjny
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

Typy pól wymiany to `rst`, `serial`, `integer`, `text`, `grid`, `callsign`
i `enum`. Pola mogą określać m.in. `required`, `generated`, `default`,
`pattern`, `width`, `min` i `max`. Mnożniki mogą być oparte na polu wymiany,
prefiksie WPX lub danych CTY: `exchange`, `prefix`, `dxcc`, `cq_zone`,
`itu_zone` i `continent`. Mnożniki zależne od CTY wymagają wczytanego pliku
`wl_cty.dat`. Numeracja seryjna rozpoczyna się od większej z wartości:
`serial_start` lub najwyższego numeru zachowanego w logu.

Dołączone definicje są profilami startowymi, a nie certyfikowanymi
implementacjami wszystkich regulaminów. Przed użyciem należy sprawdzić
punktację, warunki i aktualne zasady organizatora. Pełny opis składni znajduje
się w [docs/CONTEST_DEFINITIONS.md](docs/CONTEST_DEFINITIONS.md).

## Dane pomocnicze

Menu **File > Support data** służy do wczytywania historii znaków w CSV/TXT,
`master.cpy` i standardowego `wl_cty.dat`. Pliki są walidowane, ich ścieżki
zapisywane w konfiguracji, a dane ponownie wczytywane przy starcie. Historia
znaków uzupełnia podpowiedzi wymiany, a dane CTY dostarczają kraju, DXCC,
kontynentu i stref CQ/ITU.

## Funkcje i ograniczenia

- Logowanie i edycja QSO, walidacja wymiany, duplikaty, numery seryjne,
  punktacja i mnożniki.
- CAT przez Hamlib lub backend Mock, tryby SO2V/SO2R w zakresie opisanym
  w instrukcji, keyer CW Hamlib/cwdaemon/Mock oraz klient DX Cluster.
- Eksport Cabrillo 3.0 i ADIF, kopie zapasowe SQLite oraz import logów.
- Opcjonalna synchronizacja centralna przez trusted TCP bez TLS i logowania;
  każda stacja ma własną bazę SQLite. Nie należy współdzielić pliku bazy przez
  sieć.

Trusted TCP nie szyfruje ruchu i nie uwierzytelnia klientów. Każda stacja,
która może połączyć się z serwerem, otrzymuje dostęp do odczytu i zapisu;
zaufanie do LAN/VPN oraz ograniczenia firewall stanowią jedyną granicę dostępu.
Nie wystawiaj portu serwera do Internetu. Synchronizacja sieciowa nie jest
zatwierdzona do pracy w zawodach. Przed wdrożeniem potrzebne są pełne testy
awarii, próba odtworzenia kopii w środowisku docelowym i pilotaż wielostacyjny.
Szczegóły opisują
[NETWORKING.md](NETWORKING.md) i
[docs/NETWORKING_OPERATIONS.md](docs/NETWORKING_OPERATIONS.md).

## Dokumentacja

- [Instrukcja użytkownika](docs/USER_GUIDE.md) — konfiguracja, praca z logiem,
  radiem, CW, DX Clusterem, eksportem i kopiami zapasowymi.
- [Architektura aplikacji](docs/ARCHITECTURE.md) — diagramy architektury
  i zależności modułów.
- [Budowanie i wydawanie](docs/BUILD_AND_RELEASE.md) — kompilacja, pakiety,
  Hamlib, eksport, odzyskiwanie danych i testy sprzętowe.
- [Definicje zawodów](docs/CONTEST_DEFINITIONS.md) — składnia YAML.
- [Operacje serwera synchronizacji](docs/NETWORKING_OPERATIONS.md) — trusted
  TCP, ograniczenia sieci/firewall, backup i odzyskiwanie serwera.
- [Funkcje programu](FUNKCJONALNOSCI.md) — zestawienie zaimplementowanych
  możliwości.
