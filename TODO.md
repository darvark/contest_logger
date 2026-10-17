# TODO — przegląd i rozwój loggera

Lista powstała po przeglądzie implementacji kolejnych etapów loggera. Priorytety:

- **P1** — poprawność danych lub zachowanie wymagane do bezpiecznej pracy.
- **P2** — istotne usprawnienie przed szerszym użyciem.
- **P3** — dalszy zakres rozwoju loggera.

## P1 — poprawność i spójność danych

- [x] **Odtwarzać pełny kontekst punktacji contestu przy logowaniu i edycji.**
  `MainWindow::logQso()` przekazuje do `ContestEngine` poprzednie QSO, ale nie
  buduje `workedMultiplierKeys`. W efekcie silnik może traktować wcześniej
  zdobyty mnożnik jako nowy przy każdym kontakcie. Dodać trwałe/odtwarzalne
  rozliczanie mnożników, wynik łączny oraz testy na sekwencji kilku QSO.
  *Zrobione:* `ContestEngine::buildContext()`/`score()`/`multiplierKeys()` odtwarzają
  mnożniki (exchange, prefix WPX, DXCC/strefy/kontynent z `wl_cty.dat`) przy
  logowaniu, edycji, w podsumowaniu i kolumnie Mult; testy sekwencji QSO.
- [x] **Dopracować numerację serial.** Numer jest obecnie wyliczany jako
  `serialStart + liczba poprzednich QSO`. Odtworzyć następny numer z zapisanych
  wartości serial (z określoną polityką dla braków i korekt), aby import,
  edycja lub starsze wpisy bez wymiany nie powodowały kolizji albo błędnej
  numeracji.
  *Zrobione:* `nextSerialNumber()` = największy zapisany serial + 1 (nie mniej niż
  `serial_start`); luki i ręczne korekty są zachowywane, edycja serialu w dialogu
  aktualizuje kolejny numer.
- [x] **Zachować wymianę starszych QSO podczas edycji.** Migracja v3 dodaje
  JSON-y z pustym domyślnym obiektem, ale nie uzupełnia ich z wcześniejszych pól
  tekstowych. Dialog edycji korzysta z map JSON; zapis starszego wpisu może więc
  wyczyścić jego wymianę. Dodać migrację/backfill albo bezpieczny fallback do
  pól tekstowych i test migracji oraz edycji danych z wersji 1 i 2.
  *Zrobione:* migracja v5 uzupełnia JSON z pól tekstowych, a odczyt ma fallback.
- [x] **Walidować callsign również dla logu bez definicji YAML.** Gałąź logowania
  bez definicji wykonuje dupe-check, lecz nie sprawdza formatu callsign tak jak
  `ContestEngine`; nieprawidłowy, niepusty tekst może zostać zapisany.
  *Zrobione:* `ContestEngine::isValidCallsign()` w obu gałęziach i w dialogu edycji.
- [x] **Nie pomijać błędów konfiguracji Hamlib.** `HamlibBackend::setConfiguration()`
  uznaje brak tokenu za sukces. Wymagane ustawienie portu lub prędkości może
  zostać zignorowane, a połączenie zwróci mylący wynik. Rozróżnić ustawienia
  opcjonalne i wymagane, walidować model/transport oraz pokazać operatorowi
  konkretny błąd.
  *Zrobione:* walidacja modelu/transportu/prędkości, błędy wymaganych tokenów
  trafiają do stanu `Error` z czytelnym komunikatem.

## P2 — obsługa operatorska i wydajność

- [x] **Utrzymywać bazę spotów w ograniczonym rozmiarze.** Bandmapa wczytuje
  najwyżej 500 spotów i ukrywa starsze niż godzinę, ale zapisane rekordy nie są
  okresowo usuwane. Dodać retencję/porządkowanie w SQLite i test długotrwałego
  odbioru spotów.
  *Zrobione:* `pruneSpots()` (wiek + limit wierszy) wywoływane przy zapisie spotu,
  indeks po czasie, test retencji.
- [x] **Walidować czas spotu DX cluster.** Parser rozpoznaje znacznik czasu
  `HHMMZ`, ale wzorzec dopuszcza godziny 24–29. Odrzucać nieprawidłowe godziny
  i minuty oraz testować poprawne przejście przez północ.
  *Zrobione:* odrzucanie godzin >23 i minut >59, data wybierana najbliżej czasu
  odbioru (przejście przez północ i rok).
- [x] **Synchronizować kolejkę CW z zakończeniem nadawania.** Następna wiadomość
  jest wysyłana po czasie oszacowanym z długości tekstu i WPM, a nie po
  potwierdzeniu zakończenia przez keyer. Dodać sygnał zakończenia tam, gdzie
  provider go udostępnia, i określić bezpieczne zachowanie/fallback dla
  providerów bez potwierdzeń.
  *Zrobione:* cwdaemon potwierdza koniec (`ESC h`), mock symuluje potwierdzenie;
  brak potwierdzenia w czasie zatrzymuje kolejkę. Hamlib pozostaje przy
  estymacji czasu — status CW pokazuje, który tryb jest używany.
- [x] **Rozdzielić test połączenia od zapisu i pracy ciągłej CAT.** Obecny dialog
  ma jedną akcję „Save and connect / test”; nie przywraca też zapisanej
  konfiguracji po restarcie aplikacji. Dodać osobne testuj/połącz/rozłącz,
  automatyczne odtwarzanie ustawień (z możliwością wyłączenia) i czytelne
  wyniki prób.
  *Zrobione:* dialog radia ma Test / Zapisz i połącz / Rozłącz / Tylko zapisz,
  wynik `probeRadio()` (VFO, dual-RX) i opcję automatycznego połączenia po starcie.
- [x] **Synchronizować stan CAT z formularzem w kontrolowany sposób.** Częstotliwość
  radia jest zapisywana w QSO, ale wybrane pasmo pozostaje ręczne; odczyt trybu
  co sekundę nadpisuje wybór w formularzu. Dodać mapowanie częstotliwości na
  pasmo, stan „CAT nieaktualny” i jasną regułę, kiedy radio może zmienić tryb
  formularza.
  *Zrobione:* częstotliwość → pasmo (`bandForFrequency`), nagłówek „CAT STALE” po
  5 s bez odczytu; radio zmienia tryb formularza tylko, gdy zmienił się tryb
  w radiu od poprzedniego odczytu (ręczny wybór nie jest nadpisywany co sekundę).
- [x] **Ograniczyć koszt dupe-check.** Zmiana każdego znaku callsign pobiera i
  dekoduje pełną historię contestu. Dodać indeksowane zapytanie repozytorium
  sprawdzające duplikat dla klucza contestu oraz callsign/band/mode; pełną
  historię pobierać tylko wtedy, gdy potrzebuje jej logika contestu.
  *Zrobione:* wskaźnik dupe/mult korzysta z historii w pamięci (odświeżanej po
  zapisie), repozytorium ma indeksowane `findDuplicate()`; pełna historia jest
  pobierana raz przy zapisie QSO.
- [x] **Uzupełnić edycję QSO.** Umożliwić korektę czasu i pól wysyłanej wymiany
  oraz ponownie przeliczać wszystkie wartości po edycji. Walidować zmiany
  callsign/pasmo/trybu względem dozwolonych reguł contestu, wykrywać duplikat
  z wyłączeniem edytowanego rekordu i pokazywać szczegółowe błędy walidacji.
  *Zrobione:* dialog z czasem UTC, pasmem/trybem z definicji, wymianą wysłaną
  i odebraną, walidacją, dupe-check z wyłączeniem edytowanego rekordu i listą błędów.
- [x] **Dodać testy integracyjne przepływu QSO.** Testy obejmują repository,
  silnik contestu i mock CAT, ale nie testują całego przepływu formularz →
  walidacja/dupe → zapis → tabela ani edycji przez UI. Dodać testy Qt dla
  logowania Enterem, błędów walidacji, duplikatów, edycji i odświeżania widoku.
  *Zrobione:* `ui_tests` (Qt offscreen): Enter, błędy walidacji, dupe, edycja,
  odświeżanie tabeli i numeracja serial.
- [x] **Dodać programowe testy CAT bez sprzętu.** Testy integracyjne używają
  Hamlib Dummy i lokalnego `rigctld`; CI buduje i testuje na Linuxie i Windowsie.
- [x] **Dodać ścieżkę pakowania Windows z Hamlib.** `tools/package-windows.ps1`
  przyjmuje pakiet Hamlib z nagłówkami/DLL/`.def`, tworzy import library MSVC,
  instaluje `yaml-cpp` przez vcpkg, wymaga Hamlib przy konfiguracji CMake,
  pakuje zależności runtime i sprawdza obecność DLL.
- [ ] **Zweryfikować CAT i pakowanie Hamlib na docelowych platformach.**
  Skrypt Windows nie został uruchomiony na docelowym Windowsie. Dummy/`rigctld`
  nie sprawdzają fizycznych portów szeregowych ani firmware.
- [x] **Przetestować integrację DX cluster z różnymi serwerami.** Testy parsera
  i reconnectu nie zastępują testu z lokalnym symulatorem Telnet obejmującego
  negocjację opcji, podzielone pakiety, prompt logowania, rozłączenie i ponowne
  logowanie. Dodać scenariusze integracyjne bez zależności od publicznego
  serwera.
  *Zrobione:* lokalny symulator Telnet (IAC, prompt logowania, podzielone pakiety,
  rozłączenie i ponowne logowanie).
- [x] **Sprawdzić izolację awarii radii.** Test łączy mock radio A, powoduje
  błąd konfiguracji radio B i potwierdza, że A nadal może być strojone.

## P3 — zakres dalszych etapów

- [x] **DX cluster i bandmapa:** klient TCP/Telnet, parser spotów, reconnect,
  filtrowanie i ustawianie częstotliwości wybranego radia.
- [x] **CW keyer:** abstrakcja keyera, kolejka wiadomości, przerwanie nadawania
  i prędkość; Hamlib tylko dla obsługiwanych radii/backendów, z mockiem i
  alternatywnym providerem. Zaimplementowano Hamlib, cwdaemon/UDP i mock,
  WPM oraz anulowanie kolejki. Wiadomość wiąże się z jawnym VFO, Hamlib
  otrzymuje A/B, a cwdaemon blokuje VFO B, którego nie potrafi wybrać.
- [x] **SO2V:** model dwóch VFO, niezależne częstotliwości, dwa konteksty
  wprowadzania QSO, wybór RX focus oraz blokada aktywacji dla radia bez
  wykrytego dual-VFO. Mock CAT umożliwia testowanie bez sprzętu.
- [x] **Rozróżnić dual-VFO od dual-RX.** Obecna detekcja bazuje na liście VFO
  Hamlib i udanym odczycie częstotliwości A/B; nie potwierdza, że radio może
  jednocześnie odbierać na obu VFO. Wykrywać rzeczywistą zdolność
  wielo-odbiornikową, jeśli backend ją udostępnia, albo jawnie opisać/oznaczyć
  ograniczenia trybu zamiast sugerować pełne SO2V.
  *Zrobione:* `dualReceiveSupported`/`probeRadio()`; przy braku dual-RX SO2V jest
  dozwolone, ale oznaczone „SO2V, no dual RX” i z ostrzeżeniem.
- [x] **Wdrożyć programowe trasowanie TX/keyera w SO2V.** TX podąża za fokusem
  RX; każda wiadomość kolejkowana jest powiązana z docelowym VFO, Hamlib
  dostaje jawnie VFO A/B, a cwdaemon blokuje nadawanie na VFO B, którego jego
  protokół nie potrafi wybrać. Nagłówek wskazuje aktywny TX; testy sprawdzają
  wybór i zachowanie VFO w kolejce.
- [ ] **Zweryfikować trasowanie TX/keyera w SO2V na sprzęcie.** Potwierdzić
  rzeczywisty tor RF/PTT, brak nadawania na niezamierzonym VFO i zgodność
  modeli; testy programowe nie zastępują tej weryfikacji.
- [ ] **(wymaga sprzętu) Przeprowadzić testy sprzętowe i macierz modeli.** Potwierdzić SO2V,
  strojenie spota, logowanie z obu kontekstów i trasowanie CW na radiu
  referencyjnym z dual-RX; osobno sprawdzić radio z dwoma przełączanymi VFO,
  ale bez jednoczesnego dual-RX, aby zweryfikować bezpieczne ograniczenie trybu.
- [x] **Podstawowe SO2R:** dwa niezależne kontrolery CAT i konfiguracje radii,
  widok stanu A/B, dwa konteksty QSO kierowane do osobnych urządzeń, fokus
  aktywnego radia, routing spotu do wybranego radia, jawny wybór radia CW oraz
  ochrona przed nadawaniem po zmianie trasy keyera bez ponownej konfiguracji.
- [ ] **(wymaga wyboru sprzętu) Rozszerzyć SO2R o audio i przełączanie sprzętowe.** Dodać integrację
  wyłącznie dla określonego, wspieranego kontrolera audio/PTT/anteny; wymaga
  wyboru sprzętu i protokołu. Zapewnić, że awaria/utrata sterownika nie wybierze
  przypadkowego toru nadawczego.
- [ ] **(wymaga sprzętu) Zweryfikować SO2R na sprzęcie.** Sprawdzić dwa niezależne transceivery,
  osobne porty CAT, keyer przypisany do wybranego radia, strojenie spotów,
  utratę połączenia A/B oraz brak przekierowania komend do drugiego radia.
- [x] **Eksport i zabezpieczenie logu:** Cabrillo/ADIF, walidacja przed
  atomowym zapisem, online backup SQLite, dzienne kopie zapasowe,
  kontrola integralności i odtwarzanie zweryfikowanej kopii; starsze wpisy
  z wymianą tekstową są mapowane do pól contestu podczas walidacji/eksportu.
- [x] **CI i pakowanie podstawowe:** Linux/Windows build i testy oraz
  archiwum Linux/Windows; instrukcje buildowania, Hamlib i contest YAML.
- [x] **Układ operatorski inspirowany DXLog:** centralne formularze QSO,
  dolny dock logu, prawa zakładkowa przestrzeń bandmapy/CW/informacji contestu,
  menu Window do zarządzania panelami i zapamiętywanie geometrii/układu.
- [ ] **(wymaga sprzętu) Walidacja sprzętowa/release:** sprawdzić serial,
  `rigctld`, suspend/resume i pakiet Hamlib na fizycznych konfiguracjach
  Windows/Linux; skrypt pakujący Hamlib dla Windows jest dodany, ale nie był
  uruchamiany na docelowym systemie.

## Etap 9 — dodatki

- [x] **Podstawowe narzędzia operatorskie Run/S&P, ESM i Auto CQ.** Zmiana
  częstotliwości radia podczas Run przełącza tryb na Search & Pounce (również
  w obrębie tego samego pasma); tryb można też wybrać z menu/paska i jest
  zapamiętywany. ESM w CW
  wysyła F5 po Enter w Call, a po poprawnym zapisie QSO wysyła F3. Auto CQ
  powtarza F1 wyłącznie w trybie Run/CW i zatrzymuje się po wpisaniu znaku,
  zmianie trybu/emisji, przerwaniu lub utracie keyera. Sekwencje ESM są
  podstawowe i nie konfigurowalne per contest.
- [x] **Nawigacja spotów:** Up/Down wybiera spot, Enter/przycisk stroi aktywne
  radio i kopiuje callsign; dwuklik pozostaje obsługiwany.
- [x] **Pliki pomocnicze callsign:** import CSV/TXT call history, `master.cpy`
  i standardowego `wl_cty.dat`; walidacja, zapamiętanie ścieżek,
  autouzupełnianie, podpowiedzi wymiany i informacyjny lookup CTY.
- [x] **Profile contestów:** dodano starter YAML dla CQWW CW/SSB, CQWPX CW/SSB,
  IARU HF, SPDX, WAG, ARRL DX CW/SSB/10m, EUDX i EUHFC.
- [x] **Nawigacja wymiany:** Space przechodzi callsign → pola odbieranej wymiany
  → callsign; pola wymiany wysyłanej można edytować i są pamiętane per contest
  oraz kontekst wejściowy.
- [x] **Dodać deklaratywne reguły warunkowych wymian i punktacji.**
  `required_when` warunkuje wymagalność odebranego pola, a
  `scoring.qso_point_rules` ustala punkty według pasma, emisji, metadanych CTY
  (DXCC/kraj, kontynent, strefy) i odebranych pól. Format jest opisany w
  `docs/CONTEST_DEFINITIONS.md` i objęty testami.
- [x] **Dopracować profile według wskazanych regulaminów.** Uzupełniono
  wymiany, punkty ARRL 10 m, mnożniki CQ WPX i warunki wymian IARU; źródła oraz
  ograniczenia punktacji są opisane w odpowiednich profilach. SPDX 2026 ma
  osobne YAML dla stacji polskich i zagranicznych, punktację oraz mnożniki
  per regulamin i jawne wskazanie potrzeby CTY dla DXCC.
- [ ] **Dokończyć punktację i mnożniki reguł sponsorów.** Silnik musi umieć
- [x] **Dokończyć punktację i mnożniki reguł sponsorów.** Silnik porównuje
  encje DXCC/kraje i kontynenty obu stacji, obsługuje mnożniki `country`/`dxcc`
  rozdzielane per emisja oraz alternatywne pola wymiany `sent.when` zależne od
  lokalizacji własnej stacji; zakres jest opisany i pokryty testami.
