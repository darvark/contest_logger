# Dokumentacja wstępna: logger zawodów krótkofalarskich

## Analiza i rekomendacje

**Wniosek w skrócie:** wspólnym mianownikiem tych loggerów jest szybkie logowanie z klawiatury, obsługa reguł zawodów, komunikacja z radiem i dostęp do spotów z DX clusterów. Nie wszystkie programy realizują te możliwości w taki sam sposób. Szczególnie **SO2R/SO2V i kluczowanie CW przez Hamlib nie są funkcjami wspólnymi dla całej czwórki**. Dla nowej aplikacji warto potraktować je jako oddzielne, jawnie konfigurowane tryby oraz warstwy sprzętowe.

## 1. Porównanie aplikacji

| Aplikacja | UI i model pracy | Definicje zawodów | CAT / radio | SO1R / SO2V / SO2R | DX cluster | CW przez Hamlib |
|---|---|---|---|---|---|---|
| **DXLog.net** | Graficzny logger nastawiony na szybkie operowanie klawiaturą. Ma osobne widoki wprowadzania QSO i bandmapy. Dokumentacja wskazuje, że duża elastyczność wiąże się z krzywą uczenia. | Ma pliki definicji zawodów oraz formularz konfiguracji logu. Dokumentacja opisuje walidację pól względem reguł danego zawodu, ale nie wskazuje, by użytkownik definiował zawody w YAML. | Obsługa CAT i wielu modeli radii; komunikacja i urządzenia peryferyjne konfigurowane w interfejsach programu. | Jawnie wymienione **SO1R, SO2V, SO2R i Advanced SO2R**; dla SO2R wymagane są skonfigurowane dwa radia. | Dostępna obsługa klastrów w środowisku programu. | Dokumentacja wskazuje konfigurację CAT i sprzętu do nadawania, ale nie potwierdza, że Hamlib jest uniwersalnym mechanizmem nadawania CW. |
| **N1MM Logger+** | Graficzne okno Entry jest centrum operacyjnym; pozostałe okna, m.in. log, bandmap i cluster, można rozmieszczać osobno. | Wybór i konfiguracja zawodów w programie, z ustawieniami logu oraz plikami powiązanymi. To nie jest udokumentowany model „edytuj definicję zawodu w YAML”. | Obsługa sterowania radiem i typowych interfejsów contestowych. | Udokumentowane **SO2V i SO2R**, z odpowiednimi bandmapami, fokusowaniem radia/VFO i przypisaniami klawiszy. | Osobne okno DX cluster/Telnet. | Manual opisuje serial/parallel keying i Winkeyer; nie należy zakładać, że Hamlib jest natywną ścieżką kluczowania CW. |
| **Not1MM** | Qt i SQLite; UI jest inspirowane układem N1MM, z głównym widokiem pracy i dodatkowymi oknami. Główny cel projektu to Linux i macOS; autor nie deklaruje bezpośredniego wsparcia Windows. | Projekt wymienia wiele obsługiwanych zawodów i udostępnia konfigurację przez GUI. Nie znalazłem podstaw, by uznać plik YAML/XML/INI użytkownika za wspólny format definicji. | CAT przez **rigctld, flrig lub TCI**. | Repozytorium opisuje rozwój Multi-Multi jako pracę w toku; nie traktowałbym tego jako potwierdzenia dojrzałego SO2R/SO2V. | Cluster i bandmap są wymienione w funkcjach. | CW ma osobne interfejsy, m.in. Winkeyer/cwdaemon/SwiftKeyer. Wspomnienie wersji Hamlib dotyczy problemów z wysyłaniem CW w określonych konfiguracjach, nie gwarantuje jednak jednolitej obsługi wszystkich radii przez Hamlib. |
| **TLF** | Lekki program konsolowy w ncurses. Widok pokazuje m.in. stan radia oraz sekcje spotów i bandmapy; interakcja jest zorientowana na klawiaturę. | Pliki tekstowe `logcfg.dat` i pliki `rules` służą do konfiguracji stacji i reguł zawodów. To najbliższy z badanych przykładów konfiguracji reguł w edytowalnych plikach, choć nie jest to YAML/XML/INI. | Sterowanie radiem przez Hamlib; dokumentacja opisuje także konfigurację przez `rigctld`. | W przejrzanej aktualnej dokumentacji nie znalazłem wystarczającego potwierdzenia pełnego SO2R/SO2V — nie zakładam więc jego obsługi. | Wbudowany klient DX cluster. | CW zwykle przez `cwdaemon` lub serwer Winkeyer. Hamlib w dokumentacji TLF dotyczy przede wszystkim sterowania radiem, nie stanowi dowodu na uniwersalne kluczowanie CW przez Hamlib. |

**Ważne rozróżnienie:** „CAT przez Hamlib” i „CW przez Hamlib” to różne wymagania. CAT może obejmować odczyt częstotliwości, trybu i ustawień radia, ale nadawanie CW zależy od możliwości konkretnego modelu, jego backendu Hamlib i sposobu podłączenia klucza. Dlatego warto zaprojektować CW jako osobny interfejs sprzętowy, a Hamlib potraktować jako jeden z możliwych providerów, z alternatywą dla zewnętrznego keyera.

## 2. Wspólne funkcje i elementy wymagające świadomego projektu

### Praktyczny wspólny rdzeń

Wszystkie cztery projekty dotyczą logowania łączności w zawodach i pokazują wartość następujących elementów:

- **Szybkie wprowadzanie QSO** — callsign i pola wymiany w jednym głównym widoku, obsługa klawiatury, szybka korekta danych.
- **Reguły konkretnego zawodu** — pola wymiany, poprawność danych, duplikaty, mnożniki i wynik zależą od wybranego contestu.
- **Integracja z radiem** — w takim czy innym modelu program może współpracować z CAT/radio control.
- **Spoty z DX clusterów** i ich prezentacja w bandmapie lub osobnym oknie.
- **Nadawanie zaprogramowanych wiadomości** oraz dostosowanie ich do rodzaju zawodów.

To są **wspólne cele funkcjonalne**, a nie deklaracja, że każdy logger ma identyczną implementację.

### Nie są wspólne — ale powinny znaleźć się w planie nowej aplikacji

- **Edytowalne definicje zawodów w pliku:** TLF ma pliki `logcfg.dat` i `rules`; DXLog korzysta z definicji reguł i formularza konfiguracji; N1MM i Not1MM przede wszystkim prowadzą użytkownika przez wybór i konfigurację dostępnych contestów. YAML jako format dla użytkownika byłby więc dobrym, czytelnym kierunkiem, ale nie należy opisywać go jako wspólnego wzorca tych programów.
- **SO2R/SO2V:** DXLog i N1MM dokumentują te tryby. Dla TLF nie potwierdziłem ich w aktualnej dokumentacji, a opis rozwoju Multi-Multi w Not1MM nie dowodzi pełnej obsługi SO2R/SO2V.
- **CW przez Hamlib:** badane programy korzystają też z Winkeyera, `cwdaemon` lub portów sprzętowych. Nowa aplikacja powinna mieć abstrakcję keyera, zamiast uzależniać transmisję CW wyłącznie od Hamliba.

## 3. Proponowana architektura

### Technologie

- **C++20 i Qt 6 Widgets** — dojrzały model okien, docków, skrótów klawiszowych i przenośnego UI.
- **Hamlib** — sterowanie radiem oraz, tam gdzie radio/backend to obsługuje, próby wysłania CW.
- **Qt SQL + SQLite** — lokalna baza QSO. SQLite i moduły Qt pozwalają zachować spójny stos dla Linuxa i Windows.
- **Qt Network** — połączenie TCP z serwerem DX cluster; klient musi obsłużyć logowanie i specyfikę komunikatów Telnet.
- **Qt Serial Port** — opcjonalny dostęp do portów szeregowych, np. dla urządzeń/keyerów, jeśli nie będą obsługiwane przez Hamlib lub osobny daemon.
- **CMake** — wspólna konfiguracja budowania; osobne profile zależności i pakowania dla Windows oraz Linuxa.

### Moduły aplikacji

1. **Contest Engine**
   - Odczyt definicji zawodów.
   - Walidacja wejścia, dupe checking, punktacja i mnożniki.
   - Obliczanie oraz eksport Cabrillo i ADIF.
   - Rozdzielenie reguł zawodów od widoku i warstwy bazy.

2. **Contest Definition Loader**
   - Jeden kanoniczny format definicji — rekomenduję **YAML**.
   - Walidacja schematu i komunikaty błędów z nazwą pliku oraz wskazaniem pola.
   - Możliwość aktualizowania definicji bez przebudowy programu.
   - Opcjonalne importery XML/INI dopiero wtedy, gdy pojawi się konkretna potrzeba.

3. **Radio Service**
   - Osobny obiekt/usługa dla każdego radia lub VFO.
   - Odczyt częstotliwości i trybu, ustawianie częstotliwości/trybu, status połączenia.
   - Obsługa bezpośredniej biblioteki Hamlib i opcjonalnego połączenia z zewnętrznym `rigctld`.
   - Praca poza wątkiem UI; sygnały Qt przekazują zmiany do widoków.
   - Odzyskiwanie po utracie CAT i widoczne wskazanie nieaktualnego odczytu.

4. **CW Keyer Service**
   - Interfejs `ICwKeyer` z metodami wysyłania tekstu, przerwania, ustawienia prędkości i statusu.
   - Provider Hamlib z kontrolą dostępności funkcji radia/backendu.
   - Możliwość dołączenia osobnych providerów dla Winkeyera lub `cwdaemon`.
   - Kolejka wiadomości i polityka nadawania przypisana do radia/VFO.

5. **Operating Mode / SO1R–SO2V–SO2R**
   - Wspólny model obiektów: Radio, VFO, odbiornik, nadajnik i fokus operatora.
   - **SO1R:** jedno radio i jedna aktywna ścieżka pracy.
   - **SO2V:** jedno radio, dwa VFO/odbiorniki, jeden nadajnik — zależnie od możliwości sprzętu.
   - **SO2R:** dwa radia, dwa niezależne widoki częstotliwości/bandmapy i kontrola fokusu oraz audio.
   - Nie zakładać, że dwa VFO zawsze oznaczają dwa niezależne nadajniki; walidować możliwości radia.
   - Najpierw dostarczyć poprawny SO1R, potem SO2V, a na końcu pełny SO2R z przełączaniem fokusu i obsługą keyera per radio.

6. **DX Cluster Service**
   - Konfiguracja hosta, portu, loginu i komend inicjalizacyjnych.
   - Parser spotów oraz statusu połączenia.
   - Filtry pasma, trybu, odległości i duplikatów.
   - Kliknięcie spotu: ustawienie częstotliwości radia, wypełnienie callsignu lub dodanie spotu do bandmapy — z kontrolą aktywnego radia.

7. **QSO Store**
   - SQLite z migracjami schematu.
   - Transakcja przy zapisie QSO i indeksy dla callsignu, czasu, pasma i contestu.
   - Ochrona przed utratą danych, regularny zapis i kopia bazy.
   - Historia zmian/korekt QSO; eksport nie powinien modyfikować źródłowego logu.

8. **UI / ViewModels**
   - UI nie komunikuje się bezpośrednio z Hamlibiem ani bazą.
   - Stany CAT, cluster i keyera widoczne niezależnie.
   - Obsługa skrótów i fokusowania pól bez konieczności używania myszy.

## 4. Proponowany format YAML

Poniższy przykład jest **propozycją formatu nowej aplikacji**, nie formatem istniejącym wspólnie w analizowanych loggerach:

```yaml
id: sample-contest
name: Sample Contest
period:
  start: "2026-10-24T00:00:00Z"
  end: "2026-10-25T23:59:59Z"
modes: [CW, SSB]
bands: [160m, 80m, 40m, 20m, 15m, 10m]

exchange:
  sent:
    - name: rst
      type: rst
      generated: true
    - name: serial
      type: serial
      width: 3
  received:
    - name: rst
      type: rst
    - name: serial
      type: integer
      required: true

scoring:
  qso_points: 1
  multipliers:
    - type: dxcc
      per_band: true
dupe_key: [callsign, band, mode]
exports:
  cabrillo:
    contest_code: SAMPLE
```

Schemat warto ograniczyć do typowanych reguł, takich jak „pole”, „wymagane”, „serial”, „mnożnik” i „punktacja”. Unikałbym wykonywania dowolnego kodu z pliku definicji: utrudnia to bezpieczeństwo, diagnostykę i zgodność między Windows i Linuxem. Przydatny będzie walidator/preview pokazujący, jakie pola i reguły powstaną przed rozpoczęciem zawodów.

## 5. Propozycja głównego okna

Układ powinien łączyć szybkość obsługi klawiaturą z widocznym stanem sprzętu. Panele boczne i dolne można dokować oraz ukrywać.

```text
+------------------------------------------------------------------------------------------+
| CONTEST: Sample Contest | OP: SP... | UTC: 14:32:10 | SCORE: 12 450                      |
| R1 CAT: OK - 14.025.0 MHz CW | R2 CAT: OK - 7.025.0 MHz CW | CLUSTER: OK | MODE: SO2R    |
+------------------------------------------------------------------------------------------+
| RADIO 1 / RUN                              | RADIO 2 / S&P                               |
| 14.025.0 MHz | CW | 28 WPM | RX ON         | 7.025.0 MHz | CW | 24 WPM | RX ON           |
| CALL: [W1AW______________]                 | CALL: [______________________]              |
| EXCHANGE: [599 012]   [LOG QSO]            | EXCHANGE: [_________]   [LOG QSO]           |
| SENT: 599 014 | FOCUS: R1                  | SENT: 599 015 | FOCUS: R2                   |
+------------------------------------------------------------------------------------------+
| LAST QSOS                                                                                |
| UTC       CALL       BAND   MODE   SENT       RECEIVED      PTS                          |
| 14:31:58  K1ABC      20m    CW     599 014    599 027       2                            |
| 14:30:42  DL2XYZ     20m    CW     599 013    599 118       2                            |
+------------------------------------------------------------------------------------------+
| BANDMAP                                    | DX CLUSTER                                  |
| 14.025  W1AW       14.030  K1ABC           | 14.025  W1AW  599 014  14:32Z               |
| 14.035  DL2XYZ     14.040  JA1AA           | 7.025   DL2XYZ 599 118  14:31Z              |
+------------------------------------------------------------------------------------------+
| STATUS: R1 CAT OK | R2 CAT OK | SQLITE SAVED | CW QUEUE EMPTY | LAST SPOT: 2s            |
+------------------------------------------------------------------------------------------+
```

**Założenia UX:**

- W trybie SO1R pokazywać jeden duży formularz; w SO2V — jedno radio z dwoma panelami VFO; w SO2R — dwa niezależne panele.
- Wyraźnie wyróżniać aktywny fokus, radio nadające oraz radio przypisane do keyera.
- Główne pola QSO obsługiwać bez myszy: Tab, Enter, skróty do zmiany radia/VFO i natychmiastowego przerwania CW.
- Bandmapa i cluster powinny mieć osobne filtry, ale korzystać ze wspólnego modelu spotów.
- Kolor nie może być jedynym sposobem pokazania statusu — dodać tekst i ikonę dla CAT/cluster/keyera.
- Dodać kompaktowy układ na mniejsze ekrany oraz zapamiętywanie układu paneli.

## 6. Szczegółowy plan wdrożenia

### Etap 1 — fundamenty i model danych
- Utworzyć projekt CMake dla C++20/Qt 6.
- Zdefiniować QSO, Contest, Station, Radio, VFO, Spot i stan połączeń.
- Zaprojektować schemat SQLite oraz migracje.
- Utworzyć minimalne okno główne, konfigurację operatora i wybór contestu.
- Dodać testy jednostkowe dla zapisu, odczytu i transakcji QSO.

### Etap 2 — definicje contestów
- Zdefiniować schemat YAML i walidację pól.
- Zaimplementować loader, komunikaty błędów i podgląd wymagań contestu.
- Dodać przykładowe definicje dla różnych typów exchange i mnożników.
- Testować duplikaty, niepoprawne wymiany, numerację serial i punktację.

### Etap 3 — podstawowy SO1R
- Zaimplementować formularz callsign/exchange, dupe checking i logowanie klawiszem Enter.
- Dodać listę ostatnich QSO oraz korektę kontaktu.
- Dodać obsługę CAT przez Hamlib: model radia, port/host, baud rate i test połączenia.
- Oddzielić wątek komunikacji radiowej od UI.
- Dodać mock radia do testów bez sprzętu.

### Etap 4 — DX cluster i bandmap
- Zaimplementować klienta TCP/Telnet, parser spotów i status połączenia.
- Zaprojektować bandmapę z filtrowaniem pasma i duplikatów.
- Powiązać wybór spotu z aktywnym radiem.
- Testować parsowanie, reconnect i niepoprawne odpowiedzi serwera.

### Etap 5 — CW
- Zaimplementować `ICwKeyer` oraz kolejkę wiadomości contestowych.
- Dodać Hamlib provider i test wykrywania nieobsługiwanych komend CW.
- Dodać opcjonalny provider zewnętrznego keyera (np. Winkeyer/cwdaemon).
- Zapewnić natychmiastowe przerwanie nadawania i zmianę WPM.
- Testować zachowanie z mock keyerem i na sprzęcie referencyjnym Linux/Windows.

### Etap 6 — SO2V
- Dodać drugi VFO/odbiornik w modelu oraz dwa wejściowe konteksty pracy.
- Obsłużyć przełączanie RX focus i odzwierciedlać je w UI.
- Weryfikować możliwości modelu radia przed aktywacją trybu.
- Testować najpierw na symulatorze, później na radiu z dwoma VFO.

### Etap 7 — SO2R
- Dodać drugie radio, osobne CAT, bandmapę, fokus i powiązanie keyera.
- Zapewnić, że zmiana fokusu nie kieruje przypadkowo CW lub zmian częstotliwości do niewłaściwego radia.
- Dodać konfigurację audio/przełącznika jako etap rozszerzony — zależny od kontrolera.
- Testować utratę połączenia jednego radia przy dalszej pracy drugiego.

### Etap 8 — eksport i stabilizacja
- Eksport Cabrillo/ADIF i sprawdzanie poprawności pliku przed zapisaniem.
- Przywracanie po awarii oraz backup SQLite.
- Testy integracyjne na Linuxie i Windowsie, w tym portów szeregowych, `rigctld`, sieci i stanu uśpienia.
- Pakowanie aplikacji dla obu systemów oraz dokumentacja konfiguracji Hamlib i contestów.

### Etap 9 — dodatki
- Przechodzenie po spotach dxcluster i ustawianie qrg na danym spocie
- Obsługa plików typu call history, master.cpy, wl_cty.dat
- Import definicji zawodow: cqww ssb, cqww cw, cqwpx cw, cqwpx ssb, iaruhfc, spdx, wag, arrl dx ssb, arrl dx cw, arrl dx 10m, eudx, euhfc
- Przechodzenie pomiedzy polami callsign oraz wymiana (exchange) nastepowalo przez klawisz spacji


## 7. Rekomendowana kolejność i ryzyka

Najmniejsze ryzyko daje kolejność: **SO1R → contest YAML → CAT → cluster/bandmap → CW → SO2V → SO2R**. SO2R jest największym krokiem, ponieważ obejmuje nie tylko dwa połączenia CAT, ale też routing wejścia, nadawania, fokusu, a często audio i sprzęt zewnętrzny.

Warto od początku zaprojektować **abstrakcję keyera i warstwę radiową**, ale nie uzależniać pierwszego wydania od SO2R ani od wysyłania CW przez Hamlib w każdym modelu radia. Dostępność funkcji trzeba sprawdzać w runtime i zgłaszać operatorowi, zamiast udawać, że każda kombinacja radia/backendu obsługuje tę samą funkcjonalność.

## Źródła

- [DXLog.net — strona projektu](https://www.dxlog.net/) i [Getting started](https://www.dxlog.net/docs/index.php/Getting_started), [konfiguracja zawodów](https://www.dxlog.net/docs/index.php?title=Configuration), [obsługiwane radia](https://www.dxlog.net/docs/index.php?title=Radios)
- [N1MM Logger+ — dokumentacja](https://n1mmwp.hamdocs.com/), [SO2V/SO2R](https://n1mmwp.hamdocs.com/manual-operating/single-operator-contesting/), [okna programu](https://n1mmwp.hamdocs.com/n1mm-manual/windows/), [bandmap](https://n1mmwp.hamdocs.com/manual-windows/bandmap/)
- [Not1MM — repozytorium i funkcje](https://github.com/mbridak/not1mm), [opis rozwoju Multi-Multi](https://github.com/mbridak/not1mm/blob/master/Multi-Multi.md)
- [TLF — projekt i lista funkcji](https://tlf.github.io/), [aktualny podręcznik](https://github.com/Tlf/tlf/blob/master/doc/Manual.md)
- [Hamlib](https://hamlib.github.io/)
- [Qt — obsługiwane platformy](https://doc.qt.io/qt-6/supported-platforms.html), [Qt SQL](https://doc.qt.io/qt-6/qtsql-index.html), [Qt Serial Port](https://doc.qt.io/qt-6/qtserialport-index.html)
