# Definicje zawodów (YAML)

Definicja zawodów to plik YAML opisujący pasma, emisje, wymianę i punktację. Gotowe przykłady: [contests/examples/](../contests/examples/).

Import własnej definicji: menu plików programu, opcja importu definicji zawodów (pliki `*.yaml`, `*.yml`). Plik jest walidowany; błąd zawiera ścieżkę do niepoprawnego ustawienia (np. `contest.exchange.sent[1].type`).

Przy pierwszym uruchomieniu program tworzy podkatalog `contests` w katalogu
danych aplikacji i zapisuje w nim brakujące wbudowane definicje. Domyślna ścieżka
to `~/.local/share/ContestLogger/ContestLogger/contests/` na Linuksie lub
`%APPDATA%\ContestLogger\ContestLogger\contests\` na Windows. Można umieszczać
tam własne pliki `.yaml` i `.yml`; są odczytywane przy starcie i otwieraniu
okna nowego logu. Istniejące pliki nie są nadpisywane podczas startu.
Import z menu **Contest** lub okna **New log** również zapisuje YAML w tym
katalogu. Każda definicja w katalogu musi mieć unikalne `id`.

Nowy log tworzy się oknem **File > New log...** (Ctrl+N): wybór zawodów z listy zainstalowanych definicji (lub przycisk **Import YAML...**), znak stacji, operatorzy, klub, data startu, nazwa logu, kategorie Cabrillo i wymiana nadawana. Pasma i emisje w kategoriach pochodzą z definicji (`bands`, `modes`).

Zasady ogólne:

- Dozwolone są wyłącznie opisane niżej klucze. Nieznany lub zduplikowany klucz powoduje błąd.
- Maksymalny rozmiar pliku: 1 MiB.
- Zmiana pliku nie wpływa na istniejący dziennik, który przechowuje własną kopię definicji. Aby użyć zmian, załóż nowy dziennik lub zainstaluj definicję ponownie.

## Szkielet

```yaml
schema_version: 1
id: moje-zawody
name: Moje zawody
description: Opcjonalny opis.
bands: [160m, 80m, 40m, 20m, 15m, 10m]
modes: [CW, SSB]
exchange:
  sent:
    - {name: report, label: RST, type: rst, default: "599"}
    - {name: serial, label: Serial, type: serial, generated: true, width: 3}
  received:
    - {name: report, label: RST, type: rst}
    - {name: serial, label: Serial, type: integer, min: 1, max: 999999}
scoring:
  qso_points: 1
  serial_start: 1
  multipliers:
    - {type: prefix, per_band: true}
dupe_key: [callsign, band, mode]
```

## Pola główne

| Klucz | Wymagany | Opis |
|---|---|---|
| `schema_version` | nie | Wersja schematu; obecnie tylko `1`. |
| `id` | tak | Identyfikator: małe litery, cyfry, `_` lub `-`, zaczyna się od litery lub cyfry. |
| `name` | tak | Nazwa wyświetlana. |
| `description` | nie | Opis. |
| `bands` | tak | Lista dozwolonych pasm, np. `[40m, 20m]` (zapisywane małymi literami). Bez duplikatów. |
| `modes` | tak | Lista emisji, np. `[CW, SSB, RTTY]` (zapisywane wielkimi literami). |
| `exchange` | tak | Wymiana nadawana i odbierana (patrz niżej). |
| `scoring` | tak | Punktacja i mnożniki. |
| `dupe_key` | nie | Klucz dubli. Dozwolone: `callsign`, `band`, `mode`. Musi zawierać `callsign`. |
| `cabrillo` | nie | Mapa z kluczem `contest`: oficjalny identyfikator zawodów w nagłówku Cabrillo (np. `cabrillo: {contest: CQ-WPX-CW}`). Litery, cyfry, `_`, `-`; zapisywany wielkimi literami. Bez tego klucza eksport używa `id` wielkimi literami. |

Emisje w Cabrillo: SSB/USB/LSB są eksportowane jako `PH`, RTTY jako `RY`, DATA/FT8/FT4/PSK jako `DG`; CW, FM, AM bez zmian. Inne emisje blokują eksport Cabrillo.

Nagłówek Cabrillo 3.0 jest uzupełniany automatycznie: `CONTEST` z `cabrillo.contest`, `CALLSIGN`, `LOCATION`, `CLUB`, `OPERATORS`, `SOAPBOX` i `CATEGORY-*` z właściwości logu (okno New log / Log properties), a `NAME`, `EMAIL`, `ADDRESS*` i `GRID-LOCATOR` z ustawień stacji (Operator and station). `CLAIMED-SCORE` jest liczony z punktacji definicji, a `CREATED-BY` zawiera wersję programu. Brakujące kategorie są wyznaczane z QSO: pasmo (jedno pasmo albo `ALL`), emisja (jedna emisja albo `MIXED`) oraz moc z ustawień stacji (`HIGH`/`LOW`/`QRP`). Dla `SINGLE-OP` domyślnie są to `NON-ASSISTED`, `ONE` i `OPERATORS` równe znakowi stacji. QSO są eksportowane chronologicznie, a QSO bez częstotliwości z radia dostają dolną krawędź pasma (np. `1800` dla 160m).

## Wymiana (`exchange`)

Sekcja zawiera dwie niepuste listy pól (maks. 32 każda):

- `sent` – wymiana nadawana przez Twoją stację. W oknie wprowadzania jest to edytowalne pole "Snt ...", zapamiętywane między QSO.
- `received` – wymiana odbierana od korespondenta.

### Opcje pola

| Opcja | Dotyczy | Opis |
|---|---|---|
| `name` | wszystkie | Wymagane. Małe litery, cyfry, `_`, zaczyna się od litery. Unikalne w liście. |
| `type` | wszystkie | Wymagane. Typ pola (patrz niżej). |
| `label` | wszystkie | Etykieta w interfejsie. Domyślnie równa `name`. |
| `required` | wszystkie | `true` (domyślnie) lub `false`. Pole opcjonalne puste nie trafia do logu. |
| `required_when` | tylko `received` | Warunek, przy którym pole jest wymagane; gdy nie pasuje, pole jest opcjonalne. |
| `when` | tylko `sent` | Warunek lokalizacji stacji, przy którym pole jest wysyłane; użyj go do alternatywnych wymian stacji. |
| `default` | tylko `sent`, nie generowane | Wartość domyślna; musi spełniać regułę pola. |
| `generated` | tylko `sent` typu `serial` | Numer jest nadawany automatycznie. |
| `width` | wszystkie | Maksymalna liczba znaków (0-32). Dla `serial` uzupełnia zerami z przodu (`001`). |
| `min`, `max` | `integer`, `serial` | Zakres liczbowy (nieujemny, `min` <= `max`). `0` oznacza brak ograniczenia. |
| `pattern` | wszystkie | Wyrażenie regularne (PCRE) dopasowywane do całej wartości, wielkimi literami. Zastępuje wbudowaną regułę typu. |
| `values` | tylko `enum` | Lista dozwolonych wartości. Wymagana dla `enum`, niedozwolona dla innych typów. |

### Typy pól (`type`)

| Typ | Walidacja |
|---|---|
| `rst` | `RS`/`RST`: `[1-5][1-9]` lub `[1-5][1-9][1-9]`, np. `59`, `599`. |
| `serial` | Numer porządkowy. W `sent` musi mieć `generated: true`. W `received` działa jak liczba całkowita. |
| `integer` | Liczba całkowita >= 0, z opcjonalnym `min`/`max`. |
| `text` | Dowolny tekst (ograniczony `width`/`pattern`). |
| `grid` | Lokator: `AA00` lub `AA00AA` (litery A-R / A-X). |
| `callsign` | Znak: `[A-Z0-9/]{3,}`. |
| `enum` | Jedna z wartości z `values`. |

### Raport RST

- Pole typu `rst` w `received` nie jest wyświetlane przy wprowadzaniu QSO. Odebrany raport jest ustawiany taki sam jak nadany (pole `rst` z `sent`); bez nadawanego `rst` program wstawia 59 (emisje foniczne) lub 599.
- W Cabrillo raport RST trafia do osobnej kolumny, a reszta wymiany jest wypisywana wartościami bez nazw pól.
- Pole nadawanego `rst` ma automatycznie ustawiane 59/599 zgodnie z emisją, jeśli zawiera jedną z tych wartości.

### Wskazówki

- Jeśli wymiana nadawana jest obowiązkowa w danych zawodach (np. DOK, strefa, moc), nie ustawiaj `required: false`, bo puste pole nie trafi do tabeli QSO ani do Cabrillo.
- Kolejność pól w `sent` i `received` to kolejność w tabeli QSO i w Cabrillo.
- `required_when` określa warunek wymagalności, nie ukrywa pola w formularzu.
  Dopuszczalne klucze warunku opisano w sekcji punktacji.

## Punktacja (`scoring`)

| Klucz | Domyślnie | Opis |
|---|---|---|
| `qso_points` | `1` | Punkty za QSO (>= 0). |
| `qso_point_rules` | brak | Reguły warunkowe punktów; pierwsza pasująca reguła wygrywa. Bez dopasowania używane jest `qso_points`. |
| `serial_start` | `1` | Pierwszy numer generowanego numeru seryjnego (>= 1). |
| `multipliers` | brak | Lista mnożników. |

Wynik końcowy = suma punktów x liczba mnożników (bez mnożników: suma punktów).

### Warunki wymiany i punktacji

Warunki zapisuje się jako mapę `klucz: wartość`; wszystkie podane warunki muszą
pasować (AND). Klucze są porównywane bez względu na wielkość liter, `band`
normalizowany do małych liter, a wartości wymiany i metadane CTY do wielkich
liter.

| Klucz | Znaczenie |
|---|---|
| `band`, `mode` | Pasmo lub emisja QSO. |
| `continent` | Kontynent rozpoznany przez `wl_cty.dat`. |
| `dxcc`, `country` | Nazwa encji DXCC z `wl_cty.dat` (`country` jest aliasem `dxcc`, nie niezależnym katalogiem państw). |
| `cq_zone`, `itu_zone` | Strefa rozpoznana przez `wl_cty.dat`. |
| `station.dxcc`, `station.country` | Encja DXCC/kraj własnej stacji z jej znaku w `wl_cty.dat`. |
| `station.continent`, `station.cq_zone`, `station.itu_zone` | Kontynent i strefy własnej stacji. |
| `same_dxcc`, `same_country`, `same_continent` | `true`/`false`, porównanie encji, kraju (alias DXCC) lub kontynentu obu stacji. |
| `exchange.<name>`, `received.<name>` | Wartość odebranej wymiany z pola `<name>`. |

`required_when` dla pola odebranej wymiany używa tego samego formatu. Gdy
warunek pasuje, pole jest wymagane; gdy nie pasuje, pole jest opcjonalne.
Nieznane klucze i odwołania do nieistniejących pól są odrzucane przy imporcie
definicji.

```yaml
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
  received:
    - {name: report, type: rst}
    - name: state
      label: State
      type: text
      required: false
      required_when:
        continent: NA
scoring:
  qso_points: 1
  qso_point_rules:
    - points: 5
      when: {continent: NA, band: 20m}
    - points: 3
      when: {country: United States}
```

W przykładzie pole `state` jest wymagane dla korespondentów z Ameryki
Północnej. QSO z NA na 20 m dostaje 5 punktów; w przeciwnym razie dopasowanie
encją `United States` daje 3; QSO bez pasującej reguły dostaje wartość bazową.
Reguły są oceniane od góry, dlatego bardziej szczegółowe warunki umieszczaj
przed ogólnymi. Warunki geograficzne wymagają załadowanego `wl_cty.dat`;
gdy metadane korespondenta są niedostępne, reguła nie dopasowuje się. Warunki
`station.*` i `same_*` porównują znak zapisany w QSO ze znakiem korespondenta.
`country` jest obecnie nazwą encji CTY/DXCC, nie niezależnym katalogiem
suwerennych państw.

### Mnożniki

| Opcja | Opis |
|---|---|
| `type` | `exchange`, `prefix`, `dxcc` lub `country`. `country` i `dxcc` korzystają z encji `wl_cty.dat`. |
| `field` | Tylko dla `exchange`: nazwa pola z `received`, którego wartość jest mnożnikiem (np. strefa, DOK). Wymagane. |
| `per_band` | `true` – mnożnik liczony osobno na każdym paśmie; domyślnie `false`. |
| `per_mode` | `true` – mnożnik liczony osobno dla każdej emisji; domyślnie `false`. Można łączyć z `per_band`. |

Typ `prefix` używa prefiksu znaku (reguły WPX, np. `SP9`). Nie można zdefiniować dwóch identycznych mnożników.
Typy `dxcc` i `country` używają nazwy encji rozpoznanej z `wl_cty.dat`;
wymagają danych CTY dostarczonych przez resolver aplikacji. Klucz może więc
być liczony osobno dla pasma, emisji lub obu wymiarów.

Pole nadawanej wymiany może mieć `when` z warunkiem `station.*`. Pole jest
pomijane dla innej lokalizacji, a gdy CTY nie pozwala rozpoznać własnej stacji,
QSO zostaje odrzucone z błędem zamiast zapisać niekompletną wymianę. Warunki
alternatywnych pól powinny być wzajemnie rozłączne, np.:

```yaml
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
    - name: state
      type: text
      when: {station.country: "United States"}
    - name: province
      type: text
      when: {station.country: Canada}
```

## Dublety (`dupe_key`)

Domyślnie QSO jest dublem, gdy ten sam znak był już wpisany do dziennika. Dodanie `band` i/lub `mode` powoduje, że dublet liczy się tylko w tym samym paśmie i/lub tej samej emisji.

```yaml
dupe_key: [callsign, band]        # dubel = ten sam znak na tym samym paśmie
```

## Przykłady

Przykładowy profil nie zastępuje aktualnego regulaminu. Przed zawodami sprawdź
edycję, kategorię, wymianę i identyfikator Cabrillo; przykłady opisują swoje
ograniczenia w `description`. W szczególności ocena punktów geograficznych
wymaga poprawnego `wl_cty.dat`, a niektóre mnożniki zależne od kraju lub
kategorii entrantów mogą pozostać ręczne/prowizoryczne. Definicja określa
dozwolone pasma i emisje, ale nie waliduje dozwolonych kombinacji kategorii
Cabrillo dla danego regulaminu; ustaw je zgodnie z bieżącą edycją zawodów.

Wymiana ze strefą i mnożnikiem strefowym oraz DXCC (CQ WW):

```yaml
exchange:
  sent:
    - {name: report, label: RST, type: rst, default: "599"}
    - {name: zone, label: CQ Zone (station), type: integer, min: 1, max: 40}
  received:
    - {name: report, label: RST, type: rst}
    - {name: zone, label: CQ Zone, type: integer, min: 1, max: 40}
scoring:
  qso_points: 0
  qso_point_rules:
    - {points: 0, when: {same_country: true}}
    - {points: 2, when: {same_continent: true, continent: NA}}
    - {points: 1, when: {same_continent: true}}
    - {points: 3, when: {same_continent: false}}
  multipliers:
    - {type: exchange, field: zone, per_band: true}
    - {type: dxcc, per_band: true}
```

Pole z listą wartości i lokator:

```yaml
exchange:
  sent:
    - {name: report, label: RST, type: rst, default: "599"}
    - {name: grid, label: Grid (station), type: grid, width: 6}
  received:
    - {name: report, label: RST, type: rst}
    - {name: cls, label: Class, type: enum, values: [A, B, C]}
    - {name: grid, label: Grid, type: grid}
```

Własny wzorzec (DOK: litera i dwie cyfry lub numer):

```yaml
- {name: exchange, label: DOK or serial, type: text, pattern: "[A-Z][0-9]{2}|[0-9]{1,4}"}
```

## Najczęstsze błędy

| Komunikat | Przyczyna |
|---|---|
| `... is not a supported setting` | Literówka lub nieobsługiwany klucz. |
| `serial fields in sent exchange must be generated` | W `sent` pole `serial` bez `generated: true`. |
| `generated is only valid for sent serial fields` | `generated` użyte poza `sent` + `serial`. |
| `default is only valid for non-generated sent fields` | `default` w `received` lub w polu generowanym. |
| `values is required for enum fields` | `enum` bez listy `values`. |
| `references unknown received field` | Mnożnik `exchange` wskazuje nieistniejące pole `received`. |
| `dupe_key must include callsign` | Brak `callsign` w kluczu dubli. |
