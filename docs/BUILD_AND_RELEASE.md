# Budowanie, eksport i odzyskiwanie logów

## Budowanie w systemie Linux

W systemie Ubuntu zainstaluj zależności i zbuduj aplikację:

```sh
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-tools-dev qt6-l10n-tools \
  libqt6sql6-sqlite libyaml-cpp-dev libhamlib-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix package
```

Hamlib jest opcjonalna. Aby jawnie zbudować program bez niej:

```sh
cmake -S . -B build-nohamlib -DLOGGER_ENABLE_HAMLIB=OFF
```

Cel CMake `package` tworzy archiwum `.tar.gz` zawierające aplikację
i przykładowe definicje zawodów YAML. Biblioteki wykonawcze Qt, SQLite
i yaml-cpp należy zainstalować za pomocą menedżera pakietów systemu.

Kompilacja katalogu tłumaczeń używa komponentu Qt LinguistTools (Linguist/lrelease).

Testy integracyjne radia używają Hamlib Dummy (model 1), w tym lokalnego
procesu `rigctld` do sprawdzania transportu TCP (model NET rigctl 2). Wymagają
Hamlib podczas budowania. Test `rigctld` jest pomijany, jeśli narzędzie nie
znajduje się w `PATH`; oba testy integracyjne są pomijane w kompilacji bez
Hamlib.

## Pakiety instalacyjne dla Linuxa

Buduj pakiet dla docelowej dystrybucji i architektury. Pakiet DEB z Ubuntu nie
musi być zgodny z Debianem ani innym wydaniem Ubuntu, a pakiet RPM powinien
odpowiadać docelowemu wydaniu Fedory. Skrypt nie wykonuje kompilacji
skrośnej ani nie dołącza bibliotek systemowych. Używaj oddzielnego katalogu
budowania dla każdej dystrybucji.

```sh
# Debian / Ubuntu: instaluje kompilator, CMake, Qt 6, yaml-cpp, Hamlib i narzędzia DEB.
bash tools/package-linux.sh --install-deps
sudo apt install ./packages/deb/contest-logger_*.deb

# Fedora: instaluje odpowiednie zależności budowania i narzędzia RPM.
bash tools/package-linux.sh --install-deps
sudo dnf install ./packages/rpm/contest-logger-*.rpm

# Slackware: najpierw zainstaluj opisane niżej zależności.
bash tools/package-linux.sh
sudo upgradepkg --install-new ./packages/slackware/contest-logger-*.txz
```

Jeśli katalog wyjściowy zawiera kilka wersji, wybierz jedną nazwę pakietu.
APT i DNF pobierają zależności bibliotek z włączonych repozytoriów. Pakiety
wymagają także wtyczki SQLite dla Qt oraz wtyczek GUI/platformy, ładowanych
dynamicznie i niewykrywanych przez skanery zależności ELF. Pakiet zawiera
programy `contest_logger` i `contest_logger_server`, przykładowe definicje
zawodów, dokumentację i licencję. Nie uruchamia automatycznie usługi, nie
tworzy użytkownika systemowego i nie zawiera ustawień ani logów użytkownika.

Budowanie odbywa się z uprawnieniami bieżącego użytkownika. Tylko opcja
`--install-deps` używa `sudo` (lub wykonuje polecenie bezpośrednio, jeśli
proces działa jako root). Bez niej brakujące narzędzia i biblioteki powodują
komunikat o błędzie, a skrypt nie zmienia systemu. Domyślnie Hamlib jest
wymagana, aby pakowanie nie pomijało po cichu sprzętowej obsługi CAT/CW.

Opcje skryptu:

- `--format auto|deb|rpm|slackware` — automatyczny wybór na podstawie
  `/etc/os-release`; jawny format musi pasować do dystrybucji hosta.
- `--no-hamlib` — celowe pominięcie obsługi Hamlib i zależności budowania.
- `--skip-tests` — pominięcie kompilacji i uruchamiania testów (domyślnie
  testy są wykonywane).
- `--build-dir PATH`, `--output-dir PATH` — ścieżki względne względem
  bieżącego katalogu powłoki lub bezwzględne. Domyślnie
  `build-package-<format>` oraz `packages/<format>`.
- `--jobs N` — dodatnia liczba równoległych zadań budowania (domyślnie 2).
- `--maintainer 'Name <email>'` — kontakt dla pakietu DEB/RPM; ustaw go
  przy wydaniu.
- `--help` — wyświetla pomoc.

Slackware wymaga CMake >= 3.21, kompilatora C++20 (`gcc-g++`), `make` (lub
Ninja), Qt >= 6.2 (`qt6`, w tym plików deweloperskich, Qt Test dla domyślnej
konfiguracji testów, wtyczki QSQLITE i wtyczek platformy), yaml-cpp >= 0.7,
plików deweloperskich i wykonawczych Hamlib, GNU tar i xz. Oficjalne pakiety
instaluj z pasującego wydania Slackware przez `slackpkg`; bibliotek
niedostępnych w tym wydaniu niezbędne będzie zbudowanie wraz z ich zależnościami.
Qt 5 dołączone do Slackware 15.0 nie spełnia wymagania Qt 6. Opcja
`--no-hamlib` usuwa zależność od Hamlib.

Pakiet Slackware zawiera `install/slack-desc` oraz informacyjną listę
`install/slack-required`. Polecenia `installpkg` i `upgradepkg` nie rozwiązują
automatycznie zależności z tej listy; trzeba je zainstalować przed
Contest Logger. Opcja `--install-deps` celowo kończy działanie w Slackware,
zamiast sugerować, że zależności można rozwiązać automatycznie.

## Budowanie i pakowanie w Windows

Wymagane są narzędzia C++ x64 z Visual Studio 2022, Qt 6 dla MSVC 2022 64-bit,
CMake i vcpkg. Port Hamlib w vcpkg nie obsługuje tripletu MSVC `x64-windows`.
Pobierz więc pakiet Hamlib dla Windows zawierający nagłówki, bibliotekę DLL
i plik `.def`. Skrypt tworzy bibliotekę importową MSVC na podstawie `.def`,
instaluje yaml-cpp przez vcpkg, wymaga Hamlib podczas konfiguracji, buduje
i testuje program, wdraża Qt oraz biblioteki wykonawcze, sprawdza obecność
DLL Hamlib i tworzy archiwum ZIP:

```powershell
.\tools\package-windows.ps1 `
  -VcpkgRoot "$env:VCPKG_ROOT" `
  -HamlibRoot "C:\Hamlib" `
  -QtRoot "C:\Qt\6.8.0\msvc2022_64"
```

Aby zbudować bez Hamlib, ustaw `LOGGER_ENABLE_HAMLIB=OFF` i pozostaw
`LOGGER_REQUIRE_HAMLIB=OFF`. Pakowanie wydania wykonuj powyższym skryptem:
`LOGGER_REQUIRE_HAMLIB=ON` zapobiega niezauważonemu utworzeniu okrojonej
kompilacji, a zawartość pakietu jest sprawdzana pod kątem biblioteki DLL
Hamlib. Przed wydaniem uruchom pakiet na docelowym komputerze Windows.
Pomyślne budowanie nie potwierdza dostępu do portu COM ani zgodności
z oprogramowaniem układowym radia.

## Konfiguracja CAT przez Hamlib

1. Zainstaluj pliki deweloperskie i wykonawcze Hamlib oraz włącz ją opcją
   `-DLOGGER_ENABLE_HAMLIB=ON`. CMake wykrywa Hamlib, jeśli nagłówki i
   biblioteka znajdują się w standardowej ścieżce wyszukiwania. W przeciwnym
   razie ustaw `HAMLIB_INCLUDE_DIR` i `HAMLIB_LIBRARY`.
2. Znajdź numeryczny identyfikator modelu radia w wykazie modeli Hamlib lub
   dokumentacji. Wybierz **Radio > Radio A/B CAT settings...**, wpisz
   identyfikator i ustaw port szeregowy z prędkością transmisji albo punkt
   końcowy TCP `rigctld`.
3. W systemie Linux przyznaj operatorowi dostęp do urządzenia szeregowego
   (zwykle przez grupę `dialout`, a następnie ponowne zalogowanie). W Windows
   wybierz przypisany port `COM`. Nie otwieraj portu szeregowego w innej
   aplikacji, gdy korzysta z niego logger.
4. Aby współdzielić połączenie z radiem z inną aplikacją lub providerem CW
   Hamlib, uruchom `rigctld` i skieruj pozostałych klientów do jego hosta
   i portu, zamiast otwierać ten sam port szeregowy dwukrotnie. Ogranicz
   nasłuch sieciowy do zaufanych/lokalnych klientów.
5. Połącz radio i sprawdź jego status oraz częstotliwość przed włączeniem
   SO2V/SO2R. Backend Mock pozwala testować program bez transceivera. Udana
   kompilacja z Hamlib nie potwierdza zgodności sprzętowej; CAT, CW i pracę
   z dwoma odbiornikami sprawdź z konkretnym radiem, firmware i interfejsem.

## Definicje zawodów

Definicje zawodów używają schematu YAML w wersji 1. Importuj je przez
**Contest > Import YAML definition...** albo umieść przykłady w
`contests/examples/` przed budowaniem. Definicja określa identyfikator
i nazwę zawodów, dozwolone pasma i emisje, pola wymiany nadawanej
i odbieranej, generowanie numerów seryjnych, punkty za QSO, mnożniki
i klucz wykrywania duplikatów. Typy pól to `rst`, `serial`, `integer`, `text`,
`grid`, `callsign` i `enum`; typy mnożników to `exchange`, `prefix`, `dxcc`
i `country`. Mnożnik wymiany korzysta z pola odebranego, a mnożnik prefiksowy
wyznacza prefiks WPX ze znaku korespondenta. Mnożniki DXCC i `country` są
odczytywane ze znaku korespondenta (`country` jest aliasem encji DXCC z CTY).
Mnożniki można liczyć osobno dla pasma i/lub emisji przez `per_band`
i `per_mode`.

Loader odrzuca błędny YAML, nieznane klucze, powtórzone pola, nieprawidłowe
wyrażenia regularne, nieobsługiwane typy i wartości poza skonfigurowanymi
limitami. Warunkowe pola wymiany odbieranej i uporządkowane
`qso_point_rules` mogą korzystać z pasma, emisji, wartości odebranej wymiany
lub metadanych CTY. Pola wymiany nadawanej mogą również zależeć od położenia
stacji w CTY. Dostępne informacje lokalizacyjne obejmują `dxcc`/`country`,
`continent`, `cq_zone`, `itu_zone`, odpowiadające im pola `station.*` oraz
porównania `same_dxcc`, `same_country` i `same_continent`. Aplikacja
rozpoznaje położenie stacji i korespondenta po wczytaniu prawidłowego pliku
`wl_cty.dat`; plik jest wymagany dla warunków geograficznych i mnożników
DXCC/kraju. Bez niego warunki zależne od lokalizacji mogą nie pasować,
a wartość mnożnika CTY może pozostać nierozpoznana. Reguły
`qso_point_rules` są sprawdzane po kolei; pierwsza pasująca reguła określa
punkty QSO, a gdy żadna nie pasuje, używana jest wartość `qso_points`.
Składnię warunków opisuje [CONTEST_DEFINITIONS.md](./CONTEST_DEFINITIONS.md).
Po zmianie reguł uruchom `ctest --test-dir build --output-on-failure`.

Dołączone profile startowe obejmują CQ WW CW/SSB, CQ WPX CW/SSB, IARU HF,
SP DX, WAG, ARRL DX CW/SSB/10m, EU DX i EU HF Championship. Nie są
certyfikowanymi implementacjami wszystkich reguł organizatorów; przed
zawodami zweryfikuj warunki profilu, punktację i coroczne zmiany regulaminu.
Wartości wymiany nadawanej dla danej stacji wprowadza się w formularzu QSO;
są zapamiętywane dla danego contestu i kontekstu wejściowego. Domyślne
wartości RST zależą od emisji (fonia 59, CW/cyfrowa 599).

## Układ okna głównego

Formularz operatora znajduje się pośrodku, a dziennik zawodów jest
dokowany poniżej. Bandmapa DX, keyer CW i podsumowanie contestu są dostępne
jako karty po prawej stronie. Menu **Window** pozwala pokazywać i ukrywać
panel lub przywrócić układ domyślny. Geometria okna i układ doków są
zapisywane przez `QSettings`. Konfiguracja radia CAT, DX Cluster i keyera CW
znajduje się w menu **Radio**, a ustawienia operatora/stacji — w **Options**.

## Dane pomocnicze znaków i obsługa klawiatury

Polecenie **File > Import support data** przyjmuje historię znaków CSV/TXT,
`master.cpy` i standardowy wielowierszowy `wl_cty.dat`. Aplikacja sprawdza
pliki, zapisuje ich ścieżki w ustawieniach i wczytuje je ponownie przy starcie.
Historia znaków podpowiada znak i wymianę. Dopasowanie prefiksu lub dokładnego
znaku CTY dostarcza kraj (encję DXCC), kontynent i strefy CQ/ITU dla
podpowiedzi, warunków punktacji i mnożników DXCC/kraju. Bez danych CTY nie
można rozstrzygnąć wartości zależnych od lokalizacji.

Nagłówek historii znaków może zaczynać się od `CALL` lub `CALLSIGN`; obsługiwane
kolumny to m.in. `NAME`, `QTH`, `STATE`, `GRID`, `SECTION`, `SERIAL`, `REPORT`
i `EXCHANGE`. Wiersze bez nagłówka rozdzielone białymi znakami mają format
`CALL NAME QTH EXCHANGE`. `master.cpy` zawiera jeden znak w każdym wierszu,
opcjonalnie z dodatkowymi kolumnami. W bandmapie klawisze **Up/Down** wybierają
spot, **Enter** dostraja radio. Spacja przechodzi przez pola znaku i odebranej
wymiany, a następnie wraca do pola znaku.

## Walidacja i formaty eksportu

Wybierz zawody i użyj **File > Export Cabrillo...** lub
**File > Export ADIF...**. Przed zapisem eksporter sprawdza zawody, znaki,
czasy UTC, pasmo/częstotliwość, obsługiwaną emisję, klucze duplikatów
i skonfigurowane pola wymiany. Błąd walidacji jest wyświetlany operatorowi
i nie zastępuje istniejącego pliku. Eksport jest zapisywany do pliku
tymczasowego, a następnie zatwierdzany atomowo.

Cabrillo używa układu rekordów wersji 3 i mapuje popularne emisje (np.
USB/LSB/SSB na `PH`, RTTY na `RY`, FT8/FT4/dane na `DG`). Jeśli QSO nie ma
częstotliwości CAT, używany jest identyfikator pasma. Przed wysłaniem logu
sprawdź nagłówki kategorii właściwe dla contestu i wymagania organizatora.
ADIF zawiera standardowe pola kontaktu, czasu, pasma, emisji, częstotliwości
i wymiany nadawanej/odbieranej; częstotliwość jest pomijana, jeśli jej nie
zapisano. Starsze wpisy QSO zawierające wyłącznie tekst wymiany są mapowane
na pola contestu według kolejności definicji; rozpoznawany jest też format
`field=value`. Dzięki temu można walidować i eksportować oryginalną wymianę
bez migracji bazy.

## Lokalne kopie zapasowe SQLite i odzyskiwanie

Baza używa dziennika SQLite WAL, `synchronous=FULL`, limitu oczekiwania na
blokadę i kontroli `quick_check` podczas uruchamiania. Po przerwaniu działania
SQLite odtwarza poprawny dziennik WAL. Uszkodzona baza nie jest po cichu
zastępowana — aplikacja zgłasza błąd.

Polecenie **File > Back up database...** tworzy i sprawdza spójny zrzut online.
Aplikacja tworzy również jedną zweryfikowaną kopię automatyczną dziennie,
zachowując 30 ostatnich kopii w katalogu danych Qt, w podkatalogu `backups/`.

Aby odtworzyć kopię, zamknij Contest Logger i uruchom:

```text
contest_logger --restore-backup <ścieżka-do-kopii.sqlite>
```

Aplikacja sprawdza integralność SQLite i schemat loggera przed zastąpieniem
bazy. Poprzednia baza (oraz pliki WAL/SHM) jest zachowywana obok z sufiksem
`.before-restore-...sqlite`. Odtwarzanie jest zablokowane, jeśli działa inna
instancja loggera. Po komunikacie o powodzeniu uruchom aplikację ponownie.

## Obsługa centralnego serwera synchronizacji

Pakiet zawiera `contest_logger_server`, ale nie tworzy usługi ani konta
systemowego. Serwer korzysta z osobnego pliku `central-log.sqlite` w katalogu
`--data-dir`; interfejsowy Host i serwer CLI mogą mieć różne domyślne katalogi
danych. Nie używaj lokalnego polecenia odtwarzania aplikacji do bazy serwera.

Gdy serwer jest zatrzymany, odtwórz zweryfikowaną kopię centralną:

```sh
contest_logger_server --restore-backup <kopia.sqlite> --data-dir <katalog-danych-serwera>
contest_logger_server --check --data-dir <katalog-danych-serwera>
```

Serwer zachowuje poprzednią bazę przed instalacją kopii. Automatyczne kopie
przed migracją nie zastępują harmonogramu backupów. Procedura instalacji,
trusted TCP/firewall, kopii i odzyskiwania, aktualizacji oraz utraty hosta
jest opisana w
[NETWORKING_OPERATIONS.md](./NETWORKING_OPERATIONS.md).

Aktualne GUI/CLI nie konfiguruje tokenów ani certyfikatów: połączenia
wykorzystują nieszyfrowany trusted TCP, bez uwierzytelniania klientów.
Automatyczne uruchamianie Clienta/Hosta jest opcjonalne. `station_identity`
przekazuje etykiety nazwy komputera, znaku stacji i operatora; UUID jest
identyfikatorem protokołu, ale w tym trybie nie uwierzytelnia stacji. Wartości
opisowe są deklarowane przez klienta i nie należy traktować ich jako
zweryfikowanej tożsamości.

Przed wdrożeniem uruchom testy `ui_tests` i `sync_server_tests`. Obejmują
tabelę podłączonych stacji, aktualizowanie opisów, starszych klientów,
nieprawidłowe opisy i przepływy synchronizacji. Testy uprawnień read-only
dotyczą zachowanego wariantu TLS/token, a nie bieżącego trusted TCP, w którym
każdy połączony klient ma odczyt i zapis.
Testy automatyczne nie zastępują testów awarii i pilotażu wielostacyjnego;
szczegóły opisuje [NETWORKING.md](../NETWORKING.md).

## Macierz testów sprzętowych

CI buduje i testuje program w Linuxie i Windows, w tym mock CAT, keyer CW
i lokalny test DX Cluster TCP/Telnet. Nie emuluje fizycznego portu szeregowego,
firmware radia, usypiania i wznawiania systemu Windows ani zewnętrznego
przełączania PTT/audio. Przed pracą w terenie sprawdź każdą obsługiwaną
platformę z docelowym transceiverem i adapterem USB/szeregowym, zarówno przy
bezpośrednim CAT, jak i przez `rigctld`; sprawdź także utratę i ponowne
połączenie z klastrem oraz uśpienie/wznowienie systemu. Zapisz model radia,
wersję firmware, Hamlib, transport i wyniki problemów sprzętowych.
Programowe trasowanie SO2V wiąże każdą wiadomość CW w kolejce z wybranym
VFO nadawczym. Hamlib otrzymuje jawne A/B, natomiast cwdaemon odrzuca VFO B,
bo jego protokół nie pozwala wybrać VFO nadawczego. Nie zastępuje to testu
bezpieczeństwa RF na docelowym radiu.

Prowadź poniższy rejestr dla każdej obsługiwanej kombinacji radia i platformy.
Nie oznaczaj kombinacji jako obsługiwanej wyłącznie na podstawie Hamlib Dummy
lub `rigctld`.

| Platforma / wersja | Radio / firmware | Transport CAT / Hamlib | Dostrajanie i tryb CAT | SO2V fokus-TX A/B | Provider CW / VFO | Rozłączenie / ponowne połączenie | Uśpienie / wznowienie | Wynik / dowody |
|---|---|---|---|---|---|---|---|---|
| Do sprawdzenia | Do sprawdzenia | Do sprawdzenia | Do sprawdzenia | Do sprawdzenia | Do sprawdzenia | Do sprawdzenia | Do sprawdzenia | Do sprawdzenia |

Dla SO2V sprawdź osobno, czy zmiana fokusu RX zmienia VFO nadawcze radia,
wiadomość w kolejce pozostaje przypisana do VFO wybranego podczas jej
dodawania oraz czy przerwanie/rozłączenie nigdy nie przekierowuje nadawania
na drugie VFO. Zanotuj obserwacje rzeczywistego RF/PTT i konfigurację testu.
Jawnie oznacz niepowodzenia i nieobsługiwane kombinacje; nie wnioskuj
o zgodności na podstawie innego modelu używającego tego samego backendu Hamlib.
