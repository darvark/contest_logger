# Audyt regulaminów contestów

Stan na 2026-10-21. Audyt porównuje definicje YAML z dostępnymi regulaminami
i testami aplikacji. Oznaczenie „częściowo” nie oznacza certyfikacji ani
gwarancji przyjęcia logu przez organizatora.

## Zmiany wprowadzone w audycie

- IARU HF rozróżnia teraz punkty za tę samą i inną strefę ITU, kontakty
  międzykontynentalne oraz stacje HQ/official. Wymaga porównania strefy
  odebranej z własną oraz rozpoznania obecności wymiany HQ.
- EUDX ma punkty 2/10/3/5 dla odpowiednio własnego kraju, innego kraju UE,
  kraju na tym samym kontynencie i innego kontynentu. Dodano mnożniki regionu
  i DXCC per band.
- WAG dla stacji spoza Niemiec przyznaje punkt za QSO z Niemcami; mnożnik
  wykorzystuje pierwszą literę DOK, pomija cyfry poprzedzające literę i nie
  zalicza `NM`.
- ARRL DX ma dodatkowe profile CW/SSB dla entrantów W/VE: wysyłany stan/prowincja,
  odbierana moc stacji DX oraz DXCC multiplier per band z wykluczeniem USA
  i Kanady. Istniejące profile ARRL DX nadal obsługują entrantów DX i
  mnożniki stan/prowincja per band.
- ARRL 10m rozróżnia wymianę i mnożnik według klasy korespondenta (stany/
  prowincje Meksyku i W/VE, serial DX, region ITU dla /MM), z punktacją CW/SSB
  i mnożnikami per emisja. Lista kodów stanów Meksyku pochodzi z ARRL; klasy
  stacji wyznacza CTY i końcówka `/MM`.
- EUDX ogranicza wpisy regionu do aktualnie publikowanych kodów `FR20` i `NL13`.
- IARU HF wymaga teraz dokładnie jednego odebranego wariantu: strefy ITU albo
  pola HQ/official. Nie waliduje jeszcze autorytatywnej listy znaków HQ ani
  ograniczeń kategorii operatora.
- Profile WAE naliczają punkty QTC oraz ważone mnożniki DXCC: 80 m ×4, 40 m ×3,
  pozostałe pasma ×2. Nie rozstrzygają jeszcze kwalifikowalności QSO zależnie od
  regionu entrantów ani autorytatywnej listy krajów/call-area WAE.
- Wynik domyślny profili IARU HF i EUDX zmieniono na zero, gdy nie da się
  rozstrzygnąć geografii lub wymaganej wymiany. Zapobiega to wyświetlaniu
  niepopartej regulaminem punktacji.
- Dodano testy regresyjne punktacji tych przypadków oraz dokładnego wiersza QSO
  Cabrillo dla wszystkich 23 plików YAML. Test sprawdza też skonfigurowany
  klucz duplikatu; nie zastępuje niezależnej certyfikacji całej punktacji.

## Profile zawodów

| Profile | Ocena i pozostałe ograniczenia |
|---|---|
| ARRL 10 Meter | Profil 2026 rozróżnia wymianę i mnożnik według klasy korespondenta; 4 punkty CW, 2 Phone, mnożniki per emisja. Kody Meksyku ograniczone są do listy ARRL. Nadal zależy od aktualnych danych CTY, w szczególności dla klasy stacji oraz /MM. |
| ARRL DX CW, SSB | 3 punkty/QSO. Profile `arrl-dx-cw.yaml` i `arrl-dx-ssb.yaml` obsługują entrantów DX: wysyłana moc, odbierany stan/prowincja, mnożnik wymiany per band. Profile `arrl-dx-cw-w-ve.yaml` i `arrl-dx-ssb-w-ve.yaml` obsługują entrantów W/VE: wysyłany stan/prowincja (format 2–3 wielkie litery), odbierana moc 1–1500 W, DXCC multiplier per band z wykluczeniem USA i Kanady. Test obejmuje wymianę, punktację, nowy mnożnik, deduplikację per band, wykluczone encje i odrzucenie niepoprawnej mocy. Kategoria i uprawnienia entrantów nie są walidowane; poprawny DXCC wymaga aktualnego CTY. |
| ARRL Sweepstakes CW, Phone | Profile 2026 deklarują serial/precedence/check/section, 2 punkty, 85 sekcji i duplikat callsign w całym logu. Test obejmuje sekcję, mnożnik i eksport bez RST. Nie są wymuszane kwalifikacja geograficzna, limit czasu ani zgodność precedence z kategorią. |
| CQ WW CW, SSB | Profile 2026 zawierają punktację 3/1/2/0 i mnożniki strefa oraz kraj per band. Rozdzielenie emisji sprawia, że duplikat per band/mode nie łączy CW z SSB. Pozostaje zależność od CTY/DXCC oraz niewymuszana reguła stacji maritime mobile (tylko mnożnik strefy). |
| CQ WPX CW, SSB | Profile 2026 zawierają serial, punktację zależną od kontynentu/kraju/pasma i mnożnik WPX prefix w całych zawodach; duplikat jest per band/mode. Niezależne testy nie obejmują każdej gałęzi punktacji ani wszystkich wyjątków geograficznych. |
| EUDX | Punktacja i oba typy mnożnika zostały uzupełnione; region ograniczono do `FR20` i `NL13`. Kraj UE i DXCC nadal zależą od danych CTY oraz ręcznego wyboru odpowiedniego pola wymiany. |
| EUHFC | Profil 2026 zawiera rok pierwszej licencji, 1 punkt za kontakt europejski, zero poza Europą oraz mnożnik roku per band niezależnie od emisji. Wymaga poprawnych danych kontynentu; nie wymusza zgodności roku operatora z niezależnym źródłem. |
| IARU HF | Punktacja 1/3/5, mnożniki stref/HQ per band oraz dokładnie jeden odebrany wariant wymiany są zaimplementowane. Profil nie potwierdza jeszcze, że wartość HQ/official odpowiada autoryzowanej stacji i nie modeluje ograniczeń kategorii. |
| SP DX — stacja SP | Profil 2026 deklaruje wymianę RST/prowincja oraz RST/serial, 0 punktów w kraju, 1 w Europie i 3 poza Europą oraz DXCC per band. Zależny od klasyfikacji CTY/DXCC. |
| SP DX — stacja EU/DX | Profil 2026 deklaruje wymianę RST/serial oraz RST/prowincja, 3 punkty za QSO ze stacją SP i mnożnik województwa per band. |
| WAG | Profil DX odzwierciedla wymianę serial/DOK lub NM, 1 punkt za QSO z Niemcami, district-letter multiplier per band/mode oraz wykluczenie NM. **Brak osobnego profilu entrantów niemieckich**, którzy mają inną wymianę i DXCC/WAE multipliers. |
| WAE CW/SSB/RTTY | Wymiana serialowa, trwałe QTC, eksport `QTC:`, po 1 punkcie za wiadomość QTC oraz DXCC mnożniki z wagami pasm (80 m ×4, 40 m ×3, pozostałe ×2). Profile nie modelują jeszcze kwalifikowalności QSO według regionu entrantów ani autorytatywnej listy WAE/call-area; wynik nie jest certyfikowany i `CLAIMED-SCORE` może być niepełny. |
| `general-logging`, `grid-field-day`, `serial-sprint` | Przykłady i profile testowe silnika, a nie definicje rzeczywistych regulaminów contestów. |

## Cabrillo i duplikaty

Testy sprawdzają kolejność oraz zawartość wiersza QSO dla wszystkich 23 YAML-i,
w tym profilów bez RST i profilów z wieloelementową wymianą. Weryfikacja
nie obejmuje uploadu do serwisu organizatora, niezależnego parsera
Cabrillo ani kompletności wszystkich nagłówków zależnych od kategorii.
Klucze duplikatu profili zostały asertywnie sprawdzone: Sweepstakes po
callsign, pozostałe po callsign/band/mode. Pełne testy scoringowe reguł
duplikatów dla każdego regulaminu pozostają do dodania.

## Źródła

- [CQ WW — rules](https://cqww.com/rules/)
- [CQ WPX — rules](https://cqwpx.com/rules/)
- [ARRL Sweepstakes — rules PDF](https://contests.arrl.org/ContestRules/SS-Rules.pdf)
- [ARRL DX — rules PDF](https://contests.arrl.org/ContestRules/DX-Rules.pdf)
- [ARRL 10 Meter — 2026 rules PDF](https://www.arrl.org/files/file/Contest%20Rules%20PDFs/2026/10%20Meter%20Rules%20v%202_0%20-%202026.pdf)
- [ARRL contest multipliers — 10 Meter](https://contests.arrl.org/contestmultipliers.php?a=xe)
- [IARU HF — rules PDF](https://contests.arrl.org/ContestRules/IARU-HF-Rules.pdf)
- [SP DX — 2026 rules](https://spdxcontest.pzk.org.pl/2026/rules.php)
- [EUDX — rules](https://www.eudx-contest.com/rules/)
- [DARC WAE — rules](https://www.darc.de/der-club/referate/conteste/wae-dx-contest/en/wae-rules/)
- [EUHFC — current rules PDF](https://euhf.s5cc.eu/rules/euhfc_rules_latest.pdf)
- [DARC WAG — rules](https://www.darc.de/der-club/referate/conteste/wag-contest/en/rules/)
