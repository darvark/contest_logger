## Aktualne tryby i granica bezpieczeństwa

- **Local** — domyślna praca bez połączenia z centralnym serwerem.
- **Client/Host w GUI** oraz **`contest_logger_server --listen`** — bieżące
  ścieżki uruchamiają trusted TCP: transport nie ma TLS, szyfrowania ani
  uwierzytelniania klientów. Rejestracja przekazuje UUID i opis stacji, ale
  serwer ich nie weryfikuje; wszystkie połączone stacje otrzymują odczyt i
  zapis. Ograniczenie dostępu zapewnia wyłącznie zaufana, zamknięta sieć
  LAN/VPN oraz jej firewall. Nie wystawiaj portu serwera do Internetu.
- **TLS/tokeny/role** — starsza ścieżka protokołu i jej szczegóły niżej
  opisują wewnętrzny/legacy wariant, niewybierany przez bieżące GUI ani CLI.
  Nie traktuj tych fragmentów jako instrukcji konfiguracji lub gwarancji
  bezpieczeństwa aktualnego trybu sieciowego.

## Stan implementacji

Stan poniżej opisuje kod dostępny w bieżącym worktree na 2026-10-20, a nie
gotowość wydania. Bieżące zmiany pozostają niezacommitowane. Tryb Local jest
domyślny, Client i Host można uruchomić ręcznie lub przez jawnie włączony
autostart. Import pełnego logu pozostaje dostępny na stacji Host. Synchronizacja
nie jest gotowa do użycia w zawodach z powodu brakującego pełnego testu
awaryjnego; retencja zdarzeń jest wdrożona, a punktacja i duplikaty są
walidowane centralnie według obsługiwanych reguł definicji contestu.

| Etap | Stan w kodzie |
|---|---|
| NET-001 | Kontrakt awarii, retry, wersji protokołu i nieznanego logu opisany; część scenariuszy ma testy integracyjne, nie wszystkie scenariusze awarii są automatycznie odtwarzane. |
| NET-002 | Polityki zatwierdzone; pule offline i pełna regułowa deduplikacja nie są wdrożone. Ręczne scalanie konfliktów jest dostępne w aplikacji. |
| NET-003–004 | UUID, wersje QSO, lokalny outbox, konflikty i kursory wdrożone; baza ma schemat v17. |
| NET-005–006 | Headless centralny serwer i transakcyjne repozytorium zdarzeń/revizji wdrożone. |
| NET-007 | Serwer TCP, API synchronizacji i rezerwacji seriali wdrożone. Bieżący tryb trusted rejestruje stację bez poświadczeń w zaufanej sieci i nadaje wszystkim klientom odczyt/zapis. Funkcje wymagające roli administratora z wariantu TLS/token nie są dostępne jako uprawnienia trusted. Centralny zapis przelicza punkty i stosuje `dupe_key`; reguły zależne od CTY wymagają centralnego `wl_cty.dat`. Powiadomienia `event_available` są wysyłane po commicie do klientów deklarujących capability; polling pozostaje ścieżką awaryjną. |
| NET-008–009 | Synchronizacja przyrostowa, snapshoty oraz atomowe zapisy/edycje/usunięcia z outboxem wdrożone; podstawowe ręczne połączenie Client oraz decyzje konfliktu: przyjęcie serwera, ponowienie lokalnej edycji lub ręczne scalanie pól z kontrolą wersji i audytem. |
| NET-010 | Rezerwowanie/anulowanie seriali, kontrola band/mode, walidacja i atomowa konsumpcja z QSO, lokalny cache i trwała kolejka żądań wdrożone. Korekta administracyjna niewykorzystanej rezerwacji z potwierdzeniem, powodem i audytem należy do wariantu TLS/token; bieżący trusted TCP nie daje klientom roli admin. UI pokazuje rezerwacje i żądania offline; nowych numerów nie przydziela się bez serwera. |
| NET-011 | Konfiguracja uproszczona: Client podaje IP/port i wybiera log; Host podaje adres nasłuchu/port. UUID stacji i nazwa komputera są przekazywane automatycznie. Tryb zaufanej sieci działa bez TLS i uwierzytelniania, więc dostęp do LAN/VPN jest granicą dostępu. Local pozostaje trybem domyślnym; autostart jest opt-in. |
| NET-012 | Częściowo wdrożone: status/outbox, lista podłączonych stacji aktywnego logu z nazwą komputera, znakiem i operatorem, ręczna synchronizacja, retry, eksport diagnostyczny, porównanie i ręczne scalanie konfliktu oraz decyzje accept-server/keep-local z audytem. Tryb Read-only i metryki wymagające roli admin należą do wariantu TLS/token, nie do bieżącego trusted TCP. Lista stacji odświeża się co 5 sekund i ręcznie; zmiana operatora nie wymaga reconnectu. UUID pozostają identyfikatorami protokołu; ostatni kontakt klienta i heartbeat serwera są trwałe. |
| NET-013 | Główne przepływy wdrożone: Client może dołączyć kopię lokalną albo addytywnie scalić istniejący lokalny log; tworzenie logu i walidowany import historyczny pozostają na Host. Historia rezerwacji seriali jest przenoszona przed synchronizacją QSO. Konflikty QSO nie są automatycznie nadpisywane; są przekazywane do istniejącego przepływu decyzji konfliktu. |
| NET-014 | Test trzech klientów, równoczesnych edycji, restartu klienta i serwera z trwałym outboxem oraz retry po utraconej odpowiedzi wdrożony; nadal brakuje awarii na każdym kroku i restartu w trakcie aktywnego transferu. |
| NET-015 | Wdrożono procedurę operacyjną dla instalacji, ograniczenia firewall, backupu/restore, aktualizacji i utraty hosta. Serwer CLI obsługuje `--restore-backup`; test centralny weryfikuje rewizję, QSO, rezerwację serialu, replay idempotentny i zachowanie poprzedniej bazy. Pełny test operatorski należy wykonać przed wdrożeniem produkcyjnym. Retencja zdarzeń jest opisana w NET-008. |
| NET-016 | Nieukończone: Local pozostaje domyślny, a Client wymaga jawnej konfiguracji (autostart jest opt-in); pilotaż, pomiary wydajności i kryteria wydania wymagają środowiska wdrożeniowego. |

Opis protokołu i stanu kodu może obejmować interfejsy wewnętrzne lub
legacy. Dla konfiguracji wdrożeniowej obowiązuje powyższe rozróżnienie trybów
oraz [runbook serwera](docs/NETWORKING_OPERATIONS.md).

Szczegółowa procedura instalacji i odzyskiwania: [Operacje serwera synchronizacji](docs/NETWORKING_OPERATIONS.md).

## Wniosek
Dla założenia jeden centralny log, wiele stacji nie kopiowałbym dokładnie modelu żadnego z trzech loggerów. N1MM dobrze synchronizuje peer-to-peer, ale każdy komputer ma własną kopię logu i ręczna resynchronizacja jest potrzebna m.in. po edycjach lub usunięciach przy rozłączonej stacji. DXLog wspiera komunikację UDP oraz client-server, ale jego dokumentacja ostrzega, że sieć UDP może gubić pakiety; osobny Number Server przydziela numery seryjne. TLF dystrybuuje między węzłami log, numery, dane packet/frequency i czas przez TCP/IP, a instrukcja opisuje też osobną komendę synchronizacji logu.N1MM: multi-computer · N1MM: Network Status · DXLog: Additional Information · DXLog: Menu Options · TLF: manual · TLF: man page

Proponuję więc centralny serwer jako jedyne źródło prawdy, z lokalną bazą-cache i trwałą kolejką wysyłkową na każdej stacji. Daje to N1MM-ową wygodę pracy na stacjach i wykorzystuje DXLog-ową ideę centralnego przydziału numerów, ale zamiast polegać na niepotwierdzonym broadcastzie, zapewnia potwierdzane, odtwarzalne transakcje.

Stan obecny loggera
Aplikacja nadal obsługuje zwykłą pracę lokalną przez SQLite, a repozytorium
zawiera transakcje, backup i migracje schematu (obecnie v17). Dostępny jest
headless `contest_logger_server` oraz jawnie uruchamiany Host w UI, oba z
nieszyfrowanym TCP dla zaufanej sieci i oddzielnym centralnym SQLite. Aplikacja ma także klienta
synchronizacji przyrostowej, hostowy import lokalnego logu i dołączanie Clienta
do dostępnego centralnego logu bez lokalnego odpowiednika. Nie należy współdzielić
pliku SQLite przez udział sieciowy; tryb Host jest uruchamiany ręcznie albo
przez opt-in autostart i kończy się razem
z aplikacją, więc nie zastępuje trwałej usługi serwerowej.

Proponowana architektura

Serwer centralny utrzymuje kanoniczny log i jego historię. Na początek może używać SQLite na lokalnym dysku serwera; klienci nigdy nie otwierają bezpośrednio jego pliku. Jeśli wymagania wydajności lub dostępności wzrosną, warstwę serwerowej bazy będzie można później zmienić.
Każda stacja zachowuje lokalny SQLite dla szybkiego interfejsu, historii i działania kolejki. QSO oczekujące na wysłanie są widoczne jako „oczekujące”, a nie jako potwierdzone przez serwer.
Transport synchronizacji: potwierdzany protokół TCP z ramkowaniem i JSON oraz odpytywaniem o zmiany od ostatniej rewizji; opcjonalne komunikaty serwera mogą przyspieszać powiadamianie o nowych zmianach. Aktualny transport nie ma TLS ani uwierzytelniania i może być używany wyłącznie w zaufanej, zamkniętej sieci LAN/VPN. UDP można zostawić na niekrytyczne odkrywanie serwera lub statusy, ale nie na trwałe QSO.
Identyfikatory: stałe UUID logu i stacji oraz UUID QSO/operacji. Nie opierać synchronizacji na lokalnym id INTEGER, bo identyfikatory z niezależnych baz będą kolidować.
Rewizje: serwer przydziela monotoniczny numer rewizji dla każdej zaakceptowanej zmiany. Klient przechowuje swój kursor synchronizacji i pobiera brakujące zmiany od ostatniej rewizji.
Idempotencja: każda wysłana operacja ma swój UUID. Jeśli serwer zatwierdzi zapis, ale odpowiedź zaginie, ponowienie żądania zwróci poprzedni wynik zamiast dodać QSO ponownie.
Atomowy commit: zapis QSO, jego numer seryjny, zdarzenie w historii i nowa rewizja serwera są zatwierdzane w jednej transakcji DB. Klient potwierdza operację dopiero po otrzymaniu odpowiedzi po commitcie. Pobraną porcję zmian również stosuje lokalnie w jednej transakcji.
To model bliższy centralnemu commitowi SVN niż pełnemu rozgałęzianiu jak w Git: wiele stacji może przygotować zmiany, ale serwer nadaje im wspólną kolejność i zatwierdza je jako niepodzielne operacje.

Konflikty i numery seryjne
Równoczesne nowe QSO: serwer w jednej transakcji sprawdza duplikaty zgodnie z regułami konkursu, przydziela numer, zapisuje QSO i publikuje rewizję.
Edycja lub usunięcie: nie używać „ostatni zapis wygrywa”. Każda zmiana wskazuje oczekiwaną wersję QSO; jeśli ktoś wcześniej zmienił ten rekord, serwer zwraca konflikt i zachowuje obie wersje do rozstrzygnięcia. Usunięcie powinno być tombstone’em w historii, nie fizycznym skasowaniem.
Numery seryjne: stacja rezerwuje numer z serwera przed jego wysłaniem korespondentowi. Numer pozostaje zużyty nawet, jeśli QSO nie zostanie zapisane — w ten sposób nie da się go przydzielić innej stacji i rozminąć wymiany z logiem. Rezerwacja i zapis QSO muszą być idempotentne i audytowalne.
Polityka offline dla globalnych numerów seryjnych: nie przydzielać numerów lokalnie i nie logować QSO wymagającego nowej rezerwacji bez potwierdzenia serwera. Żądanie rezerwacji może być trwale zakolejkowane offline, ale nie oznacza to przydziału numeru; UI jawnie pokazuje oczekiwanie i blokuje zapis. Wcześniej potwierdzona, niewykorzystana rezerwacja pozostaje używalna offline. Pule offline nie są obecnie implementowane ani aktywowane — można je rozważyć osobno, wyłącznie jako jawnie przydzielone zakresy z akceptacją luk.
Opcje konfiguracji dla operatora
W ustawieniach logu lub w sekcji Sieć:

Opcja	Proponowane ustawienie
Tryb	Lokalny / Host serwera / Klient centralnego serwera
Adres i port serwera	Wymagane dla klientów; ręczne wpisanie jako podstawowy sposób dołączenia
Zabezpieczenie	Obecny uproszczony tryb: plain TCP, bez logowania, tylko zaufana LAN/VPN
Log do synchronizacji	Wskazanie istniejącego logu centralnego; utworzenie/import na Host
Tożsamość stacji	Unikalny identyfikator stacji i etykieta widoczna w historii
Synchronizacja	Automatyczna; ponawianie po rozłączeniu; widoczny stan i ostatnia zsynchronizowana rewizja
Zachowanie offline	Blokuj zapisy wymagające serwera / kolejkuj bez numerów / używaj zarezerwowanej puli
Numeracja seryjna	Centralne rezerwowanie — włączane tylko dla contestów używających wspólnych numerów
Status serwera	Liczba podłączonych stacji, ich wersje, bieżący log oraz ostrzeżenia o niesynchronizowanych zmianach
Odkrywanie serwera przez broadcast/mDNS można dodać później; nie powinno być warunkiem poprawnego działania. Serwer powinien ponadto odrzucać niezgodne wersje protokołu lub definicje contestu, zamiast pozwalać stacjom naliczać rozbieżne wyniki.

Kolejność implementacji
Protokół i decyzje zachowania: UUID, rewizje, polityka duplikatów, reguły edit/delete oraz zachowanie podczas utraty serwera.
Migracja schematu i lokalny outbox: trwałe operacje oczekujące, identyfikatory stacji/QSO i kursor serwera; migracja bez utraty istniejących logów.
Serwer centralny: rejestracja klienta w sieci zaufanej, wybór logu, zapis transakcyjny, historia rewizji, rezerwowanie numerów, backup i odzyskiwanie. Trusted TCP nie zapewnia uwierzytelnienia ani autoryzacji per klient.
Klient synchronizacji: kolejka idempotentna, pobieranie od kursora, stosowanie zmian w transakcji, ponawianie po awarii i pełne odtworzenie cache.
Interfejs operatora: konfiguracja hosta/klienta, status połączenia, licznik zmian oczekujących, ręczna synchronizacja, lista konfliktów i import logu do serwera.
Testy awarii i obciążenia: kilka równoczesnych stacji, utrata odpowiedzi po commitcie, restart serwera/klienta, duplikaty, konflikt edycji i rezerwacje numerów.
Kryterium gotowości powinno obejmować m.in. scenariusz „serwer zapisał QSO, ale klient nie dostał potwierdzenia” — po reconnectcie QSO musi istnieć dokładnie raz — oraz równoczesne rezerwacje, które nigdy nie zwracają tego samego numeru.

Aktualizacja stanu: poniższy backlog pozostaje normatywnym opisem docelowych
zachowań; tabela implementacji powyżej rozróżnia już dostępne części od
pozostałych prac.

## Szczegółowy backlog implementacyjny

Zadania są ułożone według zależności. Każdy etap powinien kończyć się działającą,
testowalną zmianą; konfiguracja sieci nie może zmieniać zachowania istniejących
logów lokalnych. Centralny SQLite jest otwierany wyłącznie przez proces serwera,
nigdy bezpośrednio przez klienta ani udział sieciowy.

### Etap 0 — kontrakt i granice synchronizacji

**NET-001 — Spisać kontrakt operacji synchronizacji**

- **Odbiór:** poniższe scenariusze protokołu mają określony wynik po stronie
  klienta i serwera oraz stan trwałych danych. Ich realizacja jest warunkiem
  zaliczenia NET-001 i podstawą testów integracyjnych NET-014.

#### Kontrakt protokołu synchronizacji v1

Poniższy kontrakt v1 dokumentuje pierwotny wariant TLS/token oraz zachowane
interfejsy protokołu. Nie opisuje bieżącego trybu trusted TCP używanego przez
GUI/CLI; szczególnie sekcje transportu, uwierzytelniania i ról nie są
konfiguracją dostępną operatorowi. Zmiana tego historycznego wariantu wymaga
zwiększenia wersji protokołu. Klient i serwer nie mogą interpretować
niejednoznacznie pól ani stosować cichych fallbacków.

##### Transport i ramkowanie

- W wariancie TLS/token klient łączy się przez TCP z TLS 1.2 lub nowszym.
  Walidacja certyfikatu serwera jest obowiązkowa; błędny certyfikat,
  nieoczekiwany host lub brak TLS kończą połączenie. Bieżący wariant
  wdrożeniowy używa trusted TCP bez szyfrowania (zob. sekcję powyżej).
- Strumień TCP składa się z ramek: 4-bajtowa długość payloadu unsigned
  big-endian, po niej dokładnie tyle bajtów kompaktowego JSON UTF-8. Długość
  nie obejmuje prefiksu. Zerowa długość, niepoprawny UTF-8/JSON i ramka większa
  niż 1 MiB powodują zamknięcie połączenia.
- Każde żądanie ma jedną odpowiedź skorelowaną przez `request_id`. Klient
  deklarujący capability `event_notifications` może otrzymać asynchroniczne
  `event_available` z `request_id: null`; zawiera ono tylko UUID logu i
  najwyższą rewizję, więc klient pobiera właściwe zdarzenia przez `get_changes`.
  Klient wysyła operacje outboxa w kolejności w porcjach do 16 operacji;
  serwer akceptuje maksymalnie 100 operacji w jednym atomowym batchu. Po
  niepowodzeniu batcha klient przełącza się na wysyłanie pojedyncze, aby
  zidentyfikować wadliwą operację i nie blokować pozostałych.
- Limit strony zmian to 500 eventów.
  Timeout odpowiedzi wynosi 30 sekund. Heartbeat jest wysyłany co 15 sekund;
  60 sekund bezczynności zamyka połączenie. Retry może użyć nowego
  `request_id`, ale zachowuje pierwotne `operation_uuid`/`reservation_uuid`.

##### Koperta i typy

Każda wiadomość ma kopertę:

```json
{
  "protocol_version": 1,
  "request_id": "UUID v4 lub null dla wiadomości serwera",
  "type": "submit_batch",
  "payload": {}
}
```

- `protocol_version` jest nieujemną liczbą całkowitą, `request_id` UUID v4
  (albo `null` dla wiadomości asynchronicznych), `type` nazwą wiadomości,
  a `payload` obiektem zgodnym ze schematem tego typu.
- UUID są tekstem kanonicznym małymi literami z łącznikami; daty i czasy to
  ISO 8601 UTC z `Z`; rewizje i wersje są nieujemnymi integerami.
- Nieznane typy, brakujące pola wymagane, błędne typy wartości i nieznane pola
  koperty są odrzucane błędem `INVALID_MESSAGE`. Schemat v1 jawnie określa,
  czy ignorowane są nieznane pola payloadu; w żądaniach mutujących są odrzucane.

##### Uwierzytelnienie i handshake — wariant TLS/token

1. Po zestawieniu TLS klient wysyła `hello` z `client_version`,
   `min_protocol_version`, `max_protocol_version` i listą `capabilities`.
   W kopercie `protocol_version` jest najwyższą wersją obsługiwaną przez
   klienta. Serwer odpowiada `hello_ack` z wybraną wersją lub
   `UNSUPPORTED_PROTOCOL`; do czasu wyboru wolno przesłać tylko handshake.
2. Klient wysyła `authenticate` w wybranej wersji z `station_uuid` i `token`.
   Token przesyłany jest tylko przez TLS; nie wolno zapisywać go w logu,
   outbox, raporcie diagnostycznym ani dalszych żądaniach. Odpowiedź
   `authenticated` zwraca `session_uuid` i uprawnienia `read`, `write`, `admin`.
3. Uwierzytelniony klient wysyła `select_log` z `log_uuid` (nullable przy
   wyborze logu) oraz `contest_definition_sha256` znaną klientowi. Hash może
   być null tylko dla nowej stacji, która nie ma jeszcze przypiętej definicji.
   `welcome` zwraca
   `server_uuid`, `server_version`, aktywny `log_uuid`, `current_revision`,
   `earliest_revision`, checksumę definicji contestu oraz uzgodnione capabilities.

Serwer odrzuca wybór logu, jeśli log nie istnieje, stacja nie ma uprawnień
lub podany hash definicji contestu jest inny niż centralny. Gdy klient
przekazuje null, serwer zwraca w `welcome` autorytatywną definicję; klient musi
ją przyjąć i utrwalić przed pierwszą mutacją. Żądania synchronizacji przed
sukcesem handshake są
niedozwolone. Checksumę definiuje SHA-256 z UTF-8 bajtów dokładnej definicji
YAML przekazanej serwerem. `station_uuid` to stabilna tożsamość
instalacji/stacji, etykieta stacji pozostaje osobnym edytowalnym polem;
`log_uuid` i `qso_uuid` są niezależne od lokalnych integerowych kluczy DB.

##### Operacje mutujące

- Każda operacja zawiera `operation_uuid`, `log_uuid`, `station_uuid`,
  `created_at_utc`, `kind` oraz payload operacji. `operation_uuid` jest
  kluczem idempotencji, a `qso_uuid` stabilnym identyfikatorem rekordu.
- `submit_batch` zawiera `log_uuid`, `atomic: true` i 1–100 operacji.
  W v1 batch jest all-or-nothing: błąd walidacji, uprawnień lub wersji w
  dowolnej operacji wycofuje całą transakcję. Wynik zwraca status każdej
  operacji w kolejności wejściowej.
- Ponowienie tego samego `operation_uuid` z tym samym kanonicznym payloadem
  zwraca zapamiętany wynik bez ponownego zapisu. Ponowne użycie UUID z innym
  payloadem zwraca `IDEMPOTENCY_KEY_REUSED`. Idempotency records są utrzymywane
  tak długo jak log/zdarzenia.
- `qso.create` wymaga nowego `qso_uuid` i `expected_version: 0`; serwer ustawia
  `qso_version: 1`.
- `qso.update` wymaga istniejącego `qso_uuid`, zgodnego `expected_version`
  oraz kompletnego obrazu QSO; serwer zwiększa wersję o 1.
- `qso.delete` wymaga zgodnego `expected_version`; tworzy tombstone z nową
  wersją. Nie kasuje historii i nie zwalnia zarezerwowanego numeru.
- Kanoniczny obraz QSO zawiera `qso_uuid`, `log_uuid`, `station_uuid`,
  `station_callsign`, `callsign`, `timestamp_utc`, `frequency_hz`, `band`,
  `mode`, `sent_exchange_fields`, `received_exchange_fields`, `sent_exchange`,
  `received_exchange` i `points`. Mapy wymiany są string→string. Czasy QSO i
  `created_at_utc` operacji oznaczają różne chwile.
- Przykładowe żądanie dodania QSO:

  ```json
  {
    "protocol_version": 1,
    "request_id": "f9355f99-88f2-4ef4-b4d1-7bf49d99787f",
    "type": "submit_batch",
    "payload": {
      "log_uuid": "62347a8b-9690-4cfe-ae3f-7cc21063f731",
      "atomic": true,
      "operations": [{
        "operation_uuid": "133fb0a9-0368-43f4-9126-dd78cfe6e7b9",
        "log_uuid": "62347a8b-9690-4cfe-ae3f-7cc21063f731",
        "station_uuid": "232a099c-6fa4-4fc5-9742-232ba25a8558",
        "created_at_utc": "2026-10-19T06:00:00.000Z",
        "kind": "qso.create",
        "expected_version": 0,
        "qso": {
          "qso_uuid": "fef764e3-c4e6-4408-8aad-26588f40af01",
          "log_uuid": "62347a8b-9690-4cfe-ae3f-7cc21063f731",
          "station_uuid": "232a099c-6fa4-4fc5-9742-232ba25a8558",
          "station_callsign": "SP9ABC",
          "callsign": "DL1XYZ",
          "timestamp_utc": "2026-10-19T06:00:00.000Z",
          "frequency_hz": 14025000,
          "band": "20m",
          "mode": "CW",
          "sent_exchange_fields": {"report": "599", "serial": "001"},
          "received_exchange_fields": {"report": "599", "serial": "027"},
          "sent_exchange": "599 001",
          "received_exchange": "599 027",
          "points": 1
        }
      }]
    }
  }
  ```

  `points` jest tylko podpowiedzią klienta; serwer oblicza i zwraca punktację
  kanoniczną z przypiętej definicji. Przykładowe UUID są ilustracyjne.
- Serwer waliduje kompletność, reguły dupe i prawa zapisu. Punkty klienta są
  propozycją; odpowiedź/event z serwera zawiera wynik kanoniczny. Duplikat
  odrzucany jest jako `DUPLICATE_QSO` z UUID istniejącego QSO, bez automatycznej
  podmiany/scalania.
- Odpowiedź sukcesu dla operacji mutującej zawiera `operation_uuid`,
  `qso_uuid`, `qso_version` i `committed_revision`. Wynik odpowiedzi może być
  wysłany dopiero po commicie serwera.

##### Rewizje, zmiany i kursor

- Każde zatwierdzone zdarzenie otrzymuje kolejną monotoniczną rewizję całkowitą
  w obrębie `log_uuid`; w strumieniu zatwierdzonych zdarzeń nie ma luk. Serwer
  alokuje licznik rewizji transakcyjnie (nie opiera ciągłości na sekwencji,
  która może zużywać wartości po rollbacku). Operacje batcha dostają kolejne
  rewizje, ale są widoczne dopiero po wspólnym commicie.
- `get_changes` przyjmuje `log_uuid`, `after_revision` (wyłącznie rewizje
  większe od tej wartości) i opcjonalne `limit` (domyślnie 500, maksimum 500).
  `changes` zwraca `from_revision`, `to_revision`, `current_revision`,
  `has_more` oraz eventy w ściśle rosnącej kolejności rewizji. Pusta odpowiedź
  ma `to_revision == after_revision`.
- `get_snapshot` przyjmuje `log_uuid`, `offset`, `limit` (1–100) oraz
  opcjonalny `snapshot_revision`. Pierwsze żądanie bez rewizji przypina migawkę
  do aktualnego headu; wszystkie następne strony muszą podać tę samą rewizję.
  `snapshot_page` zwraca `snapshot_revision`, `offset`, `total_qsos`, `sha256`,
  `qsos` i `has_more`. QSO są uporządkowane po `qso_uuid`; checksum to SHA-256
  kompaktowego UTF-8 JSON całej uporządkowanej tablicy `qsos`, a nie pojedynczej
  strony.
- Event zawiera `log_uuid`, `revision`, `event_uuid`, `operation_uuid`,
  `qso_uuid`, `qso_version`, `kind` (`qso.upsert` lub `qso.delete`),
  `station_uuid`, `committed_at_utc`; `qso.upsert` ma pełny obraz QSO,
  a `qso.delete` tombstone. Powiadomienie push nie przenosi eventów i nigdy
  nie zastępuje pobrania, atomowego zastosowania ani potwierdzenia rewizji.
- Klient wysyła `ack_revision` dopiero po zastosowaniu kompletnej, ciągłej
  strony eventów w jednej lokalnej transakcji razem z aktualizacją kursora.
  Serwer zapisuje maksymalny ack per para `(station_uuid, log_uuid)`. Ack
  cofający cursor jest ignorowany; ack wyższy niż rewizja serwera zwraca
  `INVALID_REVISION`.
- `earliest_revision` oznacza najniższą rewizję eventu, którą serwer nadal
  przechowuje; jeśli brak eventów, wynosi `current_revision + 1`. Jeżeli
  `after_revision < earliest_revision - 1`, serwer zwraca `SNAPSHOT_REQUIRED`
  bez przesuwania kursora. Klient pobiera wtedy snapshot stronami przez
  `get_snapshot`, przypięty do jednej rewizji i opatrzony SHA-256 całej
  uporządkowanej tablicy QSO. Dopiero po odebraniu i sprawdzeniu wszystkich
  stron klient atomowo zastępuje lokalny obraz centralnym i przesuwa cursor;
  lokalne operacje outbox/konflikty pozostają zachowane.
- Cursor klienta przesuwa wyłącznie lokalny commit ciągłego strumienia zmian;
  samo potwierdzenie mutacji nie może przeskoczyć nieodebranych wcześniejszych
  rewizji.
- Ręczny audyt `get_consistency_digest` przyjmuje `digest_version: 1`,
  `log_uuid` i `revision`. Serwer zwraca wersjonowaną liczbę QSO oraz SHA-256
  zgrupowane według dwóch pierwszych znaków UUID. Skrót obejmuje
  deterministycznie uporządkowane obrazy QSO, w tym wersję i tombstone.
  Klient może wykonać audyt tylko przy pustym outboxie i dokładnie zgodnym
  kursorze; audyt nie zmienia danych, a rozbieżność zgłasza operatorowi.

##### Wiadomości i błędy

| Żądanie | Payload | Odpowiedź |
|---|---|---|
| `hello` | zakres wersji i `capabilities` | `hello_ack` z wybraną wersją |
| `authenticate` | `station_uuid`, `token`; opcjonalnie `station_identity` po uzgodnieniu capability | `authenticated` |
| `set_station_identity` | `computer_name`, `station_callsign`, `operator_name` (capability `station_identity`) | `station_identity_updated` |
| `list_logs` | brak lub filtr uprawnień | `logs` z UUID, nazwą, definicją i rewizją |
| `create_log` (admin) | `log`, `definition_yaml` | `log_created` |
| `import_serial_reservations` (admin) | `log_uuid`, `reservations[]` | `serial_history_imported` |
| `import_log_batch` (admin) | `log_uuid`, `atomic`, `operations[]` | `batch_result` |
| `select_log` | `log_uuid`, opcjonalna `contest_definition_sha256` | `welcome` |
| `submit_batch` | `log_uuid`, `atomic`, `operations[]` | `batch_result` |
| `get_changes` | `log_uuid`, `after_revision`, `limit` | `changes` |
| `get_snapshot` | `log_uuid`, `offset`, `limit`, opcjonalny `snapshot_revision` | `snapshot_page` |
| `get_consistency_digest` | `digest_version: 1`, `log_uuid`, `revision` | `consistency_digest` z liczbą QSO i skrótami grup |
| `ack_revision` | `log_uuid`, `revision` | `acknowledged` |
| `get_status` | `log_uuid` | `status` z rewizją, kursorem i aktywnymi stacjami |
| `get_metrics` | `log_uuid` (wymaga uprawnienia admin) | `metrics` z rewizją, eventami/operacjami, klientami i ich statusem, czasem transakcji/historią, QSO i rezerwacjami |
| `reserve_serial` | `log_uuid`, `reservation_uuid`, `band`, `mode` | `serial_reserved` |
| `cancel_serial` | `log_uuid`, `reservation_uuid` | `serial_cancelled` |
| `get_serial_reservations` | `log_uuid` (wymaga uprawnienia admin) | `serial_reservations` |
| `adjust_serial` (admin) | `log_uuid`, `reservation_uuid`, `expected_serial`, `corrected_serial`, `reason` | `serial_adjusted` |
| `heartbeat` | `session_uuid`, `last_applied_revision`; opcjonalnie `pending_count`, `error_count`, `conflict_count` | `heartbeat_ack` |
| asynchroniczne `event_available` | `log_uuid`, `current_revision` (dla capability `event_notifications`) | bez odpowiedzi; klient pobiera zmiany przez `get_changes` |
| odpowiedź/zdarzenie `error` | `code`, `message`, `retryable`, opcjonalny `context` | błąd skorelowany z żądaniem lub asynchroniczny |

`reserve_serial` rezerwuje numer w zakresie zdefiniowanym przez log (globalny,
pasmo, emisja lub pasmo+emisja). `reservation_uuid` jest kluczem idempotencji;
retry zwraca tę samą rezerwację. Rezerwacja jest trwała nawet, gdy QSO nie
zostanie zapisane.
`adjust_serial` może zmienić tylko niewykorzystaną rezerwację i wymaga
uzasadnienia. Oczekiwany numer chroni przed nadpisaniem równoległej zmiany;
kolizja z inną rezerwacją lub zużytym numerem jest odrzucana.

`get_metrics` jest dostępne wyłącznie dla uwierzytelnionej stacji z uprawnieniem
`admin` i dotyczy wybranego logu. Heartbeat aktualizuje trwale czas kontaktu,
kursor i ostatnio zgłoszone liczniki outboxa stacji; liczby pending/error/conflict
pochodzą z klienta i są próbkowane przy heartbeatzie. Serwer mierzy czas
transakcji synchronizacji, zapisuje wynik (`success`, `error`, `conflict`) oraz
liczbę operacji. Odpowiedź zawiera agregaty oraz do 100 najnowszych transakcji;
baza zachowuje maksymalnie 1000 rekordów na log. Eksport diagnostyczny klienta
dołącza ostatni zestaw metryk, jeśli administrator wcześniej go pobrał.

Stabilne kody błędów v1: `UNAUTHENTICATED`, `FORBIDDEN`,
`UNSUPPORTED_PROTOCOL`, `INVALID_MESSAGE`, `MESSAGE_TOO_LARGE`, `UNKNOWN_LOG`,
`CONTEST_MISMATCH`, `VALIDATION_FAILED`, `DUPLICATE_QSO`, `VERSION_CONFLICT`,
`IDEMPOTENCY_KEY_REUSED`, `INVALID_REVISION`, `SNAPSHOT_REQUIRED`,
`SERIAL_UNAVAILABLE`, `RATE_LIMITED`, `INTERNAL_ERROR`. Klient ponawia tylko
błędy `retryable: true` i zawsze zachowuje UUID operacji; błędów walidacji
i uprawnień nie ponawia automatycznie. Błędy wewnętrzne nie zawierają sekretów.

##### Semantyka awarii i retry

- Serwer w jednej transakcji utrwala QSO, numer (jeśli jest przydzielany),
  eventy, rewizje i wyniki idempotencji; dopiero potem wysyła potwierdzenie.
  Awaria/rollback oznacza brak potwierdzenia sukcesu.
- Klient utrzymuje wpis outbox do otrzymania sukcesu lub odzyskania wyniku przez
  idempotentne retry. `VERSION_CONFLICT` przenosi operację do konfliktów, nigdy
  do stanu zakończonego.
- Utrata odpowiedzi po commitcie serwera jest naprawiana przez retry tego samego
  UUID operacji; ponowne eventy deduplikuje się po `(log_uuid, revision)` oraz
  `event_uuid`.
- Klient stosuje pobraną stronę eventów i aktualizuje cursor atomowo lokalnie.
  Przerwanie przed commitem powoduje bezpieczne ponowne pobranie tej samej
  strony; po commicie klient kontynuuje od nowego kursora.

##### Scenariusze odbioru NET-001

| Scenariusz | Wymagane zachowanie | Stan końcowy / asercje |
|---|---|---|
| Żądanie mutujące nie dociera do serwera (utrata połączenia przed kompletną ramką) | Klient nie uznaje operacji za zatwierdzoną. Po timeout/reconnect ponawia niezmieniony payload z tym samym `operation_uuid` i może nadać nowy `request_id`. | Serwer nie ma częściowego QSO/eventu/revizji z niekompletnej ramki. Po retry operacja zostaje zatwierdzona raz; outbox klienta znika dopiero po odpowiedzi sukcesu. |
| Serwer zatwierdza operację, ale odpowiedź ginie | Klient zachowuje outbox i po reconnect ponawia operację z tym samym `operation_uuid` i payloadem. | Serwer zwraca zapamiętany pierwotny wynik; w logu jest jedno QSO, jedna operacja i jeden zestaw eventów/revizji. Klient oznacza outbox jako potwierdzony. |
| Klient restartuje się przy nierozstrzygniętym żądaniu | Po starcie odtwarza outbox i cursor z trwałej bazy, wykonuje handshake i ponawia tę samą operację UUID; nie tworzy nowej operacji dla tego QSO. | Wynik jest taki sam jak przy nieprzerwanym kliencie. Operacja jest zatwierdzona najwyżej raz, a cursor nie wyprzedza lokalnie zastosowanych eventów. |
| Serwer restartuje się przed commitem | Transakcja niedokończona zostaje wycofana przez bazę. Klient po reconnect ponawia operację. | Brak częściowego QSO, numeru, eventu, rewizji lub wyniku idempotencji; retry może zatwierdzić operację raz. |
| Serwer restartuje się po commicie, przed odpowiedzią | Trwała baza zachowuje QSO, eventy, rewizję i wynik `operation_uuid`. Klient ponawia po reconnect. | Serwer odpowiada pierwotnym wynikiem; brak drugiego QSO/eventu i brak dodatkowej rewizji. |
| Niezgodna wersja protokołu | Serwer odpowiada `UNSUPPORTED_PROTOCOL` na `hello`, nie wybiera wersji i zamyka sesję bez przyjęcia dalszych żądań. Klient pokazuje błąd zgodności, nie próbuje mutacji ani downgrade'u bez jawnego wsparcia. | Brak utworzonej/zmienionej rezerwacji, QSO, operacji i rewizji. Outbox pozostaje zachowany jako oczekujący/błędny, a nie potwierdzony. |
| Klient wybiera nieistniejący `log_uuid` | `select_log` zwraca `UNKNOWN_LOG`; log nie jest tworzony automatycznie. Klient może wrócić do `list_logs` lub poprosić operatora o wybór prawidłowego logu. | Sesja nie ma aktywnego logu; `submit_batch`, `get_changes` i `reserve_serial` dla tego logu są niedopuszczalne. Żadne dane nie ulegają zmianie. |

Scenariusze utraty żądania obejmują utratę całej ramki i połączenie zerwane
w trakcie ramki. W obu przypadkach serwer ignoruje niekompletny payload i nie
wykonuje operacji; retry zawsze używa tego samego UUID idempotencji. W scenariuszu
restartu serwera punkt awarii jest testowany po stronie trwałego commitu, a nie
na podstawie tego, czy klient odebrał odpowiedź. Każdy test porównuje liczbę
QSO/eventów/revizji oraz trwały stan outbox/cursora, a nie tylko kod odpowiedzi.

**NET-002 — Zatwierdzić polityki offline, konfliktów i duplikatów**

**Stan: polityki zatwierdzone.**

- Po dołączeniu pierwszej stacji definicja reguł, wymiany i konfiguracja
  numeracji logu są niezmienne. Zmiana tych ustawień wymaga utworzenia nowego
  logu; niezgodna wersja definicji odrzuca synchronizację z jawnym błędem.
- Edycja QSO wymaga `expected_version`. Niezgodność zapisuje konflikt i nie
  nadpisuje wersji centralnej. Operator rozstrzyga go jawnie; domyślnie
  zachowuje się centralny rekord, a propozycja lokalna pozostaje dostępna do
  ręcznego scalenia.
- Usunięcie jest tombstone'em z rewizją i autorem; nie usuwa fizycznie
  historii. Przywrócenie wymaga jawnej nowej operacji.
- Deduplikację nowych QSO określają reguły wybranego logu (klucz dupe);
  dozwolone kontakty wielopasmowe/wielomodalne pozostają osobnymi QSO.
  Dopasowanie dupe nigdy nie scala automatycznie ani nie odrzuca bez informacji
  operatora.
- Globalny serial wymaga połączenia i rezerwacji z serwera. Opcjonalna,
  jawnie włączana pula offline może być użyta wyłącznie po jej wcześniejszej
  rezerwacji; niewykorzystane numery tworzą dopuszczalne luki i nie podlegają
  ponownemu przydziałowi. Dla logów bez wspólnej numeracji QSO może oczekiwać
  offline w outboxie.

| Przypadek | Zachowanie |
|---|---|
| Inna wersja QSO na serwerze niż `expected_version` | Serwer nie zapisuje edycji; odsyła konflikt zawierający wersje i aktualny rekord. Klient zachowuje lokalną propozycję. |
| Usunięcie QSO | Zapisuje tombstone jako nową rewizję; ponowienie z tym samym UUID operacji jest idempotentne. |
| Duplikat według reguł logu | Serwer odsyła jawny wynik dupe, nie nadpisuje istniejącego QSO i nie zatwierdza nowego bez potwierdzenia operatora. |
| Brak połączenia przy globalnym serialu bez puli | Nie przydziela numeru ani nie udaje sukcesu; QSO nie jest zatwierdzane jako centralne. |
| Brak połączenia z wcześniej zarezerwowaną pulą | Pozwala użyć tylko numeru z puli; potwierdzenie centralne następuje po synchronizacji. Niewykorzystane numery pozostają lukami. |
| Offline w logu bez wspólnego serialu | Zapisuje QSO jako lokalne/oczekujące i tworzy outbox; nie pokazuje go jako zsynchronizowanego. |
| Niezgodna definicja logu | Synchronizacja jest odrzucana z błędem; nie ma niejawnego fallbacku ani zmiany reguł. |

- **Odbiór:** te przypadki są kontraktem dla serwera i UI; brak dostępnej
  funkcji pozostaje jawnie niedostępny do czasu wdrożenia zależnego etapu.

### Etap 1 — identyfikatory i lokalna kolejka

**NET-003 — Dodać stabilne identyfikatory i wersje rekordów**

- Dodać UUID logu, stacji i QSO oraz wersję rekordu QSO. Zachować istniejące
  całkowite `id` jako lokalny klucz i mapować je na UUID dla kompatybilności.
- Migracja istniejących logów nadaje UUID każdemu rekordowi, logowi i lokalnej
  instalacji; musi być transakcyjna, powtarzalna i nie zmieniać istniejących
  identyfikatorów lokalnych ani danych QSO.
- Dodać constrainty i indeksy wymagane do unikalności UUID.
- **Pliki orientacyjne:** `include/logger/domain/types.h`,
  `include/logger/domain/contest_definition.h`,
  `src/data/sqlite_repository.cpp`, `tests/test_sqlite_repository.cpp`.
- **Odbiór:** migracja testowana dla pustej, aktualnej i starszej bazy; ponowne
  uruchomienie migracji nie zmienia UUID.

**Stan: wdrożono lokalną część etapu.** Migracja v7 oraz typy domenowe
przechowują UUID logu, stacji i QSO oraz wersję QSO; aktualny schemat bazy to
migracje v8–v10 dodają outbox, stan synchronizacji i rezerwacje seriali;
v11 dodaje trwałą kolejkę żądań rezerwacji, v12 audyt konfliktów, v13 audyt
importu logu, v14 rewizje historii, v15 wynik ręcznego scalania konfliktów,
v16 audyt administracyjnych korekt rezerwacji, a v17 trwałe liczniki heartbeat
i historię metryk transakcji.
Migracja jest transakcyjna, tworzy UUID
stacji lokalnej, uzupełnia istniejące rekordy i zakłada unikalne indeksy.
Wersja QSO rośnie przy lokalnej edycji. To nie implementuje jeszcze
synchronizacji sieciowej ani walidacji `expected_version` po stronie serwera.

**NET-004 — Wprowadzić lokalny outbox i stan synchronizacji**

- Dodać tabele lokalne `sync_state`, `sync_outbox` i `sync_conflicts`.
- Outbox przechowuje niezmienny payload operacji, UUID, log, czas utworzenia,
  stan (pending/in-flight/accepted/conflict/failed), liczbę prób i ostatni błąd.
- Zapis QSO lokalnie i utworzenie wpisu outbox wykonać w jednej transakcji,
  jeśli tryb synchronizacji jest włączony.
- Zdefiniować czytelny stan rekordu: lokalny, oczekujący, zsynchronizowany,
  konflikt; nie przedstawiać niepotwierdzonych danych jako zatwierdzonych
  centralnie.
- **Odbiór:** restart aplikacji nie gubi pending outbox; błędy pozostają
  widoczne i możliwe do ponowienia; zwykły tryb lokalny nie tworzy outbox.

**Stan: wdrożono warstwę repozytorium.** Migracja v8 dodaje `sync_state`,
`sync_outbox` i `sync_conflicts`. API repozytorium pozwala włączyć synchronizację
dla konkretnego logu, odczytywać/przesuwać monotoniczny cursor, pobierać
operacje oczekujące, przechodzić ich stany, ponawiać błędy i zapisywać konflikt
wraz z lokalnym i zdalnym payloadem. Zapis i edycja QSO wraz z wpisem outbox
wykonywane są w jednym SQLite savepoint; awaria outbox wycofuje zmianę QSO.
Stan `QsoSyncStatus` rozróżnia lokalny, oczekujący, zsynchronizowany i
konfliktowy. Synchronizacja jest domyślnie wyłączona. UI konfiguracji, klient
transportowy i atomowe stosowanie zdalnych stron eventów należą do dalszych
etapów i nie są jeszcze dostępne.

### Etap 2 — centralny serwer i trwały protokół

**NET-005 — Wydzielić warstwę centralnego repozytorium**

- Oddzielić operacje domenowe synchronizacji od UI i klientowego
  `SqliteRepository`; serwer używa własnej instancji repozytorium i lokalnego
  pliku SQLite.
- Ustalić sposób uruchomienia serwera: osobny proces/serwis albo jawnie
  wybrana stacja Host z konfiguracją katalogu danych, portu, certyfikatu,
  poświadczeń i limitu klientów. Host musi pokazywać stan i kończyć się
  kontrolowanie, nie może uruchamiać serwera domyślnie.
- Włączyć transakcje, `foreign_keys`, `busy_timeout`, WAL oraz kontrolę
  integralności; wykonać automatyczny backup przed migracją/restore.
- **Odbiór:** serwer może działać bez UI operatorskiego, restartuje się po
  awarii zgodnie z instrukcją wdrożenia, a plik centralny nie jest współdzielony
  przez SMB/NFS.

**Stan: wdrożono bootstrap centralnej bazy.** Klasa `CentralRepository`
otwiera własny plik SQLite w katalogu danych, blokuje równoległe uruchomienie
drugiego procesu na tym samym katalogu, odrzuca rozpoznane udziały sieciowe,
tworzy log ogólny oraz wykonuje backup przed migracją istniejącej bazy.
Dostępny jest osobny headless target `contest_logger_server` oraz ręczny tryb
Host w menu Options wybranej stacji:
`--check [--data-dir <ścieżka>]` inicjalizuje i weryfikuje bazę, a `--listen`
uruchamia długotrwały host TCP w zaufanej sieci. Host UI uruchamia ten sam typ
serwera z oddzielnym centralnym SQLite i wymaga wyłącznie adresu nasłuchu oraz
portu. Ruch nie jest szyfrowany, a klient nie uwierzytelnia się; ogranicz
dostęp do zamkniętej sieci LAN/VPN. Host UI działa do zamknięcia aplikacji;
dla ciągłej pracy zalecany jest osobny proces/serwis.

**NET-006 — Zapisywać zdarzenia i rewizje atomowo**

- Dodać centralne tabele `sync_events`, `sync_operations` (idempotencja),
  `sync_clients` (kursory/stacje), `sync_log_state` (ostatnia rewizja) oraz
  historię wersji/tombstone QSO.
- W jednej transakcji serwera: sprawdzić uprawnienia i wersję, sprawdzić
  duplikat, zapisać/zmienić QSO, przydzielić numer jeśli wymagany, dodać event,
  zapisać wynik operacji i zwiększyć rewizję.
- Indeksować `(log_uuid, revision)` oraz UUID operacji/QSO; rewizje są
  monotoniczne w obrębie logu.
- **Odbiór:** awaria przed commitem nie zostawia częściowych danych; awaria po
  commitcie, ale przed odpowiedzią, obsłużona retry nie tworzy drugiego QSO ani
  drugiej rewizji.

**Stan: wdrożono transakcyjną warstwę repozytorium.** Migracja v9 dodała
`sync_events`, `sync_operations`, `sync_clients`, `sync_log_state` oraz
`qso_history`, w tym tombstone bez fizycznego usuwania rekordu. `CentralRepository`
przyjmuje batch do 100 operacji QSO create/update/delete w jednej transakcji,
sprawdza `expected_version`, wersjonuje QSO, zapisuje snapshot historii,
idempotency result i event z monotoniczną rewizją. Ponowienie z tym samym UUID
i payloadem zwraca zapamiętane wyniki; ponowne użycie UUID z inną treścią lub
konflikt w dowolnej operacji wycofuje cały batch. Można pobierać strony eventów
i zapisywać monotoniczne kursory stacji. Testy sprawdzają rollback batcha,
brak powielonej rewizji po retry, konflikt wersji, ponowne użycie klucza
idempotencji z innym payloadem, tombstone i granice kursora.

Podstawowe API NET-007 jest wdrożone; etap pozostaje otwarty z powodu braku
powiadomień o wszystkich typach zmian poza QSO. Obecna implementacja udostępnia handshake v1 i
uwierzytelnianie tokenem, autoryzowaną listę/wybór istniejących logów,
atomowe batch'e QSO, odczyt stron eventów, ack kursora, heartbeat i status
logu/stacji. Błąd konfliktu wersji
zawiera aktualny centralny obraz QSO. Rezerwacja seriali jest dodana w NET-010
i udostępniona przez TLS; admin-only tworzenie logu oraz import historii
rezerwacji są dostępne dla przepływów NET-013. Centralny zapis wykorzystuje
`ContestEngine` do walidacji QSO, duplikatów według `dupe_key` oraz
autorytatywnego przeliczenia punktów zarówno przy create, jak i update; wartość
`points` przesłana przez klienta nie jest źródłem prawdy. Obsługiwane składniki
klucza dupe to callsign (wymagany przez format YAML) oraz opcjonalne band/mode.
Reguły punktacji i wymiany zależne od CTY są zatwierdzane tylko wtedy, gdy
serwer ma prawidłowy `wl_cty.dat` w swoim katalogu danych i potrafi rozpoznać
callsign stacji z metadanych centralnego logu oraz korespondenta; w przeciwnym
razie operacja jest jawnie odrzucana jako `CONTEST_DATA_UNAVAILABLE`. Logi bez
YAML pozostają w trybie zgodności wstecznej, bez contestowej walidacji i
przeliczania punktów.
Testy repozytorium i integracyjne TLS pokrywają atomowość/idempotencję,
fragmentowaną ramkę, konflikt wersji, status, odrzucenie nieobsługiwanej wersji,
limit sesji oraz podstawowe ścieżki rezerwacji seriali.

**NET-007 — Udostępnić API TCP/TLS** *(w toku; podstawowe API, walidacja i
powiadomienia o nowych rewizjach wdrożone)*

- [x] Dodać serwer Qt Network z ograniczoną liczbą równoległych połączeń, limitem
  rozmiaru wiadomości, timeoutem, kontrolą formatu i zamykaniem niepoprawnych
  sesji.
- [x] Zaimplementować handshake kompatybilności, uwierzytelnianie tokenem, listę
  logów dostępnych stacji i uprawnienia read/write/admin.
- [x] Zaimplementować zapis batcha idempotentnych operacji, pobieranie eventów
  od rewizji z jawnym `from_revision`/`to_revision` oraz heartbeat.
- [x] Udostępnić status logu/stacji i szczegóły centralnego rekordu dla
  konfliktu `VERSION_CONFLICT`.
- [x] Wysyłać błędy jako typowane kody; tokeny nie są logowane.
- [x] Udostępnić rezerwowanie i anulowanie seriali oraz administracyjny odczyt
  audytu; szczegóły i ograniczenia opisuje NET-010.
- [x] Dodać admin-only tworzenie logu oraz import historii rezerwacji seriali
  używane przez NET-013.
- [x] Walidować zapis centralnie według `dupe_key` z definicji i przeliczać
  punkty z `qso_points`/`qso_point_rules`; punkty wysłane przez klienta są
  zastępowane wynikiem serwera.
- [x] Obsłużyć warunki zależne od CTY przez `wl_cty.dat` w katalogu danych
  serwera; jawnie odrzucać zapis, jeśli wymagane dane lokalizacji są niedostępne.
- [x] Dodać powiadomienia push o nowych rewizjach po zatwierdzeniu batcha.
  Powiadomienie `event_available` jest wysyłane tylko do zalogowanych klientów
  z capability `event_notifications` i uprawnieniem read; zawiera rewizję
  nagłówkową. Klient natychmiast pobiera eventy przez `get_changes`, stosuje je
  atomowo i potwierdza przez `ack_revision`; polling pozostaje fallbackiem.
- **Pliki orientacyjne:** nowe `include/logger/network/` i `src/network/`,
  aktualizacja `CMakeLists.txt`, testy protokołu i serwera.
- **Odbiór NET-007:** status i szczegóły konfliktu są wdrożone; test TLS
  potwierdza rollback całego batcha przy konflikcie drugiej operacji, a retry
  idempotentnego batcha nie tworzy dodatkowej rewizji. Rezerwacja seriali jest
  dostępna jako NET-010, a tworzenie logu i import historii jako NET-013;
  centralne punkty i duplikaty są wyliczane według definicji contestu.

### Etap 3 — replikacja klienta

**NET-008 — Implementować klienta połączenia i synchronizatora**

- Dodać klienta TCP/TLS z reconnectem (wykładniczy backoff z limitem),
  heartbeat, kontrolą certyfikatu i widocznym statusem rozłączenia.
- Pętla synchronizacji najpierw wysyła outbox według kolejności operacji, potem
  pobiera kolejne strony eventów od zapisanej rewizji.
- Stosować każdą pobraną porcję lokalnie w jednej transakcji; zaktualizować
  cursor dopiero w tym samym commicie. Powtórne eventy rozpoznawać po UUID/
  rewizji i nie stosować ponownie.
- Po luce rewizji, wygaśnięciu kursora albo niezgodnej migawce wykonać
  kontrolowane pobranie snapshotu z sumą kontrolną i atomową podmianę danych;
  zachować lokalny outbox i konflikty.
- **Odbiór:** zerwanie połączenia na każdym kroku, restart aplikacji i
  powtórzenie odpowiedzi kończą się tym samym stanem klienta co serwera.

**Stan: cykl przyrostowy i odtwarzanie snapshotu wdrożone; NET-008 w toku.**
`SyncClient` zestawia zweryfikowane TLS, wykonuje handshake/auth/select-log,
utrzymuje timeouty, heartbeat i reconnect z wykładniczym backoffem do 30 s.
Po włączeniu synchronizacji odzyskuje przerwane operacje `in-flight`, wysyła
pojedynczo oczekujące operacje outbox z niezmienionym UUID, oznacza je jako
zaakceptowane po odpowiedzi serwera, pobiera zmiany i atomowo zapisuje QSO
wraz z kursorem w lokalnym SQLite. Strony muszą mieć ciągły zakres rewizji;
strona wadliwa jest wycofywana bez przesunięcia kursora. Konflikt wersji
zachowuje lokalny payload i aktualny obraz serwera jako lokalny konflikt.
Utrata wymaganych rewizji kończy się jawnym `SNAPSHOT_REQUIRED`.

Transfer snapshotu jest stronicowany i przypięty do jednej rewizji; SHA-256
sprawdza pełną, deterministycznie uporządkowaną zawartość. Lokalna podmiana
QSOs i kursora jest jedną transakcją, a lokalne rekordy z nierozstrzygniętym
outboxem/konfliktem nie są usuwane ani nadpisywane. Niekompletna lub wadliwa
migawka nie zmienia lokalnej bazy; po przerwaniu klient rozpoczyna pobieranie
od nowa. Serwer kompaktuje zdarzenia starsze niż 30 dni przy starcie i raz
dziennie. Usuwa wyłącznie ciągły prefiks potwierdzony przez wszystkie stacje
aktywne w okresie retencji; heartbeat aktualizuje ich aktywność i kursor.
Klient z wygasłym kursorem otrzymuje `SNAPSHOT_REQUIRED`. Rewizja każdej wersji
QSO jest przechowywana niezależnie od eventu, więc snapshoty przypięte do
rewizji pozostają poprawne po kompaktowaniu. Historia QSO i wyniki operacji
idempotentnych nie są usuwane. Klienci deklarujący `event_notifications`
otrzymują `event_available` po zatwierdzeniu nowych zdarzeń i od razu pobierają
je standardową ścieżką przyrostową. Pięciosekundowy polling pozostaje fallbackiem
dla utraconego powiadomienia lub klientów nieobsługujących capability.
Test integracyjny pokrywa wysłanie outbox, lokalne potwierdzenie, własny event,
odbiór QSO z innej stacji, atomowy cursor i potwierdzenie go na serwerze.
Testy repozytorium i protokołu sprawdzają snapshot przypięty do starszej
rewizji, strony, checksumę, atomową podmianę i zachowanie nierozstrzygniętego
outboxu.

**NET-009 — Zintegrować zapisy/edycje/usunięcia QSO z synchronizacją**

- Wydzielić ścieżkę zapisu QSO w UI tak, by tryb lokalny nadal używał obecnego
  repozytorium, a tryb klienta zapisywał lokalnie + outbox atomowo.
- Po zatwierdzeniu serwera podmienić lokalny status/revision; rozstrzygnąć
  równoczesną edycję formularza i aktualizację tego samego QSO z innej stacji.
- Edycje i usunięcia wysyłać jako operacje z `expected_version`; konflikty
  przechowywać lokalnie wraz z wersją centralną i lokalną.
- **Odbiór:** formularz nie zamyka się z fałszywym komunikatem sukcesu, gdy
  lokalny zapis/outbox zawiedzie; ponowne wysłanie nie tworzy dubla.

**Stan: wdrożono atomowy outbox dla dodawania, edycji i usuwania oraz
obsługę błędów edycji w UI; pełna konfiguracja trybu Client należy do NET-011.**
Ścieżki zapisu i edycji wykorzystują istniejące transakcje repozytorium:
gdy synchronizacja logu jest włączona, awaria wpisu outbox wycofuje zmianę
QSO. Edycja używa lokalnej wersji jako `expected_version`; jeśli synchronizacja
zastosowała już zdalną zmianę w czasie otwartego formularza, kontrola wersji
odrzuca zapis, pokazuje błąd i pozostawia formularz otwarty. Przy błędzie
lokalnego zapisu/outbox formularz edycji także pozostaje otwarty. Usuwanie
dostępne jest z menu kontekstowego tabeli QSO po potwierdzeniu operatora;
repozytorium zapisuje lokalny tombstone i operację `qso.delete` w jednej
transakcji, zachowując historię i oczekującą operację do wysłania. Outbox
wysyłany jest w kolejności jego trwałego wstawienia do SQLite (nie wyłącznie
po znaczniku czasu), żeby szybkie create/update/delete tego samego QSO nie
zostały przestawione przy równych timestampach. Test TLS obejmuje replikację
tombstone po usunięciu z klienta.

Podstawowe połączenie Client można skonfigurować z UI (szczegóły NET-011).
Obsługa konfliktów udostępnia ręczne scalanie pól z audytem.

**NET-010 — Dodać transakcyjne rezerwowanie seriali**

- Rozszerzyć definicję logu o semantykę numeru: globalny/per-pasmo/per-mode/
  per-pasmo+mode, start, szerokość formatu i czy wymaga potwierdzenia serwera
  przed nadaniem.
- Serwer rezerwuje numer w transakcji z UUID rezerwacji i stacji. Rezerwacja
  jest trwała nawet po anulowaniu QSO; retry tego samego UUID zwraca ten sam
  numer.
- Klient utrzymuje widoczny stan rezerwacji i żądań oczekujących. Przy utracie
  połączenia serial globalny nie może zostać wymyślony lokalnie; numer i zapis
  wymagającego go QSO są zablokowane, dopóki serwer nie potwierdzi rezerwacji.
  Wcześniejsza potwierdzona rezerwacja może zostać użyta offline.
- Dodać audyt zużytych, anulowanych i niewykorzystanych numerów oraz narzędzia
  administratora do korekty niewykorzystanej rezerwacji z potwierdzeniem,
  uzasadnieniem i audytem.
- **Odbiór:** równoczesne żądania z wielu stacji nie dostają powtórzonego
  numeru; wysłany serial jest dokładnie tym zapisanym w QSO.

**Stan: podstawowa obsługa wdrożona; pula offline pozostaje celowo wyłączona.**
Definicje contestu walidują `serial_scope` (`global`, `band`, `mode`,
`band_mode`) i `serial_server_required`; numer startowy oraz szerokość wynikają
z definicji generowanego pola serialowego. Serwer rezerwuje numery transakcyjnie,
traktuje UUID jako klucz idempotencji i utrzymuje audyt statusów `reserved`,
`used` i `cancelled`. Numer anulowany nie jest ponownie przydzielany.
Gdy log wymaga rezerwacji, zatwierdzenie `qso.create` wymaga UUID rezerwacji
stacji/logu/zakresu i identycznego numeru w wysłanym polu wymiany; zużycie
rezerwacji i zapis QSO są atomowe. Endpointy rezerwacji, anulowania i odczytu
audytu są dostępne przez TLS; test protokołu sprawdza retry, trwałe anulowanie,
zużycie serialu przez QSO, równoległe żądania dwóch stacji oraz uprawnienia
administratora. Administrator może skorygować numer wyłącznie niewykorzystanej
rezerwacji: wymagane są oczekiwany poprzedni numer, potwierdzenie w UI i powód.
Serwer w jednej transakcji sprawdza bieżący status/wersję oraz kolizję w zakresie,
zmienia numer i zapisuje stację administratora, obie wartości, powód i czas w
`serial_reservation_adjustments`; numery użyte lub anulowane pozostają
niezmienne. Przy niezgodności aktualnego numeru korekta jest odrzucana jako
`SERIAL_CHANGED`.

Przy aktywnej synchronizacji `SyncClient` zapisuje potwierdzoną rezerwację
w lokalnym `serial_reservations`, oznacza ją jako anulowaną po potwierdzeniu
serwera i jako zużytą atomowo z lokalnym zapisem powiązanego QSO. Cache jest
trwały w SQLite i można go odczytać przez API repozytorium; oczekujące żądania
rezerwacji/anulowania są również trwale kolejkowane i ponawiane po ponownym
połączeniu z tym samym UUID. Operatorski formularz udostępnia przycisk
rezerwacji dla contestu z `serial_server_required`; pokazuje numer po
potwierdzeniu, odtwarza rezerwację z cache i blokuje logowanie bez dopasowanego
band/mode, logu, stacji i statusu `reserved`. Niewykorzystaną rezerwację można
anulować z potwierdzeniem; UI ostrzega, że numer zostanie trwale zużyty,
blokuje użycie serialu w QSO podczas oczekiwania na odpowiedź i komunikuje
potwierdzenie lub błąd. Żądanie anulowania korzysta z trwałej kolejki klienta.
Network Status pokazuje cache rezerwacji, stan, stację/QSO oraz wszystkie
oczekujące żądania; żądanie offline jest jawnie oznaczone „Queued offline” i
informuje, że numer nie został przydzielony. Administrator może pobrać pełną
listę centralnych rezerwacji i korygować wyłącznie status `reserved`.
Rezerwacja żądana offline jest wysyłana po reconnectcie z tym samym UUID;
numer pozostaje pusty, a QSO nie można zapisać przed potwierdzeniem. Pule
offline nie są aktywowane — bezpieczna polityka wymaga dostępności serwera dla
nowych numerów. Serwer blokuje
`qso.create` bez prawidłowej
rezerwacji tylko dla logów z `serial_server_required: true`; nie oznacza to,
że UI już zapewnia rezerwację przed nadaniem.

Ograniczenia centralnego zapisu niezależne od numeracji: loader contestu
obsługuje tylko `callsign` (wymagany), `band` i `mode` jako składniki `dupe_key`.
Warunki punktacji/wymiany oparte na CTY wymagają aktualnego `wl_cty.dat` w
katalogu danych serwera; brak pliku lub nierozpoznane callsigny powodują jawne
odrzucenie operacji, nie domyślne naliczenie punktów. Logi bez YAML pozostają
obsługiwane w trybie zgodności, bez regułowej walidacji punktacji i dupe.

### Etap 4 — konfiguracja i obsługa operatora

**NET-011 — Dodać konfigurację trybu i połączenia**

- Wprowadzić tryby Local / Host / Client oraz walidację zależnych pól
  (adres, port, token, TLS, UUID stacji).
- Klient wskazuje centralny log po UUID, a nie po lokalnym `contestId`.
  Host może utworzyć nowy log albo jawnie zaimportować log lokalny.
- Bieżąca konfiguracja umieszcza adres nasłuchu i port w ustawieniach Host,
  a adres IP, port i wybór logu w Client. Nie wprowadza poświadczeń sieciowych.
- **Pliki orientacyjne:** istniejące ustawienia operatora, `main.cpp`,
  `src/ui/` oraz nowe dialogi sieciowe.
- **Odbiór:** przejście z Local do Client nie nadpisuje ani nie usuwa lokalnych
  logów; przed połączeniem klient pokazuje, który log zostanie użyty.

**Stan: uproszczony tryb zaufanej sieci wdrożony; pilotaż pozostaje otwarty.**
Menu Options udostępnia połączenie Client z adresem IP i portem, wyborem logu
i opcjonalnym autostartem. Klient automatycznie rejestruje UUID, nazwę
komputera, znak i operatora. Host wymaga tylko adresu nasłuchu i portu, a
GUI oraz `contest_logger_server` używają nieszyfrowanego TCP bez poświadczeń.
Każdy osiągalny klient otrzymuje odczyt/zapis; administracyjne tworzenie/
import historycznych logów pozostaje operacją lokalną na Host. Local pozostaje
domyślnym trybem. Centralna baza jest oddzielna, zapisywana w lokalnym katalogu
danych aplikacji i blokowana przed równoczesnym drugim procesem. Dane połączeń
są zapisywane w `QSettings`. Client może synchronizować istniejący lokalny log
albo pobrać listę dostępnych logów centralnych i utworzyć lokalną kopię z
wybranym UUID. Wszystkie stacje trusted otrzymują uprawnienia read/write;
operacje administracyjne pozostają lokalne na Host.

**NET-012 — Dodać status sieci, kolejkę i konflikty**

- Pokazać połączenie, nazwę stacji, centralny log, rewizję lokalną/serwera,
  ostatni kontakt, pending operacje, błędy i kompatybilność.
- Dodać akcje: synchronizuj teraz, ponów błędne, eksportuj raport diagnostyczny,
  odłącz; nie pozwalać na ręczne „wyczyszczenie” outbox bez potwierdzenia.
- Dodać widok konfliktu z porównaniem pól i opcjami: przyjmij serwer,
  zachowaj lokalną zmianę jako nową edycję, ręcznie scal; każda decyzja zapisuje
  audyt.
- Statusy/heartbeat mogą używać kanału push, ale dane krytyczne nadal pobierane
  są z potwierdzonym cursor/revision.
- **Odbiór:** operator odróżnia Connected, Syncing, Pending, Conflict,
  Read-only i Offline; żaden błąd sieci nie znika bez komunikatu.

**Stan: częściowo wdrożone w bieżącym worktree.** Menu Options udostępnia
panel statusu dla aktywnego albo skonfigurowanego logu: pokazuje połączenie,
UUID stacji i logu, lokalnie zastosowaną rewizję, ostatnią znaną rewizję
serwera, czas ostatniego kontaktu oraz liczniki oczekujących, błędnych
i konfliktowych operacji. Tabela prezentuje outbox bez treści QSO. Operator
może wymusić synchronizację, ponowić wybraną operację w stanie `failed`
i wyeksportować raport JSON z metadanymi oraz błędami; raport nie zawiera
tokenu ani payloadów QSO. Outbox nie ma akcji czyszczenia.

Panel **Options > Network status and queue...** zawiera również tabelę
podłączonych stacji dla wybranego logu: nazwę komputera, znak stacji,
operatora i stan połączenia (z oznaczeniem własnej stacji). Dane są pobierane
przez `get_status` po otwarciu, po zmianie stanu połączenia, co 5 sekund oraz
przyciskiem **Refresh**. W bieżącym trybie trusted TCP klient nie
uwierzytelnia się i nie ma indywidualnych ról. Wszystkie połączone stacje
mają odczyt i zapis. Przy
aktywnym GUI Host panel może odczytać tę samą listę bezpośrednio z lokalnego
serwera. Lista obejmuje wyłącznie aktywne sesje z wybranym logiem, nie
wszystkie historycznie zarejestrowane stacje; po rozłączeniu
Clienta bez aktywnego Hosta jest czyszczona i oznaczana jako niedostępna.
Błędy pobrania lub nieprawidłowa odpowiedź są pokazywane nad tabelą.
Rejestracja trusted może przekazać `station_identity` z polami
`computer_name`, `station_callsign` i `operator_name`. Wszystkie trzy pola
są stringami do 255 znaków bez znaków sterujących; mogą być puste. Klient
pobiera nazwę komputera z systemu, a znak/operatora z ustawień stacji.
Żądanie `set_station_identity` aktualizuje te same opisy w aktywnej sesji,
np. po zmianie operatora. Nie zmienia UUID ani dostępu. Opisy są deklarowane
przez klienta i nie są zaufanym identyfikatorem.
Serwer dodaje je do `connected_stations` obok niezmienionych `station_uuid`
i `session_uuid`; nie zapisuje ich jako danych uwierzytelniających.
Nowy klient wysyła opisy tylko po uzgodnieniu capability, więc współpracuje
ze starszym serwerem; UUID oraz opisy nadal nie są danymi uwierzytelniającymi.
Brak opisu jest prezentowany jako `Not provided`, nigdy jako UUID w tabeli.

Status rozróżnia Offline, Connecting, Connected, Syncing, Pending i Conflict.
Read-only i role serwerowe należą do zachowanego wariantu TLS/token
opisanego wyżej; bieżąca konfiguracja trusted TCP nie oferuje połączeń tylko
do odczytu i nadaje wszystkim stacjom odczyt/zapis.
Wybrany konflikt można otworzyć w widoku porównującym pola lokalnego i
serwerowego payloadu. Operator może wybrać lokalną lub serwerową wartość każdego
pola i zapisać scalony wynik jako nową edycję opartą o ostatnią znaną wersję
serwera, zaakceptować całą wersję serwera albo zachować lokalną zmianę. Decyzja,
obie wersje wejściowe i scalony payload trafiają do trwałego audytu.
Przyjęcie serwera aktualizuje lokalny QSO i kończy konflikt. Zachowanie lokalnej
zmiany zamyka starą operację i tworzy nową operację `qso.update` z oczekiwaną
wersją z serwera, więc równoległa późniejsza edycja nadal zostanie wykryta jako
konflikt. Brak miejsca na lokalny zapis audytu powoduje rollback całej decyzji.
Heartbeat/ostatni kontakt nie jest jeszcze utrwalony między uruchomieniami.

**NET-013 — Obsłużyć import i dołączanie istniejącego logu**

- Dodać kreator tworzenia logu centralnego lub dołączenia do istniejącego.
- Import lokalnego logu wykonuje walidację definicji, mapowanie UUID, kontrolę
  duplikatów i podgląd liczby QSO/mnożników przed wysłaniem.
- Import zatwierdza się jako jeden batch lub jawnie podzielone, wznowialne
  batche; zapisuje źródło i wynik w audycie. Nie usuwa lokalnego źródła.
- **Odbiór:** przerwanie importu można wznowić bez powielania kontaktów;
  konflikt istniejącego QSO wymaga decyzji operatora.

**Stan: główne przepływy zaimplementowane; pozostają ograniczenia i odbiór
operacyjny.** Stacja uruchomiona w trybie Host
może wybrać aktywny lokalny log; jego YAML jest ponownie parsowany i każdy QSO
sprawdzany jest względem band/mode, callsign, daty/częstotliwości, exchange i
reguł contestu. Podgląd pokazuje liczbę źródłowych, już istniejących, nowych,
konfliktowych QSO i mnożników. Import zachowuje UUID logu i QSO. Jeśli UUID logu
już istnieje centralnie, definicja YAML musi być identyczna (SHA-256); w innym
przypadku Host tworzy log centralny o tym samym UUID. Konflikty/duble wymagają
jawnego potwierdzenia pominięcia; lokalne źródło nie jest modyfikowane.
Wysyłka odbywa się atomowymi porcjami do 100 QSO. Centralna tabela
`sync_import_audit` przechowuje źródło, stację, liczby zaimportowanych/
pominiętych rekordów, status i błąd. Po przerwaniu importu ponowne uruchomienie
rozpoznaje wcześniej przyjęte UUID QSO i kontynuuje bez duplikowania; test
repozytorium pokrywa ponowienie porcji i audytu.

Dołączenie nowego logu Client pobiera metadane i definicję contestu z listy
serwera, weryfikuje YAML, tworzy lokalny rekord z centralnym UUID i włącza
synchronizację dopiero po pomyślnym wyborze logu. Można też utworzyć centralny
log z lokalnej definicji; tworzenie wymaga uprawnienia administratora. Jeśli
wybrany UUID istnieje lokalnie, UI porównuje YAML, sprawdza pochodzenie QSO i
prosi o potwierdzenie scalenia. Operacja jest addytywna: identyczne QSO jest
idempotentne, a różne dane dla tego samego UUID trafiają do rozstrzygnięcia
konfliktu; żadne lokalne dane nie są po cichu zastępowane.

Przed startem synchronizacji Client importuje historię rezerwacji seriali
(statusy `reserved`, `used` i `cancelled`) z zachowaniem UUID rezerwacji,
stacji, zakresu i powiązanego UUID QSO. Import jest transakcyjny i idempotentny;
konflikt UUID albo zajętego numeru przerywa import zamiast zmieniać historię.
Jedna operacja Client jest ograniczona do 1000 rezerwacji. Tworzenie logu i
import historii wymagają uprawnień administratora; zwykłe `submit_batch`
pozostaje ograniczone do uwierzytelnionej stacji. Client może scalać QSO tylko
z lokalnego logu, którego wszystkie rekordy pochodzą od tej stacji; logi
wielostacyjne wymagają administracyjnego importu Host. Tworzenie/scalanie nie
usuwa lokalnego źródła. Centralna walidacja punktacji i duplikatów jest
wdrożona dla obsługiwanych reguł; format YAML nadal ogranicza `dupe_key` do
callsign, band i mode.

### Etap 5 — testy, wdrożenie i odbiór

**NET-014 — Dodać testy protokołu, migracji i współbieżności**

- Testy jednostkowe DTO, walidacji, serializacji, wersjonowania, idempotencji,
  uprawnień, duplikatów, tombstone i konfliktu wersji.
- Testy repozytorium dla atomowego `QSO + event + revision + operation`,
  rollbacku, cursorów, snapshotu i migracji.
- Testy integracyjne z jednym serwerem i co najmniej trzema niezależnymi
  bazami klienta; synchronizacja pełna i przyrostowa, zmiana jednego rekordu
  równocześnie na dwóch klientach, ponowne połączenie po utracie sieci.
- Testy numeracji z równoczesnymi żądaniami i retry po symulowanej utracie
  odpowiedzi. Testy legacy TLS/tokenów i trusted TCP, rejestracji stacji,
  wiązania UUID QSO, niepoprawnych wiadomości, limitu rozmiaru i przerwania połączenia.
- **Odbiór:** identyczny zbiór zatwierdzonych UUID QSO i identyczna rewizja
  logu na wszystkich klientach po synchronizacji; brak niejawnego „last write
  wins”.

**Stan: rozszerzone pokrycie, etap nadal otwarty.** Istnieją testy migracji i transakcji SQLite,
centralnego repozytorium, snapshotu oraz legacy TLS dla synchronizacji add/update/
delete, retry, konfliktu i rezerwacji seriali. Test TLS numeracji wysyła
równoczesne żądania z dwóch stacji. Nowy test uruchamia trzy niezależne bazy
klientów, sprawdza zgodny zestaw UUID i rewizję po pełnej synchronizacji, a
następnie potwierdza zachowanie outboxu i odbiór zmian przez klienta
read-only. Dodatkowe testy TLS równocześnie wysyłają edycje tej samej wersji QSO
z dwóch stacji (dokładnie jedna zostaje zatwierdzona), restartują klienta z
trwałym oczekującym outboxem i serwer podczas niedostępności klienta, odtwarzają
outbox po reconnect oraz ponawiają QSO i rezerwację serialu po celowo utraconej
odpowiedzi już po commicie. Potwierdzają niezmienioną rewizję i UUID/serial po
retry. Nadal brakuje symulacji awarii na każdym kroku protokołu, restartu
serwera przy istniejącym aktywnym kliencie w trakcie transferu oraz
automatycznego sprawdzenia zgodności danych wszystkich trzech klientów po
każdej z tych awarii.

**NET-015 — Dodać diagnostykę, backup i procedury odzyskiwania**

- Serwer loguje operacje z UUID stacji/logu/operacji i rewizją, nigdy token
  ani niepotrzebne dane wrażliwe.
- Dodać metryki: klienci online, cursor lag, pending/error outbox, konflikty,
  czas transakcji, rezerwacje seriali i rozmiar event logu.
- [x] Udokumentować instalację, firewall/port, kopię zapasową, restore,
  aktualizację protokołu oraz zachowanie przy utracie hosta:
  [procedura operacyjna](docs/NETWORKING_OPERATIONS.md). Dodać serwerowe
  `--restore-backup`; kopia jest walidowana, a istniejąca baza zachowywana
  przed instalacją.
- [x] Wdrożyć retencję i kompaktowanie eventów z odzyskiwaniem snapshotem;
  klient aktywny w okresie retencji zachowuje chroniony cursor, a starszy klient
  dostaje `SNAPSHOT_REQUIRED` (NET-008).
- **Odbiór:** przetestowany backup/restore przy wyłączonym serwerze odtwarza
  rewizje, logi, operacje idempotencji i seriale; istnieje procedura weryfikacji.
  Test automatyczny sprawdza rewizję, QSO, rezerwację serialu, replay operacji
  idempotentnej i zachowanie poprzedniej bazy; operator musi dodatkowo wykonać
  próbę na środowisku testowym.

**Stan: wdrożone w kodzie i dokumentacji; test produkcyjnej procedury pozostaje
zadaniem operatorskim.** Endpoint `get_metrics` wymaga uprawnienia
administratora w wariancie TLS/token. Trusted TCP nie przydziela tej roli,
więc endpointu nie udostępnia zwykłym klientom tego trybu. W wariancie
uprawnionym zwraca online station count, rewizję, eventy, operacje,
zarejestrowane stacje, lag, QSO, rezerwacje, per-stacja pending/error/conflict
oraz trwały ostatni kontakt. Serwer zapisuje czas i wynik transakcji;
odpowiedź udostępnia agregaty i do 100 ostatnich pomiarów, a baza przechowuje
do 1000 na log. Eksport diagnostyczny może dołączyć pobrane metryki. Backup
bazy i test backup/restore istnieją, a serwer tworzy backup przed migracją.
Tryb CLI `--restore-backup` odtwarza zweryfikowaną kopię centralną i zachowuje
poprzednią bazę. Test obejmuje rewizję, QSO, rezerwację serialu i replay
idempotentny. Procedura operatorska obejmuje instalację, trusted TCP/firewall,
backup, restore, aktualizacje i utratę hosta; próbę odtworzenia należy wykonać
na oddzielnym katalogu danych przed wdrożeniem produkcyjnym.

**NET-016 — Wykonać pilotaż i włączyć funkcję etapami**

- Pilotaż na dwóch, następnie trzech stacjach w testowym logu; zweryfikować
  równoległe QSO, edycje, utratę Wi-Fi/LAN i restart hosta.
- Zmierzyć opóźnienie potwierdzenia zapisu i synchronizacji przyrostowej oraz
  zużycie CPU/pamięci przy wielkości logu odpowiadającej długim zawodom.
- Funkcję wdrażać za przełącznikiem; tryb Local pozostaje domyślny do czasu
  przejścia testów. Nie migrować automatycznie istniejących aktywnych contestów
  do sieci.
- **Kryterium wydania:** brak utraconych/powielonych zatwierdzonych QSO,
  brak powtórzonego serialu, jawny i odzyskiwalny każdy konflikt oraz
  udokumentowane odzyskanie po awarii serwera i klienta.

### Zależności i kolejność

```text
NET-001 + NET-002
  -> NET-003 -> NET-004
  -> NET-005 -> NET-006 -> NET-007
  -> NET-008 -> NET-009
  -> NET-010
  -> NET-011 + NET-012 + NET-013
  -> NET-014 -> NET-015 -> NET-016
```

NET-010 zależy od polityk NET-001/002 i transakcji NET-006; serwer, trwały
cache klienta, kolejka żądań, status oczekujących rezerwacji i administracyjna
korekta z audytem są wdrożone. Pula offline jest świadomie wyłączona; nowe
numery wymagają potwierdzenia serwera. NET-011–012 mają ręczne lub opcjonalnie
automatyczne połączenie Client/Host, rejestrację trusted TCP, status, read-only
i trzy decyzje konfliktu z trwałym heartbeatem. NET-013 obsługuje dołączanie,
addytywne scalanie lokalnego logu i transfer historii
seriali; ograniczenia są opisane w sekcji etapu.

### Pierwszy inkrement do wdrożenia

Fundamentalny wycinek (trusted TCP, istniejący log, idempotentny outbox,
pobieranie przyrostowe, klient read-only, audyt konfliktu i importu oraz
metryki administratora) ma testy automatyczne, w tym integrację trzech baz
klientów. Nie jest to gotowe wydanie. Następne kroki to uzupełnienie scenariuszy
awaryjnych NET-014, weryfikacja dostępności i aktualności danych CTY w
wdrożeniu centralnym oraz domknięcie pozostałych ograniczeń operacyjnych.
Dopiero potem można przeprowadzić pilotaż i wykonać pomiary NET-016.







## Pozostałe prace

| Priorytet | Etap | Pozostałe prace |
|---|---|---|
| P0 — poprawność danych | NET-007 | Zrealizowane dla reguł obsługiwanych przez YAML/ContestEngine: centralne punkty, `dupe_key` oraz jawny błąd przy brakujących danych CTY. Loader obsługuje callsign (wymagany), band i mode; brak wsparcia dla innych składników dupe pozostaje ograniczeniem formatu. |
| P0 — odbiór i niezawodność | NET-014 | Testy pokrywają równoczesne edycje, restart klienta i serwera z trwałym outboxem, utratę odpowiedzi po commicie i idempotentny retry QSO/rezerwacji. Pozostają awarie na każdym kroku, aktywny transfer podczas restartu serwera i pełna weryfikacja replik po awariach. |
| P1 — operacje sieciowe | NET-007/008 | Tworzenie logu i import historii w bieżącym trusted TCP wykonuje się lokalnie z GUI Host; endpointy admin-only są częścią wariantu TLS/token. Retencja zdarzeń, odzyskiwanie snapshotem i powiadomienia `event_available` są wdrożone. Polling pozostaje ścieżką awaryjną. |
| P1 — numeracja | NET-010 | Administracyjna korekta niewykorzystanej rezerwacji z uzasadnieniem/audytem i widok stanu oczekującego/offline są wdrożone. Pula numerów offline jest świadomie wyłączona; nowy numer wymaga serwera, a żądanie może oczekiwać w trwałej kolejce. |
| P1 — konfiguracja | NET-011 | Uproszczone GUI Client/Host oraz opt-in autostart są wdrożone. Bieżące GUI/CLI nie konfiguruje TLS, tokenów, dodatkowych CA ani hostowych poświadczeń; te elementy należą do zachowanego wariantu legacy TLS/token. |
| P1 — obsługa konfliktów | NET-012 | Przyjęcie serwera, ponowienie lokalnej edycji i ręczne scalanie pól z audytem są dostępne; heartbeat i czas kontaktu są trwałe. |
| P1 — adopcja logów | NET-013 | Dołączanie, tworzenie centralnego logu przez administratora, addytywne scalanie lokalnego logu i transfer historii seriali są wdrożone. Pozostają limity transferu (1000 rezerwacji na żądanie), ograniczenie scalania Client do własnych QSO i odbiór konfliktów/awarii w środowisku wielostacyjnym. |
| P2 — eksploatacja | NET-015 | Metryki, czas transakcji, stan outbox per klient, procedura operacyjna i CLI restore wdrożone; próba produkcyjnej procedury pozostaje zadaniem operatorskim. |
| P2 — wydanie | NET-016 | Wymaga pilotażu 2–3 stacji i pomiarów w docelowym środowisku; automatyczny rollout/feature flag nie jest wdrożony. |

Capabilities handshake serwera uwzględniają teraz `list_logs`,
`get_serial_reservations`, `get_metrics`, `event_notifications`, `create_log`,
`import_serial_reservations`, `import_log_batch` i `station_identity`; ich
szczegóły opisują sekcje NET-007, NET-010, NET-012, NET-013 i NET-015.
**Zalecana kolejność:** uzupełnić brakujące testy awaryjne NET-014 i sprawdzić
dane CTY w centralnym wdrożeniu. Następnie wykonać próbę odtworzenia według
[procedury operacyjnej](docs/NETWORKING_OPERATIONS.md) oraz pilotaż i pomiary
NET-016. Host, magazyn sekretów Clienta i scalanie pól konfliktów są już
wdrożone; nie zastępuje to odbioru w docelowym środowisku.