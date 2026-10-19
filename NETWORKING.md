## Stan implementacji

Stan poniżej opisuje kod dostępny w bieżącym worktree na 2026-10-19, a nie
gotowość wydania. Bieżące zmiany pozostają niezacommitowane. Tryb Local jest
domyślny, Client wymaga ręcznego połączenia, a Host można uruchomić ręcznie z
aplikacji. Import pełnego logu pozostaje dostępny na stacji Host. Synchronizacja
nie jest gotowa do użycia w zawodach z powodu braku ręcznego scalania, retencji
zdarzeń i pełnego testu awaryjnego; punktacja i duplikaty są już walidowane
centralnie według obsługiwanych reguł definicji contestu.

| Etap | Stan w kodzie |
|---|---|
| NET-001 | Kontrakt awarii, retry, wersji protokołu i nieznanego logu opisany; część scenariuszy ma testy integracyjne, nie wszystkie scenariusze awarii są automatycznie odtwarzane. |
| NET-002 | Polityki zatwierdzone; pule offline, pełna regułowa deduplikacja i ręczne scalanie konfliktów nie są wdrożone. |
| NET-003–004 | UUID, wersje QSO, lokalny outbox, konflikty i kursory wdrożone; baza ma schemat v13. |
| NET-005–006 | Headless centralny serwer i transakcyjne repozytorium zdarzeń/revizji wdrożone. |
| NET-007 | Serwer TLS/TCP, API synchronizacji, rezerwacji seriali, admin-only metryk, tworzenia logu i importu historii seriali wdrożone. Centralny zapis przelicza punkty i stosuje `dupe_key`; reguły zależne od CTY wymagają centralnego `wl_cty.dat`. Push eventów pozostaje poza implementacją. |
| NET-008–009 | Synchronizacja przyrostowa, snapshoty oraz atomowe zapisy/edycje/usunięcia z outboxem wdrożone; podstawowe ręczne połączenie Client i rozstrzyganie konfliktu (przyjęcie serwera lub ponowienie lokalnej edycji) są dostępne, bez ręcznego scalania. |
| NET-010 | Częściowo wdrożone w niezacommitowanych zmianach worktree: operatorskie rezerwowanie/anulowanie seriali, kontrola band/mode, walidacja i atomowa konsumpcja z QSO, audyt, cache oraz trwała kolejka żądań; brakuje administracyjnej korekty i pełnego statusu/offline UX. |
| NET-011 | Częściowo wdrożone: ręczne połączenie Client przez zweryfikowane TLS oraz ręczne uruchomienie/wyłączenie Host z UI. Host używa wskazanych plików PEM/poświadczeń i oddzielnej centralnej bazy. Read-only jest obsługiwany. Brakuje bezpiecznego magazynu sekretów, automatycznego startu Client/Host i konfiguracji niestandardowego CA. |
| NET-012 | Częściowo wdrożone: status/outbox, ręczna synchronizacja, retry, eksport diagnostyczny, porównanie konfliktu, decyzje accept-server/keep-local z audytem oraz tryb Read-only. Brakuje ręcznego scalania i trwałego heartbeat/ostatniego kontaktu. |
| NET-013 | Główne przepływy wdrożone: Client może utworzyć centralny log (admin), dołączyć kopię lokalną albo addytywnie scalić istniejący lokalny log; historia rezerwacji seriali jest przenoszona przed synchronizacją QSO. Host nadal obsługuje walidowany import porcjami z podglądem i audytem wznowienia. Konflikty QSO nie są automatycznie nadpisywane; są przekazywane do istniejącego przepływu decyzji konfliktu. |
| NET-014 | Test trzech klientów, równoczesnych edycji, restartu klienta i serwera z trwałym outboxem oraz retry po utraconej odpowiedzi wdrożony; nadal brakuje awarii na każdym kroku i restartu w trakcie aktywnego transferu. |
| NET-015 | Częściowo wdrożone: endpoint metryk serwera dla administratora raportuje stan rewizji/eventów/operacji, liczbę klientów, lag kursora, QSO i rezerwacji. Brakuje czasu transakcji, metryk outbox/conflict per klient, retencji eventów i kompletnej procedury operacyjnej. |
| NET-016 | Nieukończone: Local pozostaje domyślny, a Client wymaga jawnego połączenia; pilotaż, pomiary wydajności i kryteria wydania wymagają środowiska wdrożeniowego. |

## Wniosek
Dla założenia jeden centralny log, wiele stacji nie kopiowałbym dokładnie modelu żadnego z trzech loggerów. N1MM dobrze synchronizuje peer-to-peer, ale każdy komputer ma własną kopię logu i ręczna resynchronizacja jest potrzebna m.in. po edycjach lub usunięciach przy rozłączonej stacji. DXLog wspiera komunikację UDP oraz client-server, ale jego dokumentacja ostrzega, że sieć UDP może gubić pakiety; osobny Number Server przydziela numery seryjne. TLF dystrybuuje między węzłami log, numery, dane packet/frequency i czas przez TCP/IP, a instrukcja opisuje też osobną komendę synchronizacji logu.N1MM: multi-computer · N1MM: Network Status · DXLog: Additional Information · DXLog: Menu Options · TLF: manual · TLF: man page

Proponuję więc centralny serwer jako jedyne źródło prawdy, z lokalną bazą-cache i trwałą kolejką wysyłkową na każdej stacji. Daje to N1MM-ową wygodę pracy na stacjach i wykorzystuje DXLog-ową ideę centralnego przydziału numerów, ale zamiast polegać na niepotwierdzonym broadcastzie, zapewnia potwierdzane, odtwarzalne transakcje.

Stan obecny loggera
Aplikacja nadal obsługuje zwykłą pracę lokalną przez SQLite, a repozytorium
zawiera transakcje, backup i migracje schematu (obecnie v13). Dostępny jest
headless `contest_logger_server` oraz jawnie uruchamiany Host w UI, oba z
TLS/TCP i oddzielnym centralnym SQLite. Aplikacja ma także klienta
synchronizacji przyrostowej, hostowy import lokalnego logu i dołączanie Clienta
do dostępnego centralnego logu bez lokalnego odpowiednika. Nie należy współdzielić
pliku SQLite przez udział sieciowy; tryb Host jest ręczny i kończy się razem
z aplikacją, więc nie zastępuje trwałej usługi serwerowej.

Proponowana architektura

Serwer centralny utrzymuje kanoniczny log i jego historię. Na początek może używać SQLite na lokalnym dysku serwera; klienci nigdy nie otwierają bezpośrednio jego pliku. Jeśli wymagania wydajności lub dostępności wzrosną, warstwę serwerowej bazy będzie można później zmienić.
Każda stacja zachowuje lokalny SQLite dla szybkiego interfejsu, historii i działania kolejki. QSO oczekujące na wysłanie są widoczne jako „oczekujące”, a nie jako potwierdzone przez serwer.
Transport synchronizacji: potwierdzany protokół TCP/TLS z ramkowaniem i JSON opisany w NET-001 oraz odpytywaniem o zmiany od ostatniej rewizji; opcjonalne komunikaty serwera mogą przyspieszać powiadamianie o nowych zmianach. UDP można zostawić na niekrytyczne odkrywanie serwera lub statusy, ale nie na trwałe QSO.
Identyfikatory: stałe UUID logu i stacji oraz UUID QSO/operacji. Nie opierać synchronizacji na lokalnym id INTEGER, bo identyfikatory z niezależnych baz będą kolidować.
Rewizje: serwer przydziela monotoniczny numer rewizji dla każdej zaakceptowanej zmiany. Klient przechowuje swój kursor synchronizacji i pobiera brakujące zmiany od ostatniej rewizji.
Idempotencja: każda wysłana operacja ma swój UUID. Jeśli serwer zatwierdzi zapis, ale odpowiedź zaginie, ponowienie żądania zwróci poprzedni wynik zamiast dodać QSO ponownie.
Atomowy commit: zapis QSO, jego numer seryjny, zdarzenie w historii i nowa rewizja serwera są zatwierdzane w jednej transakcji DB. Klient potwierdza operację dopiero po otrzymaniu odpowiedzi po commitcie. Pobraną porcję zmian również stosuje lokalnie w jednej transakcji.
To model bliższy centralnemu commitowi SVN niż pełnemu rozgałęzianiu jak w Git: wiele stacji może przygotować zmiany, ale serwer nadaje im wspólną kolejność i zatwierdza je jako niepodzielne operacje.

Konflikty i numery seryjne
Równoczesne nowe QSO: serwer w jednej transakcji sprawdza duplikaty zgodnie z regułami konkursu, przydziela numer, zapisuje QSO i publikuje rewizję.
Edycja lub usunięcie: nie używać „ostatni zapis wygrywa”. Każda zmiana wskazuje oczekiwaną wersję QSO; jeśli ktoś wcześniej zmienił ten rekord, serwer zwraca konflikt i zachowuje obie wersje do rozstrzygnięcia. Usunięcie powinno być tombstone’em w historii, nie fizycznym skasowaniem.
Numery seryjne: stacja rezerwuje numer z serwera przed jego wysłaniem korespondentowi. Numer pozostaje zużyty nawet, jeśli QSO nie zostanie zapisane — w ten sposób nie da się go przydzielić innej stacji i rozminąć wymiany z logiem. Rezerwacja i zapis QSO muszą być idempotentne i audytowalne.
Tryb offline: tu jest ważny kompromis. Gdy kilka rozłączonych stacji ma nadawać globalnie kolejne numery seryjne, nie da się jednocześnie zagwarantować unikalności i ciągłości bez wcześniejszej koordynacji. Domyślnie proponuję więc blokować rezerwację i zapis wymagający numeru, gdy serwer jest niedostępny. Opcjonalne pule numerów rezerwowane wcześniej pozwolą pracować offline bez kolizji, ale mogą tworzyć luki. Dla logów bez wspólnej numeracji można dopuścić kolejkę offline i rozwiązywać duplikaty przy synchronizacji.
Opcje konfiguracji dla operatora
W ustawieniach logu lub w sekcji Sieć:

Opcja	Proponowane ustawienie
Tryb	Lokalny / Host serwera / Klient centralnego serwera
Adres i port serwera	Wymagane dla klientów; ręczne wpisanie jako podstawowy sposób dołączenia
Zabezpieczenie	TLS oraz token dostępu; serwer kontroluje, które stacje mogą zapisywać
Log do synchronizacji	Wskazanie istniejącego logu centralnego albo utworzenie nowego
Tożsamość stacji	Unikalny identyfikator stacji i etykieta widoczna w historii
Synchronizacja	Automatyczna; ponawianie po rozłączeniu; widoczny stan i ostatnia zsynchronizowana rewizja
Zachowanie offline	Blokuj zapisy wymagające serwera / kolejkuj bez numerów / używaj zarezerwowanej puli
Numeracja seryjna	Centralne rezerwowanie — włączane tylko dla contestów używających wspólnych numerów
Status serwera	Liczba podłączonych stacji, ich wersje, bieżący log oraz ostrzeżenia o niesynchronizowanych zmianach
Odkrywanie serwera przez broadcast/mDNS można dodać później; nie powinno być warunkiem poprawnego działania. Serwer powinien ponadto odrzucać niezgodne wersje protokołu lub definicje contestu, zamiast pozwalać stacjom naliczać rozbieżne wyniki.

Kolejność implementacji
Protokół i decyzje zachowania: UUID, rewizje, polityka duplikatów, reguły edit/delete oraz zachowanie podczas utraty serwera.
Migracja schematu i lokalny outbox: trwałe operacje oczekujące, identyfikatory stacji/QSO i kursor serwera; migracja bez utraty istniejących logów.
Serwer centralny: uwierzytelnianie, wybór logu, zapis transakcyjny, historia rewizji, rezerwowanie numerów, backup i odzyskiwanie.
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

Poniższe reguły są normatywne dla implementacji v1. Zmiana niekompatybilna
wymaga zwiększenia wersji protokołu. Klient i serwer nie mogą interpretować
niejednoznacznie pól ani stosować cichych fallbacków.

##### Transport i ramkowanie

- Klient łączy się przez TCP z TLS 1.2 lub nowszym. Walidacja certyfikatu
  serwera jest obowiązkowa; błędny certyfikat, nieoczekiwany host lub brak TLS
  kończą połączenie.
- Strumień TCP składa się z ramek: 4-bajtowa długość payloadu unsigned
  big-endian, po niej dokładnie tyle bajtów kompaktowego JSON UTF-8. Długość
  nie obejmuje prefiksu. Zerowa długość, niepoprawny UTF-8/JSON i ramka większa
  niż 1 MiB powodują zamknięcie połączenia.
- Każde żądanie ma jedną odpowiedź skorelowaną przez `request_id`. Serwer może
  przesłać asynchroniczne zdarzenie `event_batch` z `request_id: null`.
  Mutacje od jednej stacji są wysyłane kolejno, bez równoległych batchy.
- Limit batcha mutacji to 100 operacji, a limit strony zmian to 500 eventów.
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

##### Uwierzytelnienie i handshake

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
  a `qso.delete` tombstone. Asynchroniczne `event_batch` używa tego samego
  schematu zakresu i eventów co odpowiedź `changes`.
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

##### Wiadomości i błędy

| Żądanie | Payload | Odpowiedź |
|---|---|---|
| `hello` | zakres wersji i `capabilities` | `hello_ack` z wybraną wersją |
| `authenticate` | `station_uuid`, `token` | `authenticated` |
| `list_logs` | brak lub filtr uprawnień | `logs` z UUID, nazwą, definicją i rewizją |
| `create_log` (admin) | `log`, `definition_yaml` | `log_created` |
| `import_serial_reservations` (admin) | `log_uuid`, `reservations[]` | `serial_history_imported` |
| `import_log_batch` (admin) | `log_uuid`, `atomic`, `operations[]` | `batch_result` |
| `select_log` | `log_uuid`, opcjonalna `contest_definition_sha256` | `welcome` |
| `submit_batch` | `log_uuid`, `atomic`, `operations[]` | `batch_result` |
| `get_changes` | `log_uuid`, `after_revision`, `limit` | `changes` |
| `get_snapshot` | `log_uuid`, `offset`, `limit`, opcjonalny `snapshot_revision` | `snapshot_page` |
| `ack_revision` | `log_uuid`, `revision` | `acknowledged` |
| `get_status` | `log_uuid` | `status` z rewizją, kursorem i aktywnymi stacjami |
| `get_metrics` | `log_uuid` (wymaga uprawnienia admin) | `metrics` z rewizją, liczbą eventów/operacji/stacji, lagiem kursora, QSO i rezerwacjami |
| `reserve_serial` | `log_uuid`, `reservation_uuid`, `band`, `mode` | `serial_reserved` |
| `cancel_serial` | `log_uuid`, `reservation_uuid` | `serial_cancelled` |
| `get_serial_reservations` | `log_uuid` (wymaga uprawnienia admin) | `serial_reservations` |
| `heartbeat` | `session_uuid`, `last_applied_revision` | `heartbeat_ack` |
| asynchroniczne `event_batch` | zakres rewizji i eventy | bez odpowiedzi |
| odpowiedź/zdarzenie `error` | `code`, `message`, `retryable`, opcjonalny `context` | błąd skorelowany z żądaniem lub asynchroniczny |

`reserve_serial` rezerwuje numer w zakresie zdefiniowanym przez log (globalny,
pasmo, emisja lub pasmo+emisja). `reservation_uuid` jest kluczem idempotencji;
retry zwraca tę samą rezerwację. Rezerwacja jest trwała nawet, gdy QSO nie
zostanie zapisane.

`get_metrics` jest dostępne wyłącznie dla uwierzytelnionej stacji z uprawnieniem
`admin` i dotyczy wybranego logu. Metryki są odczytem bieżących liczników, a nie
systemem monitoringu: nie przechowują historii próbek, czasu transakcji ani
pending/error outbox klienta.

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
v13 (migracje v8–v10 dodają outbox, stan synchronizacji i rezerwacje seriali;
v11 dodaje trwałą kolejkę żądań rezerwacji, v12 audyt rozstrzygania konfliktów,
a v13 audyt resumowalnego importu logu).
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
uruchamia długotrwały host TCP/TLS. Host UI uruchamia ten sam typ serwera z
oddzielnym centralnym SQLite i wymaga adresu/portu, certyfikatu i klucza PEM
oraz pliku poświadczeń stacji. Działa do zamknięcia aplikacji; dla wdrożenia
produkcyjnego zalecany jest osobny proces/serwis.

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
push eventów. Obecna implementacja udostępnia handshake v1 i
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

**NET-007 — Udostępnić API TCP/TLS** *(w toku; podstawowe API i walidacja
wdrożone, push eventów otwarty)*

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
- [ ] Dodać push eventów.
- **Pliki orientacyjne:** nowe `include/logger/network/` i `src/network/`,
  aktualizacja `CMakeLists.txt`, testy protokołu i serwera.
- **Odbiór NET-007:** status i szczegóły konfliktu są wdrożone; test TLS
  potwierdza rollback całego batcha przy konflikcie drugiej operacji, a retry
  idempotentnego batcha nie tworzy dodatkowej rewizji. Rezerwacja seriali jest
  dostępna jako NET-010, a tworzenie logu i import historii jako NET-013;
  centralne punkty i duplikaty są wyliczane według definicji contestu, a push
  eventów pozostaje poza implementacją.

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
od nowa. Serwer nie prowadzi jeszcze retencji/kompaktowania eventów, więc
automatyczna ścieżka `SNAPSHOT_REQUIRED` nie jest osiągalna w obecnej
konfiguracji serwera. Nie ma jeszcze push eventów — nowe zmiany są wykrywane
przez polling.
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
Obsługa konfliktów nadal przechowuje payloady do ręcznego rozstrzygnięcia, bez
dialogu scalania.

**NET-010 — Dodać transakcyjne rezerwowanie seriali**

- Rozszerzyć definicję logu o semantykę numeru: globalny/per-pasmo/per-mode/
  per-pasmo+mode, start, szerokość formatu i czy wymaga potwierdzenia serwera
  przed nadaniem.
- Serwer rezerwuje numer w transakcji z UUID rezerwacji i stacji. Rezerwacja
  jest trwała nawet po anulowaniu QSO; retry tego samego UUID zwraca ten sam
  numer.
- Klient utrzymuje widoczny stan rezerwacji. Przy utracie połączenia serial
  globalny nie może zostać wymyślony lokalnie; blokowanie logowania lub pulę
  offline określają NET-002.
- Dodać audyt zużytych, anulowanych i niewykorzystanych numerów oraz narzędzia
  administratora do korekty z potwierdzeniem.
- **Odbiór:** równoczesne żądania z wielu stacji nie dostają powtórzonego
  numeru; wysłany serial jest dokładnie tym zapisanym w QSO.

**Stan: częściowa implementacja w bieżącym worktree; NET-010 pozostaje w toku.**
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
administratora.

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
Nie ma jeszcze pełnego widoku stanu oczekującego/offline ani administracyjnej
korekty numeracji z potwierdzeniem. Serwer blokuje
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
- Umieścić konfigurację serwera w konfiguracji hosta, a klienta/stacji w
  ustawieniach instalacji; sekrety przechowywać w magazynie systemowym, nie
  w YAML/logach/URL.
- **Pliki orientacyjne:** istniejące ustawienia operatora, `main.cpp`,
  `src/ui/` oraz nowe dialogi sieciowe.
- **Odbiór:** przejście z Local do Client nie nadpisuje ani nie usuwa lokalnych
  logów; przed połączeniem klient pokazuje, który log zostanie użyty.

**Stan: częściowo wdrożone w bieżącym worktree.** Menu Options udostępnia
połączenie Client przez zweryfikowane TLS oraz ręczne uruchomienie Host na
wybranej stacji. Host wskazuje bind address, port, łańcuch certyfikatu PEM,
niezaszyfrowany klucz RSA PEM i plik poświadczeń; plik musi zawierać UUID
lokalnej stacji z uprawnieniami `read`, `write` i `admin`. Centralna baza jest
oddzielna, zapisywana w lokalnym katalogu danych aplikacji i blokowana przed
równoczesnym drugim procesem. Host można zatrzymać z menu. Ścieżki konfiguracji
zapisywane są w `QSettings`, ale zawartość tokenu nie jest kopiowana do ustawień.
Client może synchronizować istniejący lokalny log albo pobrać listę dostępnych
logów centralnych i utworzyć lokalną kopię z wybranym UUID.
Automatyczny start Host/Client, magazyn kluczy systemu operacyjnego i
niestandardowe CA pozostają niezaimplementowane. Uprawnienia `read` bez `write`
są rozpoznawane po uwierzytelnieniu; serwer nadal autoryzuje każdą operację,
a klient nie wysyła zaległego outboxu ani żądań rezerwacji dla takiej stacji.

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

Status rozróżnia Offline, Connecting, Connected, Syncing, Pending i Conflict.
Połączenie z uprawnieniem tylko `read` jest oznaczane jako Read-only; UI
blokuje dodawanie/edycję/usuwanie QSO i rezerwowanie/anulowanie seriali.
Klient pozostawia istniejące lokalne operacje w outboxie i pobiera zdalne
zmiany. Ta blokada jest egzekwowana również przez uprawnienia serwera.
Wybrany konflikt można otworzyć w widoku tylko do odczytu, który porównuje
pola lokalnego i serwerowego payloadu. Operator może atomowo zaakceptować
wersję serwera albo zachować lokalną zmianę jako nową edycję opartą o ostatnią
znaną wersję serwera; obie decyzje zapisują payloady i wybór w trwałym audycie.
Przyjęcie serwera aktualizuje lokalny QSO i kończy konflikt. Zachowanie lokalnej
zmiany zamyka starą operację i tworzy nową operację `qso.update` z oczekiwaną
wersją z serwera, więc równoległa późniejsza edycja nadal zostanie wykryta jako
konflikt. Brak miejsca na lokalny zapis audytu powoduje rollback całej decyzji.
Nie ma jeszcze ręcznego scalania ani heartbeat/ostatniego kontaktu utrwalonego
między uruchomieniami.

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
  odpowiedzi. Testy TLS/tokenów, niepoprawnych wiadomości, limitu rozmiaru i
  przerwania połączenia.
- **Odbiór:** identyczny zbiór zatwierdzonych UUID QSO i identyczna rewizja
  logu na wszystkich klientach po synchronizacji; brak niejawnego „last write
  wins”.

**Stan: rozszerzone pokrycie, etap nadal otwarty.** Istnieją testy migracji i transakcji SQLite,
centralnego repozytorium, snapshotu oraz TLS dla synchronizacji add/update/
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
- Udokumentować instalację, firewall/port, TLS, kopię zapasową, restore,
  rotację tokenów, aktualizację protokołu oraz zachowanie przy utracie hosta.
- Sprawdzić limit retencji/kompaktowania eventów i snapshotów tak, by żaden
  aktywny klient nie utracił możliwości resync.
- **Odbiór:** przetestowany backup/restore przy wyłączonym serwerze odtwarza
  rewizje, logi, operacje idempotencji i seriale; istnieje procedura weryfikacji.

**Stan: częściowo wdrożone.** Endpoint `get_metrics` wymaga uprawnienia
administratora i zwraca online station count, bieżącą rewizję, liczbę eventów,
operacji, zarejestrowanych stacji, maksymalny lag kursora, aktywne QSO oraz
liczniki rezerwacji według statusu. UI udostępnia odczyt metryk z panelu
statusu. Backup bazy i test backup/restore istnieją, a serwer tworzy backup
przed migracją. Nadal brakuje historii/eksportu metryk, czasu transakcji,
metryk pending/error/conflict klienta, retencji eventów oraz instrukcji
wdrożenia, rotacji tokenów i odtworzenia po utracie hosta. Restore produkcyjny
nie został zweryfikowany jako procedura operatorska.

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
cache klienta, kolejka żądań i podstawowe UI rezerwacji są wdrożone. Nadal
brakuje puli offline i narzędzi administracyjnej korekty.
NET-011–012 mają ręczne połączenie Client, jawnie uruchamiany Host, status,
read-only i dwie decyzje konfliktu, ale nie bezpieczny magazyn tokenów ani
scalanie pól QSO. NET-013 obsługuje dołączanie, tworzenie centralnego logu
przez administratora, addytywne scalanie lokalnego logu i transfer historii
seriali; ograniczenia są opisane w sekcji etapu.

### Pierwszy inkrement do wdrożenia

Fundamentalny wycinek (TLS/token, istniejący log, idempotentny outbox,
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
| P1 — operacje sieciowe | NET-007/008 | Admin-only tworzenie logu i import historii są dostępne przez protokół; push eventów nie jest wdrożony. Klient polega na pollingu; snapshot recovery wymaga retencji/kompaktowania eventów, której brak. |
| P1 — numeracja | NET-010 | Brak puli numerów offline i administracyjnej korekty numeracji; status oczekujący/offline wymaga pełniejszego UX. |
| P1 — konfiguracja | NET-011 | Host/Server można uruchomić ręcznie; brakuje systemowego magazynu sekretów, auto-startu Host/Client i CA niestandardowego. |
| P1 — obsługa konfliktów | NET-012 | Przyjęcie serwera, ponowienie lokalnej edycji z audytem i read-only są dostępne; brakuje ręcznego scalania i trwałego heartbeat. |
| P1 — adopcja logów | NET-013 | Dołączanie, tworzenie centralnego logu przez administratora, addytywne scalanie lokalnego logu i transfer historii seriali są wdrożone. Pozostają limity transferu (1000 rezerwacji na żądanie), ograniczenie scalania Client do własnych QSO i odbiór konfliktów/awarii w środowisku wielostacyjnym. |
| P2 — eksploatacja | NET-015 | Brakuje historii metryk, czasu transakcji, retencji eventów i pełnej procedury operacyjnej/restore. |
| P2 — wydanie | NET-016 | Wymaga pilotażu 2–3 stacji i pomiarów w docelowym środowisku; automatyczny rollout/feature flag nie jest wdrożony. |

Capabilities handshake serwera uwzględniają teraz `list_logs`,
`get_serial_reservations`, `get_metrics`, `create_log`,
`import_serial_reservations` i `import_log_batch`; ich szczegóły opisują sekcje
NET-007, NET-010, NET-013 i NET-015.
**Zalecana kolejność:** uzupełnić brakujące testy awaryjne NET-014 i sprawdzić
dane CTY w centralnym wdrożeniu. Później uzupełnić Host/secret store oraz
scalanie pól konfliktów;
retencję, procedury i pilotaż wykonać po ustabilizowaniu tych przepływów.