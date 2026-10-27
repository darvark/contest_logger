# Operacje serwera synchronizacji

> Ten dokument dotyczy osobnego, starszego serwera CLI. W aktualnej pracy
> sieciowej GUI wybierz **Opcje → Praca w sieci...**: serwer udostępnia
> obecnie otwarty log, bez osobnej bazy centralnej. Instrukcja:
> [praca w sieci](./USER_GUIDE.md#praca-w-sieci--serwer-i-klienci).

**Tryb sieciowy:** aktualne GUI Client/Host i `contest_logger_server --listen`
korzystają z trusted TCP. Połączenie jest nieszyfrowane i nie uwierzytelnia
klientów; każda osiągalna stacja może się zarejestrować i uzyskuje odczyt oraz
zapis. Nazwa komputera, znak stacji i operator są deklarowane przez klienta,
więc nie potwierdzają jego tożsamości. Używaj serwera wyłącznie w zaufanej,
zamkniętej sieci LAN/VPN z ograniczonym firewallem. Nie wystawiaj portu
bezpośrednio do Internetu. TLS, tokeny i role opisane w starszych sekcjach
specyfikacji protokołu nie są konfigurowane ani używane przez bieżące GUI/CLI.

Procedura dotyczy centralnego serwera TCP. Serwer przechowuje dane w
`central-log.sqlite` w katalogu podanym przez `--data-dir`; pliki pomocnicze i
backupy migracyjne znajdują się obok w `backups/`. Nie udostępniaj katalogu
SQLite przez udział sieciowy. To runbook headless serwera CLI; serwer GUI
używa bazy aktualnie otwartego logu. Nie zakładaj, że CLI i serwer GUI
korzystają z tej samej bazy ani nie uruchamiaj obu procesów na tym samym
katalogu.

## Instalacja i pierwsze uruchomienie

1. Zainstaluj pakiet Contest Logger dla docelowego systemu zgodnie z
   [instrukcją budowania i pakowania](./BUILD_AND_RELEASE.md). Pakiet nie
   tworzy usługi ani użytkownika systemowego.
2. Zainicjuj centralną bazę i sprawdź, czy katalog danych jest dostępny dla
   konta serwera:

   ```sh
   contest_logger_server --check --data-dir /var/lib/contest-logger
   ```

3. Uruchom serwer (port domyślny 7443/TCP, limit 16 klientów):

   ```sh
   contest_logger_server --listen \
     --data-dir /var/lib/contest-logger \
     --address 0.0.0.0 --port 7443 --max-connections 16
   ```

   Dla stałej eksploatacji uruchamiaj tę komendę pod supervisorem systemowym
   (np. systemd lub Windows Service Wrapper/Task Scheduler) z automatycznym
   restartem po błędzie. Repozytorium nie dostarcza gotowej jednostki usługi.
   `--check` otwiera/aktualizuje bazę i kończy pracę; `--listen` utrzymuje
   proces serwera. Obie opcje są wzajemnie wykluczające.
   Limit klientów można ustawić w zakresie 1–256; domyślna wartość 16 nie
   została zwiększona bez pomiarów obciążeniowych.

## Firewall i port

- Otwórz wyłącznie przychodzące TCP na skonfigurowanym porcie (domyślnie
  7443), tylko z zaufanych podsieci stacji operatorskich. Ruch nie jest
  szyfrowany, a serwer nie uwierzytelnia klientów. Każda osiągalna stacja ma
  odczyt i zapis. Nigdy nie wystawiaj usługi bezpośrednio do publicznego
  Internetu.
- Na Windows dodaj regułę ograniczoną do podsieci LAN/VPN (uruchom PowerShell
  jako administrator i zastąp przykładowy zakres własnym):

  ```powershell
  New-NetFirewallRule -DisplayName "Contest Logger Server" `
    -Direction Inbound -Action Allow -Protocol TCP -LocalPort 7443 `
    -RemoteAddress "10.20.30.0/24" -Profile Domain,Private
  ```

- Ustaw bind address na interfejs LAN/VPN, jeśli serwer nie ma nasłuchiwać na
  wszystkich interfejsach (`0.0.0.0`).
- Klienci przekazują automatycznie UUID instalacji, nazwę komputera, znak
  stacji i operatora. UUID wiąże QSO z sesją protokołu, ale nie jest
  uwierzytelniony. Złośliwy lub niepożądany klient w dostępnej sieci może
  podać cudzy UUID lub opis; nie traktuj tych pól jako wiarygodnego
  potwierdzenia źródła.
- Wszystkie połączone stacje mają odczyt i zapis. Tworzenie oraz import
  historycznych logów wykonuj lokalnie z GUI Host; nie udostępniaj pliku
  SQLite bezpośrednio innym komputerom.

## Backup i kontrola kopii

Serwer automatycznie tworzy backup przed migracją istniejącej bazy do nowszego
schematu. Nie jest to harmonogram backupu. Kopie wykonuj regularnie na innym
nośniku; dla prostego i spójnego snapshotu zatrzymaj serwer:

```sh
sudo systemctl stop contest-logger-server
sudo install -d -m 700 /srv/contest-logger-backups
sudo cp -a /var/lib/contest-logger/central-log.sqlite \
  "/srv/contest-logger-backups/central-log-$(date -u +%Y%m%dT%H%M%SZ).sqlite"
sudo systemctl start contest-logger-server
```

Zastąp nazwę jednostki i ścieżki wartościami z lokalnej instalacji. Poczekaj
na pełne zatrzymanie procesu przed kopiowaniem; nie kopiuj aktywnego pliku
SQLite bez jego WAL/SHM. W razie potrzeby osobno skopiuj katalog
`/var/lib/contest-logger/backups/`, który zawiera automatyczne kopie sprzed
migracji. Backup i cały katalog `backups/` zawierają dane zawodów, identyfikatory i
historię rezerwacji — chroń je jako dane operacyjne.
W wariancie headless, jeśli używasz dodatkowego `wl_cty.dat`, zabezpiecz go osobno;
baza go nie zawiera. Niezależne pliki logów GUI zawierają kopię używanych danych CTY.
Bieżący serwer trusted TCP nie wymaga certyfikatów TLS ani pliku
poświadczeń.

## Odtwarzanie backupu centralnego

`--restore-backup` sprawdza integralność i strukturę kopii przed instalacją,
zachowuje dotychczasową bazę obok jako `.before-restore-...sqlite`, jeśli taka
istnieje, a następnie waliduje otwartą bazę centralną. Użyj tej samej wersji
aplikacji co backup lub nowszej:

```sh
sudo systemctl stop contest-logger-server
sudo -u contest-logger contest_logger_server \
  --restore-backup /srv/contest-logger-backups/central-log-20261020T070000Z.sqlite \
  --data-dir /var/lib/contest-logger
sudo -u contest-logger contest_logger_server \
  --check --data-dir /var/lib/contest-logger
sudo systemctl start contest-logger-server
```

Jeśli serwer nie jest usługą systemd, zatrzymaj proces ręcznie i wykonaj
polecenia pod kontem, które ma prawo zapisu do katalogu danych. Po starcie
sprawdź logi usługi, połącz klienta i zweryfikuj logi,
rewizję oraz synchronizację w **Options > Network status and
queue... > Server metrics...**. W tabeli stacji potwierdź nazwy komputerów,
znaki i operatorów, oznaczenie własnej stacji oraz usuwanie rozłączonej
stacji po odświeżeniu (automatycznie co 5 sekund lub przez **Refresh**).
UUID identyfikuje stację w protokole, ale ponieważ sieć nie wymaga
uwierzytelniania, klient może podszyć się pod inną stację. Nazwa i znak
przekazane przez klienta są opisowe, a nie potwierdzeniem tożsamości.
Wykonaj testowy zapis na uzgodnionym logu i potwierdź jego odbiór na kliencie.
Nie kasuj automatycznie pliku `.before-restore` ani źródłowego backupu:
zachowaj je do czasu zatwierdzenia odtworzenia.

## Aktualizacje i utrata hosta

Przed aktualizacją zatrzymaj serwer, wykonaj i zweryfikuj backup, zainstaluj
nową wersję i uruchom `contest_logger_server --check --data-dir ...`. Przy
migracji centralna baza tworzy dodatkowy backup przed zmianą schematu. Następnie
uruchom serwer i sprawdź logi oraz testowy odczyt/zapis klienta. Protokół
uzgadnia wersję podczas handshake; klient bez wspólnej wersji odmawia połączenia.
Nie aktualizuj równocześnie wszystkich stacji bez sprawdzenia zgodności
klienta z serwerem.

W razie utraty hosta:

1. Zablokuj jego port/firewall i sprawdź, czy jego kopie zapasowe są dostępne.
2. Na odtworzonym hoście przywróć `central-log.sqlite` z ostatniej sprawdzonej
   kopii i skonfiguruj adres nasłuchu, port oraz ewentualny `wl_cty.dat`.
   Zachowaj UUID logów i serwera z backupu.
3. Zaktualizuj adres IP/port na klientach, jeśli uległy zmianie. Trwałe outboxy
   klientów należy zachować; klient pobierze zmiany od kursora lub wykona
   snapshot, jeśli jego kursor jest starszy od retencji.
4. Jeśli żadnego backupu nie da się zweryfikować, nie twórz nowego pustego
   serwera pod starym UUID logu i nie pozwalaj klientom automatycznie
   przepisywać outboxu. Wyizoluj klienty, zabezpiecz ich lokalne bazy, utwórz
   nowy centralny log po decyzji operatora i uzgodnij ręczny import/reconcile.

## Próba odtworzenia przed wdrożeniem

Wykonaj próbę na kopii i osobnym katalogu danych, nigdy na produkcyjnej bazie:

1. Zatrzymaj testowy serwer, skopiuj jego bazę do backupu.
2. Uruchom `--restore-backup <backup> --data-dir <oddzielny-katalog>`, następnie
   `--check` dla tego katalogu.
3. Uruchom serwer na innym porcie i połącz testowego Clienta. Potwierdź
   identyfikator logu, rewizję, QSO oraz rezerwacje w UI; wyślij i odbierz
   testową zmianę.
4. Zapisz czas odtworzenia i ewentualną utratę danych względem czasu backupu.
   Ustal harmonogram kopii i maksymalny dopuszczalny RPO/RTO przed użyciem
   podczas zawodów.

Test repozytorium automatycznie sprawdza przywrócenie centralnej rewizji, QSO
i rezerwacji serialu, replay wyniku operacji idempotentnej oraz zachowanie
poprzedniej bazy. Test funkcjonalny pełnego procesu uruchamia operator
w środowisku docelowym; nie jest wykonywany automatycznie na sprzęcie
produkcyjnym.
