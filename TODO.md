# TODO — dalszy rozwój

Zrealizowane funkcje są zebrane w [FUNKCJONALNOSCI.md](FUNKCJONALNOSCI.md). Ta lista obejmuje wyłącznie dalsze prace i znane ograniczenia.

Priorytety:

- **P1** — poprawność danych lub zachowanie wymagane do bezpiecznej pracy.
- **P2** — istotne usprawnienia przed szerszym użyciem.
- **P3** — dalszy zakres rozwoju loggera.

Zadania są podzielone według zależności od sprzętu: sekcja programowa obejmuje prace możliwe do wykonania i testowania bez podłączania radia, a sekcja sprzętowa wymaga urządzenia, kontrolera lub walidacji na rzeczywistej stacji. Część zadań w tej drugiej sekcji ma również etap programistyczny.

## Zmiany programowe i sieciowe

### P1 — poprawność i spójność danych

- [ ] **Zweryfikować definicje contestów z aktualnymi regulaminami.** Audyt profili i źródeł wykazał i naprawił reguły punktacji IARU HF, EUDX i WAG; dodano przypadki referencyjne oraz obsługę mnożnika z pierwszej litery DOK (w tym specjalnych DOK z cyframi na początku i wykluczeniem NM). Dodano role-specific profile ARRL DX dla entrantów W/VE (stan/prowincja, moc i DXCC per band); dotychczasowe profile pozostają dla entrantów DX. Testy sprawdzają wiersz QSO Cabrillo dla wszystkich 23 YAML-i oraz klucz duplikatu. Nie jest to pełna certyfikacja: do zrealizowania pozostają modelowanie typów wymiany i mnożników ARRL 10m, kody EU-region/DXCC EUDX, ograniczenia kategorii IARU, niezależne przypadki wszystkich gałęzi punktacji/mnożników, pełna punktacja i eksport WAE oraz potwierdzenie eksportu przez organizatorów. Szczegółowy status i źródła: [audyt regulaminów](docs/CONTEST_RULE_AUDIT.md). Źródła: [ARRL IARU HF](https://contests.arrl.org/ContestRules/IARU-HF-Rules.pdf), [EUDX](https://www.eudx-contest.com/rules/), [DARC WAG 2026](https://www.darc.de/der-club/referate/conteste/wag-contest/en/rules/).
- [ ] **Rozszerzać silnik reguł contestowych tylko pod zweryfikowane przypadki.** Dodano porównanie własnej i odebranej strefy ITU, warunek obecności pola wymiany, mnożnik z pierwszej litery odebranej wymiany oraz listę wykluczanych wartości dla DXCC/country multiplier. Zastosowano je w IARU/EUDX/WAG i role-specific profilach W/VE ARRL DX. Nadal wymagany jest jawny model klasy wymiany/mnożnika i kategorii entrantów dla ARRL 10m; nie implementować tego na podstawie przybliżeń. Pozostałe mechanizmy (ograniczenia zmian pasma i kategorii) dodawać dopiero dla konkretnych przypadków regulaminowych, z walidacją definicji i zachowaniem istniejących logów.

### P2 — synchronizacja sieciowa i eksploatacja

- [ ] **Udostępnić uwierzytelniony tryb synchronizacji w GUI i serwerze CLI.** Bieżący trusted TCP nie szyfruje ruchu ani nie uwierzytelnia klientów; role i ścieżka TLS/token istnieją tylko jako niedostępny wariant legacy. Ocenić możliwość bezpiecznego dokończenia tej ścieżki, skonfigurować ją w obu aplikacjach i przetestować role read/write/admin, odrzucanie niezaufanych klientów oraz migrację konfiguracji. Do czasu wdrożenia nie przedstawiać trusted TCP jako trybu bezpiecznego poza zaufaną LAN/VPN.
- [ ] **Rozszerzyć testy awaryjne synchronizacji.** Tryb trusted ma test restartu serwera z trwałym outboxem, ponownego wysłania operacji i zgodności kursora/QSO trzech klientów, reconnectu po niedostępności, utraty odpowiedzi po commicie oraz odrzucenia batcha z późniejszym retry. Pozostaje odtwarzanie zerwania na kolejnych etapach protokołu i restartu w trakcie aktywnego transferu/snapshotu.
- [ ] **Zmierzyć wydajność kolejki synchronizacji.** Test integracyjny rejestruje pomiar lokalny dla dwóch operacji w jednym żądaniu (ok. 52 ms w bieżącym przebiegu). To nie jest jeszcze porównywalny baseline. Zebrać czasy i liczbę żądań/błędów dla kolejek 1/8/16 oraz kontrolowanych opóźnień sieciowych, porównać z wysyłką pojedynczą i zmierzyć opóźnienie jednego QSO.
- [ ] **Dodać okresowy, nieblokujący audyt spójności.** Ręczny audyt jest dostępny. Automatyczne uruchamianie pozostaje odłożone do pomiaru kosztu na większych logach i weryfikacji zachowania w pilotażu; po nich można ustalić okres oraz warunek bezczynności.
- [ ] **Zmierz skalowanie limitu połączeń serwera.** Test sprawdza limit dwóch jednoczesnych klientów, odmowę nadmiarowego połączenia i zwolnienie miejsca po rozłączeniu. Nadal należy mierzyć rosnącą liczbę klientów i równoczesne synchronizacje, stabilność, czas zapisu, pamięć oraz zachowanie SQLite przy docelowej skali.
- [ ] **Przeprowadzić pilotaż zmian wydajnościowych i skali.** W docelowej LAN/VPN sprawdzić długą kolejkę, batchowanie, audyt spójności, kilka jednoczesnych klientów, utratę sieci i restart hosta. Potwierdzić zgodność QSO, brak utraty operacji i wyniki pomiarów przed zwiększeniem limitu lub uznaniem zmian za gotowe do zawodów.

### P3 — rozwój

- [ ] **Rozwinąć odbiór emisji cyfrowych po sprawdzeniu integracji MMTTY.** Ocenić przenośny interfejs do dekoderów (np. FLdigi) oraz wiele okien RX, podświetlanie dupe/mnożników, przechwytywanie znaku i wymiany kliknięciem i kolejkę odebranych znaków. Przechwycenie ma wypełniać formularz, nigdy samodzielnie zatwierdzać QSO; zachować działanie bez dekodera i testować osobno integracje platformowe.
- [ ] **Uzupełnić bibliotekę profili zawodów według zweryfikowanego zapotrzebowania.** Zidentyfikować brakujące, często używane zawody, priorytetyzować je na podstawie potrzeb operatorów i dodać profile dopiero po weryfikacji aktualnych regulaminów, wymiany, punktacji, mnożników i eksportu
- [ ] **Uzupełnić polski katalog tłumaczeń.** Przetłumaczyć pozostałe komunikaty, okna dialogowe i podpowiedzi oraz dodać walidację pokrycia katalogu; nieprzetłumaczone teksty obecnie pozostają po angielsku.
- [ ] **Przeprowadzić pilotaż synchronizacji wielostacyjnej.** Przetestować kolejno dwie i trzy stacje, zmierzyć opóźnienia oraz zużycie zasobów, zweryfikować utratę sieci i restart hosta; przed szerszym wdrożeniem ustalić kryteria wydania i kontrolowany rollout.

## Zmiany wymagające sprzętu lub walidacji na stacji

### P2 — obsługa operatorska i niezawodność

- [ ] **Zweryfikować potwierdzanie CW przez Hamlib na sprzęcie.** Programowe testy obejmują fallback Dummy i abort/disconnect. Sprawdzić polling PTT, abort oraz rozłączenie na modelach używających ogólnego `wait_morse`; zweryfikować osobno niestandardowe callbacki.
- [ ] **Ocenić i dodać obsługę WinKeyer.** Wybrać model/protokół, sprawdzić wymagania transmisji szeregowej i dostępność sprzętu testowego, a następnie dodać provider z testami abortu, kolejki, rozłączenia i konfiguracji prędkości.
- [ ] **Zweryfikować CAT i pakowanie Hamlib na docelowych platformach.** Uruchomić skrypt pakowania Windows na Windowsie i sprawdzić fizyczne porty szeregowe, firmware oraz zachowanie po suspend/resume na wspieranych platformach.
- [ ] **Wykonać proof of concept integracji MMTTY na Windows.** MMTTY Engine udostępnia zdalne sterowanie przez komunikaty Win32, m.in. znaki RX/TX i sterowanie PTT/TX; nie jest to przenośny interfejs Qt ani API sieciowe. Najpierw zweryfikować z aktualnym MMTTY Engine 1.68a odbiór i kierunek znaków, uruchamianie/kończenie procesu, podtrzymanie komunikacji oraz współdzielenie zasobów audio/COM. Prototyp powinien przekazywać odebrany tekst do pól loggera bez automatycznego zatwierdzania QSO; nadawanie i dystrybucję SDK rozważyć dopiero po sprawdzeniu warunków licencji i testach sprzętowych. Szczegóły oraz źródła: [instrukcja użytkownika](docs/USER_GUIDE.md#rtty-i-mmtty).

### P3 — rozwój i weryfikacja sprzętowa

- [ ] **Rozwinąć Advanced SO2R.** Po wyborze i walidacji wspieranych kontrolerów dodać konfigurowalne scenariusze dwóch radii oraz opcjonalne przypisanie osobnych klawiatur do paneli. Zapewnić bezpieczne zachowanie przy utracie sterownika, zmianie fokusu lub błędzie routingu; nie przełączać torów ani nie nadawać bez jawnie skonfigurowanej ścieżki.
- [ ] **Dodać obsługę PTT, DVK i nagrywania audio.** Ustalić wspierane interfejsy i integrację z radiem lub zewnętrznym kontrolerem; zapewnić przerwanie transmisji i bezpieczne wyłączenie PTT przy błędzie, rozłączeniu lub zamknięciu aplikacji. Osobno ocenić nagrywanie RX/TX, audio stereo dla dwóch radii i ewentualne TCI.
- [ ] **Zweryfikować SO2V na sprzęcie.** Potwierdzić rzeczywisty tor RF/PTT, trasowanie TX/keyera oraz brak nadawania na niezamierzonym VFO. Przygotować macierz modeli: radio z dual-RX oraz radio z dwoma VFO bez jednoczesnego dual-RX.
- [ ] **Dodać sprzętowe przełączanie SO2R.** Po wyborze wspieranego kontrolera i protokołu zaimplementować integrację audio/PTT/anteny z bezpiecznym zachowaniem przy awarii lub utracie sterownika.
- [ ] **Zweryfikować SO2R na sprzęcie.** Sprawdzić dwa transceivery i porty CAT, przypisanie keyera, strojenie spotów, utratę połączenia A/B oraz brak przekierowania komend do drugiego radia.
- [ ] **Ocenić widmo radia jako źródło bandmapy.** Ustalić, czy warto dodać integrację ze źródłem widma (np. FlexRadio), wybrać wspierane API i urządzenia oraz sprawdzić obciążenie, opóźnienie i zachowanie po utracie połączenia.
- [ ] **Ocenić sterowanie rotatorem i obliczenia kierunku.** Ustalić wspierane kontrolery/protokoły, wymagane dane lokalizacyjne i zakres (ustawienie kierunku, obrót do stacji, krótka/długa ścieżka); przygotować testy symulacyjne, a sprzętową weryfikację przeprowadzić przed deklarowaniem obsługi.

## Wnioski z porównania loggerów

Contest Logger ma już istotne podstawy wspólne z N1MM Logger+ i DXLog.net: logowanie contestowe, ESM/makra CW, CAT, SO2V/SO2R w podstawowym zakresie, bandmapę, spoty DX Cluster, punktację/mnożniki oraz eksport Cabrillo/ADIF. Wyróżniają go m.in. wieloplatformowość, definicje YAML, lokalne kopie i audyt zmian oraz implementowana synchronizacja z trwałym outboxem. Najbardziej widoczne luki funkcjonalne względem opisanych możliwości porównywanych programów to rozbudowany odbiór emisji cyfrowych, wieloźródłowe spotowanie/skimmery, czytelny przegląd brakujących mnożników na pasmach, zaawansowane sterowanie SO2R i urządzeniami stacji oraz szersza biblioteka zweryfikowanych profili. To kierunki do oceny i priorytetyzacji, nie zobowiązanie do pełnej zgodności funkcji.

Źródła producentów: [N1MM Logger+ — lista funkcji](https://n1mmwp.hamdocs.com/n1mm-features/), [DXLog — okno główne](https://dxlog.net/docs/index.php?title=Main_Window), [DXLog — dokumentacja SO2R](https://dxlog.net/docs/index.php?title=Special:Search&search=SO2R), [DXLog — dokumentacja DX Cluster/skimmerów](https://dxlog.net/docs/index.php?title=Special:Search&search=Skimmer+telnet+cluster), [DXLog — dokumentacja trybów cyfrowych](https://dxlog.net/docs/index.php?title=Special:Search&search=RTTY+digital+support).
