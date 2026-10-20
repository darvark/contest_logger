# TODO — dalszy rozwój

Zrealizowane funkcje są zebrane w [FUNKCJONALNOSCI.md](FUNKCJONALNOSCI.md). Ta lista obejmuje wyłącznie dalsze prace i znane ograniczenia.

Priorytety:

- **P1** — poprawność danych lub zachowanie wymagane do bezpiecznej pracy.
- **P2** — istotne usprawnienia przed szerszym użyciem.
- **P3** — dalszy zakres rozwoju loggera.

## P1 — poprawność i spójność danych

Brak otwartych pozycji.

## P2 — obsługa operatorska i niezawodność

- [ ] **Zweryfikować potwierdzanie CW przez Hamlib na sprzęcie.** Programowe testy obejmują fallback Dummy i abort/disconnect. Sprawdzić polling PTT, abort oraz rozłączenie na modelach używających ogólnego `wait_morse`; zweryfikować osobno niestandardowe callbacki.
- [ ] **Ocenić obsługę WinKeyer.** Wybrać model/protokół i sprawdzić wymagania transmisji szeregowej oraz dostępność sprzętu testowego przed dodaniem providera.
- [ ] **Zweryfikować CAT i pakowanie Hamlib na docelowych platformach.** Uruchomić skrypt pakowania Windows na Windowsie i sprawdzić fizyczne porty szeregowe, firmware oraz zachowanie po suspend/resume na wspieranych platformach.

## P2 — synchronizacja sieciowa i eksploatacja

- [x] **Dodać listę podłączonych stacji w oknie statusu sieci.** Tabela pokazuje nazwę komputera, znak stacji, operatora i stan aktywnego logu zamiast UUID, wyróżnia własną stację i odświeża się co 5 sekund lub ręcznie. UUID pozostają identyfikatorami protokołu. Dostępna dla Clienta z uprawnieniem `read` i lokalnego GUI Host; testy obejmują listę, zmianę operatora, walidację opisów, rozłączenie oraz odświeżanie.
- [x] **Dodać retencję i kompaktowanie zdarzeń synchronizacji.** Serwer utrzymuje 30-dniowy okres retencji, usuwa tylko ciągły prefiks potwierdzony przez niedawno aktywne stacje, a starszych klientów kieruje do snapshotu; testy obejmują ochronę cursorów i snapshot po kompaktowaniu.
- [x] **Dodać powiadomienia push o nowych zdarzeniach.** Serwer wysyła `event_available` do subskrybujących klientów; klient natychmiast pobiera zmiany przez potwierdzoną ścieżkę `get_changes`/ack, a polling pozostaje fallbackiem.
- [x] **Dodać ręczne scalanie konfliktów QSO.** Operator wybiera lokalną lub serwerową wartość każdego pola QSO; wynik jest atomowo zapisywany z nowym outboxem względem bieżącej wersji serwera, a wybór i obie wersje trafiają do audytu.
- [x] **Dokończyć obsługę numerów seryjnych.** Admin może korygować niewykorzystaną rezerwację po potwierdzeniu, z uzasadnieniem i audytem; widok sieci pokazuje cache i żądania oczekujące/offline. Polityka: nie przydzielać nowych numerów bez serwera; istniejąca potwierdzona rezerwacja pozostaje używalna offline, a pula offline jest wyłączona.
- [x] **Utrwalić status kontaktu i rozszerzyć diagnostykę.** Czas kontaktu klienta i heartbeat są zapisywane trwale; admin otrzymuje per-stacja liczniki pending/error/conflict, lag kursora i agregaty czasu transakcji. Historia transakcji jest ograniczona do 1000 rekordów na log, a eksport diagnostyczny może dołączyć ostatnio pobrane metryki serwera.
- [x] **Uzupełnić bezpieczną konfigurację sieci.** Tokeny Clienta można zapisać w Windows Credential Manager lub Linux Secret Service; dodano dodatkowe CA i opt-in autostart Client/Host, pozostawiając Local jako domyślny. Konfiguracja Host nadal korzysta z zewnętrznego pliku poświadczeń zarządzanego przez administratora.
- [x] **Opisać operacje i odzyskiwanie serwera.** Dodano procedurę instalacji, portów/firewalla, TLS, rotacji tokenów, backupu/restore, aktualizacji protokołu i odzyskania po utracie hosta w [docs/NETWORKING_OPERATIONS.md](docs/NETWORKING_OPERATIONS.md). Serwer CLI obsługuje `--restore-backup`; test sprawdza rewizję, QSO, rezerwację serialu, wynik replay idempotentnego oraz zachowanie poprzedniej bazy. Próbę operatorską na docelowym środowisku należy wykonać przed produkcyjnym wdrożeniem.
- [ ] **Rozszerzyć testy awaryjne synchronizacji.** Odtworzyć zerwanie połączenia na kolejnych krokach protokołu, restart serwera w trakcie transferu oraz sprawdzić zgodność replik po wznowieniu.

## P3 — dalszy rozwój i weryfikacja sprzętowa

- [ ] **Zweryfikować SO2V na sprzęcie.** Potwierdzić rzeczywisty tor RF/PTT, trasowanie TX/keyera oraz brak nadawania na niezamierzonym VFO. Przygotować macierz modeli: radio z dual-RX oraz radio z dwoma VFO bez jednoczesnego dual-RX.
- [ ] **Dodać sprzętowe przełączanie SO2R.** Po wyborze wspieranego kontrolera i protokołu zaimplementować integrację audio/PTT/anteny z bezpiecznym zachowaniem przy awarii lub utracie sterownika.
- [ ] **Zweryfikować SO2R na sprzęcie.** Sprawdzić dwa transceivery i porty CAT, przypisanie keyera, strojenie spotów, utratę połączenia A/B oraz brak przekierowania komend do drugiego radia.
- [ ] **Przeprowadzić pilotaż synchronizacji wielostacyjnej.** Przetestować kolejno dwie i trzy stacje, zmierzyć opóźnienia oraz zużycie zasobów, zweryfikować utratę sieci i restart hosta; przed szerszym wdrożeniem ustalić kryteria wydania i kontrolowany rollout.
