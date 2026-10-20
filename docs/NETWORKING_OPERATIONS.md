# Operacje serwera synchronizacji

Procedura dotyczy centralnego serwera TCP/TLS. Serwer przechowuje dane w
`central-log.sqlite` w katalogu podanym przez `--data-dir`; pliki pomocnicze i
backupy migracyjne znajdują się obok w `backups/`. Nie udostępniaj katalogu
SQLite przez udział sieciowy. To runbook headless serwera CLI; GUI Host może
używać innej domyślnej lokalizacji danych. Nie zakładaj, że CLI i GUI Host
korzystają z tej samej bazy ani nie uruchamiaj obu procesów na tym samym
katalogu.

## Instalacja i pierwsze uruchomienie

1. Zainstaluj pakiet Contest Logger dla docelowego systemu zgodnie z
   [instrukcją budowania i pakowania](./BUILD_AND_RELEASE.md). Pakiet nie
   tworzy usługi ani użytkownika systemowego.
2. Na hoście Linux utwórz dedykowane konto usługi, katalog danych i katalog
   konfiguracji z dostępem tylko dla administratora i konta usługi. Na Windows
   użyj dedykowanego konta usługi/Task Scheduler oraz ogranicz ACL tych samych
   katalogów. Nie uruchamiaj serwera jako konto o szerszych uprawnieniach niż
   wymagane.
3. Przygotuj certyfikat TLS z poprawnym SAN dla nazwy, której użyją klienci,
   oraz niezaszyfrowany klucz RSA w PEM. Niezaszyfrowany klucz jest wymagany,
   aby serwer mógł wystartować bez interaktywnego hasła; chroń go ACL/trybem
   pliku i nie umieszczaj w repozytorium. W środowisku produkcyjnym użyj
   certyfikatu podpisanego przez zarządzane CA. Dla kontrolowanego testu można
   utworzyć własny certyfikat:

   ```sh
   openssl req -x509 -newkey rsa:3072 -nodes \
     -keyout server.key -out server.crt -days 365 \
     -subj "/CN=logger.example.net" \
     -addext "subjectAltName=DNS:logger.example.net" \
     -addext "basicConstraints=critical,CA:TRUE"
   chmod 600 server.key
   ```

   Dla tego testowego certyfikatu dodaj `server.crt` jako dodatkowe CA
   w konfiguracji każdego Clienta; nazwa hosta w polu serwera musi pasować do
   SAN. Nie wyłączaj weryfikacji TLS.
4. Wygeneruj oddzielny losowy token dla każdej stacji (co najmniej 32 znaki):

   ```sh
   openssl rand -hex 32
   ```

   Umieść tokeny w pliku JSON dostępnym tylko administratorowi/usłudze:

   ```json
   {
     "stations": [
       {
         "station_uuid": "232a099c-6fa4-4fc5-9742-232ba25a8558",
         "token": "REPLACE_WITH_A_UNIQUE_RANDOM_TOKEN",
         "permissions": ["read", "write", "admin"],
         "log_uuids": []
       }
     ]
   }
   ```

   UUID stacji musi odpowiadać instalacji loggera. Dla operatorów niebędących
   administratorami pomiń `admin`; dodaj tylko UUID logów, do których stacja
   ma mieć dostęp. Pusta lista `log_uuids` nie przyznaje dostępu do żadnego
   logu stacji niebędącej administratorem. Tokenu nie wpisuj w argumenty
   procesu, logi ani ustawienia jawne.
5. Sprawdź bazę i utwórz strukturę katalogów:

   ```sh
   contest_logger_server --check --data-dir /var/lib/contest-logger
   ```

6. Uruchom serwer (port domyślny 7443/TCP):

   ```sh
   contest_logger_server --listen \
     --data-dir /var/lib/contest-logger \
     --address 0.0.0.0 --port 7443 \
     --tls-cert /etc/contest-logger/server.crt \
     --tls-key /etc/contest-logger/server.key \
     --tokens /etc/contest-logger/stations.json
   ```

   Dla stałej eksploatacji uruchamiaj tę komendę pod supervisorem systemowym
   (np. systemd lub Windows Service Wrapper/Task Scheduler) z automatycznym
   restartem po błędzie. Repozytorium nie dostarcza gotowej jednostki usługi.
   `--check` otwiera/aktualizuje bazę i kończy pracę; `--listen` utrzymuje
   proces serwera. Obie opcje są wzajemnie wykluczające.

## Firewall, port i TLS

- Otwórz wyłącznie przychodzące TCP na skonfigurowanym porcie (domyślnie
  7443), tylko z zaufanych podsieci stacji operatorskich. Nie otwieraj portu
  UDP i nie wystawiaj usługi bezpośrednio do publicznego Internetu.
- Na Windows dodaj regułę ograniczoną do firmowej podsieci (uruchom PowerShell
  jako administrator i zastąp przykładowy zakres własnym):

  ```powershell
  New-NetFirewallRule -DisplayName "Contest Logger Server" `
    -Direction Inbound -Action Allow -Protocol TCP -LocalPort 7443 `
    -RemoteAddress "10.20.30.0/24" -Profile Domain,Private
  ```

- Ustaw bind address na interfejs LAN/VPN, jeśli serwer nie ma nasłuchiwać na
  wszystkich interfejsach (`0.0.0.0`).
- Sprawdź poprawność certyfikatu, jego SAN i termin ważności przed startem.
  Przy odnowieniu certyfikatu zachowaj tę samą nazwę SAN; jeśli zmieni się CA,
  najpierw zaktualizuj dodatkowe CA na klientach, następnie wdrażaj nowy
  certyfikat i restartuj serwer.
- Klient wymaga zweryfikowanego TLS. Jeśli własne CA nie może być jeszcze
  skonfigurowane na wszystkich stacjach, nie przełączaj ich na nowy certyfikat.

## Rotacja tokenów stacji

Serwer wczytuje plik poświadczeń podczas startu. Protokół utrzymuje jeden
aktywny token na UUID stacji; nie ma okresu równoległej akceptacji starego i
nowego tokenu.

1. Zaplanuj krótką przerwę i poinformuj daną stację o rozłączeniu.
2. Wygeneruj nowy unikalny token. Zmień Clienta na nowy token; zapisuj go
   opcjonalnie w systemowym magazynie sekretów. Jeśli zostanie zapisany przed
   zmianą po stronie serwera, klient może chwilowo nie uwierzytelnić się.
3. Atomowo podmień token dla UUID stacji w `stations.json`, zachowując
   permissions i `log_uuids`; ogranicz plik do administratora/usługi.
4. Zrestartuj serwer, co zamknie istniejące sesje i wczyta nowy token.
5. Połącz stację ponownie i sprawdź stan Clienta, kursor oraz kolejkę. Stary
   token nie może być dalej używany.

Po utracie lub podejrzeniu ujawnienia tokenu usuń go z pliku poświadczeń,
wygeneruj nowy i powtórz rotację przed dopuszczeniem stacji do sieci.

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
migracji. Backup i cały katalog `backups/` zawierają dane
zawodów, identyfikatory i historię rezerwacji — chroń je jak dane logowania.
Osobno i bezpiecznie zabezpieczaj TLS key, `stations.json` i ewentualny
`wl_cty.dat`; baza ich nie zawiera.

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
sprawdź logi usługi, połącz klienta administracyjnego i zweryfikuj logi,
rewizję, online station count oraz lag w **Options > Network status and
queue... > Server metrics...**. W tabeli stacji potwierdź nazwy komputerów,
znaki i operatorów, oznaczenie własnej stacji oraz usuwanie rozłączonej
stacji po odświeżeniu (automatycznie co 5 sekund lub przez **Refresh**).
UUID nadal identyfikują stacje w protokole; opisy są deklarowane przez
klientów i nie potwierdzają tożsamości. Dla starszych klientów/serwerów bez
capability `station_identity` lub pustych opisów tabela pokazuje **Not provided**.
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

1. Zablokuj jego port/firewall i unieważnij wszystkie tokeny, do których
   nie masz pewności, że są bezpieczne.
2. Na odtworzonym hoście przywróć `central-log.sqlite` z ostatniej sprawdzonej
   kopii, skonfiguruj ponownie certyfikat, tokeny, uprawnienia i `wl_cty.dat`.
   Zachowaj UUID logów i serwera z backupu.
3. Jeśli nazwa hosta/certyfikat uległy zmianie, najpierw zaktualizuj endpoint
   i zaufane CA na klientach, a potem zezwól na reconnect. Trwałe outboxy
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
