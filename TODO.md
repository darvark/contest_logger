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
- [ ] **Rozszerzyć DX Cluster.** Dodać wysyłanie spotów/komend, filtry po emisji i kontynencie oraz obsługę wielu równoległych połączeń.
- [ ] **Zweryfikować CAT i pakowanie Hamlib na docelowych platformach.** Uruchomić skrypt pakowania Windows na Windowsie i sprawdzić fizyczne porty szeregowe, firmware oraz zachowanie po suspend/resume na wspieranych platformach.
- [ ] **Uaktualnić dokumentację ograniczeń.** Poprawić sekcję w `docs/USER_GUIDE.md`, która nadal twierdzi, że brak Run/S&P, Auto CQ, wymian warunkowych i mnożników CTY. Sprawdzić również aktualność opisu reguł mnożników i danych CTY w `docs/BUILD_AND_RELEASE.md`.

## P3 — dalszy rozwój i weryfikacja sprzętowa

- [ ] **Zweryfikować SO2V na sprzęcie.** Potwierdzić rzeczywisty tor RF/PTT, trasowanie TX/keyera oraz brak nadawania na niezamierzonym VFO. Przygotować macierz modeli: radio z dual-RX oraz radio z dwoma VFO bez jednoczesnego dual-RX.
- [ ] **Dodać sprzętowe przełączanie SO2R.** Po wyborze wspieranego kontrolera i protokołu zaimplementować integrację audio/PTT/anteny z bezpiecznym zachowaniem przy awarii lub utracie sterownika.
- [ ] **Zweryfikować SO2R na sprzęcie.** Sprawdzić dwa transceivery i porty CAT, przypisanie keyera, strojenie spotów, utratę połączenia A/B oraz brak przekierowania komend do drugiego radia.
- [ ] **Zweryfikować profile contestów z regulaminami.** Starterowe YAML-e nie są certyfikowane dla wszystkich edycji; potwierdzić wymiany, punktację, mnożniki, kategorie Cabrillo i identyfikatory przed użyciem w zawodach.
