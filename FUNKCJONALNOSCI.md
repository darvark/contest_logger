# Funkcjonalności programu

Spis funkcji dostępnych w obecnej wersji Contest Logger. Funkcje wymagające fizycznego radia lub zewnętrznego kontrolera nie są tu traktowane jako zweryfikowane sprzętowo. Ograniczenia i prace do wykonania są zebrane w [TODO.md](TODO.md).

## Logowanie i contesty

- Definicje contestów w YAML z walidacją schematu, pól wymiany, zakresów, wzorców i reguł.
- Logowanie QSO z walidacją callsign, dupe-checkiem, numeracją serial odtwarzaną z zapisanych wartości oraz edycją QSO. Opcjonalnie można zapisać dupe; taki kontakt ma 0 punktów i nie przyznaje mnożnika.
- Ustawienie **Operating → Allow logging duplicate QSOs** jest domyślnie wyłączone; po włączeniu pozwala zapisywać i edytować dupe bez naliczania punktów ani nowych mnożników.
- Edycja czasu UTC, callsign, częstotliwości, pasma, trybu i wysłanej/odebranej wymiany; po edycji ponownie sprawdzane są reguły i przeliczane wartości.
- Zachowanie wymian w ustrukturyzowanych polach SQLite, migracja starszych wpisów oraz fallback dla historycznych danych tekstowych.
- Punktacja warunkowa przez `qso_point_rules` i wymagalność pól przez `required_when`.
- Mnożniki `exchange`, `prefix`, `dxcc`, `country`, `cq_zone`, `itu_zone` i `continent`; dane CTY z `wl_cty.dat` pozwalają rozwiązywać kraj, DXCC, kontynent i strefy. Stan mnożników jest odtwarzany z logu i prezentowany w tabeli oraz podsumowaniu.
- Starterowe definicje contestów, m.in. CQ WW, CQ WPX, IARU HF, SP DX, WAG, ARRL DX, EU DX i EU HF Championship. Są to profile startowe, nie certyfikacja zgodności z każdą edycją regulaminu.

## Radio i formularze

- CAT przez Hamlib (radio szeregowe lub rigctld/TCP) oraz backend Mock do pracy i testów bez sprzętu.
- Walidacja konfiguracji Hamlib, jednorazowy test połączenia, zapis/połączenie/rozłączenie, automatyczne łączenie przy starcie oraz monitoring stanu CAT.
- Synchronizacja częstotliwości z pasmem formularza; zmiana trybu z radia nie nadpisuje ręcznego wyboru bez zmiany trybu po stronie radia. Nieaktualny odczyt CAT jest sygnalizowany.
- SO2V: dwa konteksty QSO dla VFO A/B, niezależne częstotliwości i wybór RX focus. Dostępność dual-RX jest sygnalizowana osobno od samego dual-VFO.
- Podstawowe SO2R: dwa niezależne kontrolery CAT, dwa konteksty QSO kierowane do osobnych radii, wybór aktywnego radia, routowanie spotu oraz jawny wybór radia CW.
- Trasowanie CW w SO2V wiąże wiadomość z docelowym VFO; Hamlib dostaje jawne A/B, a cwdaemon blokuje nadawanie na VFO B. Nie potwierdza to rzeczywistego toru RF/PTT na konkretnym radiu.

## CW i obsługa operatorska

- Keyer Hamlib, cwdaemon/UDP i Mock; kolejka wiadomości, ustawianie WPM oraz przerwanie/anulowanie kolejki.
- Koniec nadawania potwierdzają cwdaemon, Mock i Hamlib przez nieblokujące odczyty PTT dla backendów z ogólnym `wait_morse`. Niestandardowe callbacki Hamlib korzystają z estymowanego czasu.
- Run i Search & Pounce, zapamiętywanie trybu i przełączenie na S&P po zmianie częstotliwości w Run.
- ESM w CW: osobne, edytowalne sekwencje wiadomości dla Enter w polu Call i poprawnego zapisu QSO; konfiguracja jest zapamiętywana per contest. Domyślne treści odpowiadają F5 i F3.
- Auto CQ powtarza F1 w trybie Run/CW i zatrzymuje się m.in. po wpisaniu callsign, zmianie trybu/emisji, przerwaniu lub utracie keyera.
- Edytowalne wiadomości F1-F8; makra obejmują callsign, znak własny, operatora, WPM, numer serial/NR, wysłaną wymianę i jej pola.
- Nawigacja spotów klawiszami, strojenie aktywnego radia i kopiowanie callsign do formularza.

## DX Cluster i dane pomocnicze

- Pojedyncze połączenie TCP/Telnet z obsługą spotów, ponownym łączeniem i parserem czasu uwzględniającym przejście przez północ.
- Bandmapa z filtrem pasma, sortowaniem, ukrywaniem zrobionych łączności, wyborem spotu i strojeniem wybranego radia.
- Spoty są przechowywane w SQLite i usuwane według retencji oraz limitu liczby rekordów.
- Import call history CSV/TXT, `master.cpy` i `wl_cty.dat`; walidacja danych, zapamiętanie ścieżek, sprawdzanie częściowych znaków, podpowiedzi wymiany i lookup CTY.

## Eksport, kopie i interfejs

- Eksport logu do ADIF i Cabrillo z walidacją przed zapisem atomowym.
- Kopie zapasowe SQLite, kontrola integralności oraz odtwarzanie zweryfikowanej kopii.
- Układ operatorski z formularzami QSO, tabelą logu i panelami bandmapy, CW oraz informacji contestu; panele można zarządzać z menu, a ich układ jest zapamiętywany.
- Dwa panele wejściowe dla SO2V/SO2R, obsługa klawiatury i wskaźniki dupe/mult.

## Zakres potwierdzony testami

- Testy Qt obejmują logowanie i edycję QSO przez UI, walidację, duplikaty i odświeżanie tabeli.
- CAT można testować bez radia przez Mock, Hamlib Dummy i lokalny `rigctld`; DX Cluster ma testy z lokalnym symulatorem Telnet.
- CI buduje i testuje projekt na Linuxie i Windowsie. Testy programowe nie zastępują prób na fizycznym radiu, portach szeregowych ani kontrolerach audio/PTT.
