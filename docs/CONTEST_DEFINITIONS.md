# Definicje zawodów (YAML)

Definicja zawodów to plik YAML opisujący pasma, emisje, wymianę i punktację. Gotowe przykłady: [contests/examples/](../contests/examples/).

Import własnej definicji: menu plików programu, opcja importu definicji zawodów (pliki `*.yaml`, `*.yml`). Plik jest walidowany; błąd zawiera ścieżkę do niepoprawnego ustawienia (np. `contest.exchange.sent[1].type`).

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

Emisje w Cabrillo: SSB/USB/LSB są eksportowane jako `PH`, RTTY jako `RY`, DATA/FT8/FT4/PSK jako `DG`; CW, FM, AM bez zmian. Inne emisje blokują eksport Cabrillo.

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

## Punktacja (`scoring`)

| Klucz | Domyślnie | Opis |
|---|---|---|
| `qso_points` | `1` | Punkty za QSO (>= 0). |
| `serial_start` | `1` | Pierwszy numer generowanego numeru seryjnego (>= 1). |
| `multipliers` | brak | Lista mnożników. |

Wynik końcowy = suma punktów x liczba mnożników (bez mnożników: suma punktów).

### Mnożniki

| Opcja | Opis |
|---|---|
| `type` | `exchange` lub `prefix`. |
| `field` | Tylko dla `exchange`: nazwa pola z `received`, którego wartość jest mnożnikiem (np. strefa, DOK). Wymagane. |
| `per_band` | `true` – mnożnik liczony osobno na każdym paśmie; domyślnie `false`. |

Typ `prefix` używa prefiksu znaku (reguły WPX, np. `SP9`). Nie można zdefiniować dwóch identycznych mnożników.

## Dublety (`dupe_key`)

Domyślnie QSO jest dublem, gdy ten sam znak był już wpisany do dziennika. Dodanie `band` i/lub `mode` powoduje, że dublet liczy się tylko w tym samym paśmie i/lub tej samej emisji.

```yaml
dupe_key: [callsign, band]        # dubel = ten sam znak na tym samym paśmie
```

## Przykłady

Wymiana ze strefą i mnożnikiem strefowym oraz prefiksowym (CQ WW):

```yaml
exchange:
  sent:
    - {name: report, label: RST, type: rst, default: "599"}
    - {name: zone, label: CQ Zone (station), type: integer, min: 1, max: 40}
  received:
    - {name: report, label: RST, type: rst}
    - {name: zone, label: CQ Zone, type: integer, min: 1, max: 40}
scoring:
  qso_points: 1
  multipliers:
    - {type: exchange, field: zone, per_band: true}
    - {type: prefix, per_band: true}
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
