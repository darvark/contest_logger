# TODO — dalszy rozwój

Zrealizowane funkcje są zebrane w [FUNKCJONALNOSCI.md](FUNKCJONALNOSCI.md). Ta lista obejmuje wyłącznie dalsze prace i znane ograniczenia.

Priorytety:

- **P1** — poprawność danych lub zachowanie wymagane do bezpiecznej pracy.
- **P2** — istotne usprawnienia przed szerszym użyciem.
- **P3** — dalszy zakres rozwoju loggera.

## P1 — poprawność i spójność danych

- [x] **Dodać testy regresyjne dla wszystkich wbudowanych definicji.** Każda z 20 definicji YAML ma testowy QSO z oczekiwanym wynikiem punktowym; trzy przykłady (`general-logging`, `grid-field-day`, `serial-sprint`) są demonstracyjne, a nie certyfikowane regulaminowo. To pokrycie zachowania implementacji, nie niezależna certyfikacja zgodności.
- [ ] **Zweryfikować definicje contestów z aktualnymi regulaminami.** Audyt profili i źródeł wykazał i naprawił reguły punktacji IARU HF, EUDX i WAG; dodano przypadki referencyjne oraz obsługę mnożnika z pierwszej litery DOK (w tym specjalnych DOK z cyframi na początku i wykluczeniem NM). Dodano role-specific profile ARRL DX dla entrantów W/VE (stan/prowincja, moc i DXCC per band); dotychczasowe profile pozostają dla entrantów DX. Testy sprawdzają wiersz QSO Cabrillo dla wszystkich 20 YAML-i oraz klucz duplikatu. Nie jest to pełna certyfikacja: do zrealizowania pozostają modelowanie typów wymiany i mnożników ARRL 10m, kody EU-region/DXCC EUDX, ograniczenia kategorii IARU, niezależne przypadki wszystkich gałęzi punktacji/mnożników oraz potwierdzenie eksportu przez organizatorów. Szczegółowy status i źródła: [audyt regulaminów](docs/CONTEST_RULE_AUDIT.md). Źródła: [ARRL IARU HF](https://contests.arrl.org/ContestRules/IARU-HF-Rules.pdf), [EUDX](https://www.eudx-contest.com/rules/), [DARC WAG 2026](https://www.darc.de/der-club/referate/conteste/wag-contest/en/rules/).
- [ ] **Rozszerzać silnik reguł contestowych tylko pod zweryfikowane przypadki.** Dodano porównanie własnej i odebranej strefy ITU, warunek obecności pola wymiany, mnożnik z pierwszej litery odebranej wymiany oraz listę wykluczanych wartości dla DXCC/country multiplier. Zastosowano je w IARU/EUDX/WAG i role-specific profilach W/VE ARRL DX. Nadal wymagany jest jawny model klasy wymiany/mnożnika i kategorii entrantów dla ARRL 10m; nie implementować tego na podstawie przybliżeń. Pozostałe mechanizmy (ograniczenia zmian pasma i kategorii) dodawać dopiero dla konkretnych przypadków regulaminowych, z walidacją definicji i zachowaniem istniejących logów.
- [x] **Dodać audyt i cofanie lokalnych zmian QSO.** Utworzenie, edycja, unieważnienie i usunięcie są zapisywane w lokalnym audycie; undo odtwarza poprzedni stan jako nową wersję i operację outboxa. Historia sprzed migracji oraz zmiany stosowane bezpośrednio z synchronizacji nie są cofane przez tę funkcję.

## P2 — obsługa operatorska i niezawodność

- [x] **Rozszerzyć katalog definicji zawodów.** Dodano profile ARRL November Sweepstakes CW i Phone (SSB), oparte na regulaminie 2026 i oficjalnej stronie z listą 85 mnożników. Testy sprawdzają punktację, wymianę/sekcje i Cabrillo bez RST. Ograniczenia profili (m.in. limit czasu pracy i kwalifikacja stacji) są opisane w definicjach; dalsze profile wymagają analogicznej weryfikacji.
- [ ] **Dodać QTC dla zawodów, które go wymagają.** Zaprojektować osobny przepływ nadawania i odbioru QTC z walidacją, trwałym zapisem, obsługą numeracji i czasu oraz testami eksportu i synchronizacji. Uzgodnić zakres obsługiwanych regulaminów przed implementacją.
- [ ] **Dodać opcjonalny broadcast QSO w formacie N1MM UDP.** Umożliwić zewnętrznym narzędziom odbiór zalogowanych QSO, z konfigurowalnym adresem docelowym, jawnym włączeniem i testami formatu oraz zachowania przy braku odbiorcy. Rozważyć broadcast całego logu jako oddzielną, ręcznie uruchamianą operację.
- [x] **Ułatwić wyszukiwanie i przeglądanie logu.** Dodano wyszukiwanie po znaku i wymianie oraz filtry pasma, emisji i ważności. Wyniki zachowują chronologię; dopasowania są liczone w całym aktywnym logu, a renderowana tabela jest ograniczona do 1000 najnowszych pasujących QSO.
- [x] **Uzupełnić obsługę QSO o podpowiedzi.** Pole Call pokazuje sugestie po prefiksie znaku z Master.SCP/MASTER.CPY, call history i zalogowanych QSO; tekst zmienia się dopiero po wybraniu sugestii. Usuwanie QSO z potwierdzeniem oraz undo z audytem są dostępne; ograniczenia cofania opisuje [instrukcja](docs/USER_GUIDE.md).
- [ ] **Zweryfikować potwierdzanie CW przez Hamlib na sprzęcie.** Programowe testy obejmują fallback Dummy i abort/disconnect. Sprawdzić polling PTT, abort oraz rozłączenie na modelach używających ogólnego `wait_morse`; zweryfikować osobno niestandardowe callbacki.
- [ ] **Ocenić i dodać obsługę WinKeyer.** Wybrać model/protokół, sprawdzić wymagania transmisji szeregowej i dostępność sprzętu testowego, a następnie dodać provider z testami abortu, kolejki, rozłączenia i konfiguracji prędkości.
- [ ] **Zweryfikować CAT i pakowanie Hamlib na docelowych platformach.** Uruchomić skrypt pakowania Windows na Windowsie i sprawdzić fizyczne porty szeregowe, firmware oraz zachowanie po suspend/resume na wspieranych platformach.

## P2 — synchronizacja sieciowa i eksploatacja
- [ ] **Rozszerzyć testy awaryjne synchronizacji.** Tryb trusted ma test restartu serwera z trwałym outboxem, ponownego wysłania operacji i zgodności kursora/QSO trzech klientów, reconnectu po niedostępności, utraty odpowiedzi po commicie oraz odrzucenia batcha z późniejszym retry. Pozostaje odtwarzanie zerwania na kolejnych etapach protokołu i restartu w trakcie aktywnego transferu/snapshotu.
- [ ] **Zmierzyć wydajność kolejki synchronizacji.** Test integracyjny rejestruje pomiar lokalny dla dwóch operacji w jednym żądaniu (ok. 52 ms w bieżącym przebiegu). To nie jest jeszcze porównywalny baseline. Zebrać czasy i liczbę żądań/błędów dla kolejek 1/8/16 oraz kontrolowanych opóźnień sieciowych, porównać z wysyłką pojedynczą i zmierzyć opóźnienie jednego QSO.
- [ ] **Dodać okresowy, nieblokujący audyt spójności.** Ręczny audyt jest dostępny. Automatyczne uruchamianie pozostaje odłożone do pomiaru kosztu na większych logach i weryfikacji zachowania w pilotażu; po nich można ustalić okres oraz warunek bezczynności.
- [ ] **Zmierz skalowanie limitu połączeń serwera.** Test sprawdza limit dwóch jednoczesnych klientów, odmowę nadmiarowego połączenia i zwolnienie miejsca po rozłączeniu. Nadal należy mierzyć rosnącą liczbę klientów i równoczesne synchronizacje, stabilność, czas zapisu, pamięć oraz zachowanie SQLite przy docelowej skali.
- [ ] **Przeprowadzić pilotaż zmian wydajnościowych i skali.** W docelowej LAN/VPN sprawdzić długą kolejkę, batchowanie, audyt spójności, kilka jednoczesnych klientów, utratę sieci i restart hosta. Potwierdzić zgodność QSO, brak utraty operacji i wyniki pomiarów przed zwiększeniem limitu lub uznaniem zmian za gotowe do zawodów.

## P3 — dalszy rozwój i weryfikacja sprzętowa

- [ ] **Rozwinąć Advanced SO2R.** Po wyborze i walidacji wspieranych kontrolerów dodać konfigurowalne scenariusze dwóch radii oraz opcjonalne przypisanie osobnych klawiatur do paneli. Zapewnić bezpieczne zachowanie przy utracie sterownika, zmianie fokusu lub błędzie routingu; nie przełączać torów ani nie nadawać bez jawnie skonfigurowanej ścieżki.
- [ ] **Dodać obsługę PTT, DVK i nagrywania audio.** Ustalić wspierane interfejsy i integrację z radiem lub zewnętrznym kontrolerem; zapewnić przerwanie transmisji i bezpieczne wyłączenie PTT przy błędzie, rozłączeniu lub zamknięciu aplikacji. Osobno ocenić nagrywanie RX/TX, audio stereo dla dwóch radii i ewentualne TCI.
- [ ] **Zweryfikować SO2V na sprzęcie.** Potwierdzić rzeczywisty tor RF/PTT, trasowanie TX/keyera oraz brak nadawania na niezamierzonym VFO. Przygotować macierz modeli: radio z dual-RX oraz radio z dwoma VFO bez jednoczesnego dual-RX.
- [ ] **Dodać sprzętowe przełączanie SO2R.** Po wyborze wspieranego kontrolera i protokołu zaimplementować integrację audio/PTT/anteny z bezpiecznym zachowaniem przy awarii lub utracie sterownika.
- [ ] **Zweryfikować SO2R na sprzęcie.** Sprawdzić dwa transceivery i porty CAT, przypisanie keyera, strojenie spotów, utratę połączenia A/B oraz brak przekierowania komend do drugiego radia.
- [ ] **Ocenić widmo radia jako źródło bandmapy.** Ustalić, czy warto dodać integrację ze źródłem widma (np. FlexRadio), wybrać wspierane API i urządzenia oraz sprawdzić obciążenie, opóźnienie i zachowanie po utracie połączenia.
- [ ] **Przygotować interfejs do lokalizacji.** Przenieść teksty interfejsu do mechanizmu tłumaczeń Qt, dodać co najmniej polski i angielski oraz sprawdzić przełączanie języka i kompletność tłumaczeń.
- [ ] **Przeprowadzić pilotaż synchronizacji wielostacyjnej.** Przetestować kolejno dwie i trzy stacje, zmierzyć opóźnienia oraz zużycie zasobów, zweryfikować utratę sieci i restart hosta; przed szerszym wdrożeniem ustalić kryteria wydania i kontrolowany rollout.
