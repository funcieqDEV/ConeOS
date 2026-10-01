# ConeOS Roadmap

Poniższe kroki są ułożone w kolejności zależności. Każdy etap powinien być
ukończony i przetestowany przed rozpoczęciem następnego.

Statusy: `[x]` ukończone, `[~]` częściowo wdrożone, `[ ]` oczekujące.

## Aktualny stan

- **Ukończony etap:** 9 — izolacja pamięci i odporność procesów.
- **Aktywny etap:** 10 — wielordzeniowość, synchronizacja i stabilizacja.
- Kernel uruchamia procesy ELF64 w Ring 3 z `argc/argv`.
- Działają `spawn`, `exec`, `waitpid`, PID/PPID, zombie i kody zakończenia.
- Ramfs, VFS, deskryptory per proces i standardowe strumienie są dostępne.
- Syscalle `pipe`, `dup` i `dup2` oraz potok `program | program` w shellu działają.
- `/bin/shell` działa w Ring 3 i jest automatycznie uruchamiany jako PID 1.
- QEMU uruchamia ConeOS z trwałym `disk.img` przez VirtIO Block.
- FAT32 jest zamontowany jako `/disk`.

## 1. Stabilne ABI syscalli i bezpieczny dostęp do pamięci użytkownika — ukończone

- [x] Zdefiniować konwencję wywołań, numery syscalli i kody błędów.
- [x] Dodać `copy_from_user()` i `copy_to_user()` z kontrolą zakresu, mapowania oraz
  praw dostępu stron.
- [x] Dodać syscalle `read`, `sleep` i `gettime`.
- [x] Zwracać spójne, ujemne kody błędów.
- [x] Dodać bibliotekę userspace z wrapperami syscalli.

**Gotowe, gdy:** program Ring 3 potrafi czytać klawiaturę, spać bez aktywnego
oczekiwania i nie może zawiesić kernela przez przekazanie błędnego wskaźnika.

## 2. Pełniejszy model procesów — ukończone funkcjonalnie

- [~] Oddzielić proces od wątku schedulera — istnieje osobny rekord procesu,
  lecz jeden proces nadal odpowiada jednemu taskowi.
- [x] Wprowadzić PID, PPID, stan procesu, kod wyjścia i relację rodzic–dziecko.
- [x] Zaimplementować `spawn`, `exec`, `waitpid` i `exit`.
- [x] Obsłużyć procesy zombie i zwalnianie przestrzeni adresowej.
- [x] Zezwolić na wiele procesów użytkownika.

**Gotowe, gdy:** proces może uruchomić drugi program, poczekać na niego i odebrać
jego kod wyjścia bez wycieku stron ani pamięci kernela.

## 3. VFS i ramfs — częściowo ukończone

- [~] Zdefiniować interfejs VFS — pliki działają, katalogi wymagają rozbudowy.
- [x] Zaimplementować ramfs jako pierwszy backend.
- [x] Dodać deskryptory plików per proces.
- [x] Dodać `open`, `close`, `read`, `write`, `seek` i `stat`.
- [x] Dodać dynamiczne katalogi oraz `mkdir`, `unlink`, `rmdir` i wspólny `readdir`
  dostępny przez VFS i syscall. Userspace ma już `cp` dla ramfs.
- [~] Dodać `cwd`, `pwd`, `cd` i rozwiązywanie względnych ścieżek plików;
  `cd`, `cd .`, `cd ..` oraz normalizacja `.`/`..` dla VFS działają, a `ls
  <ścieżka>` listuje wybrany katalog.

**Gotowe, gdy:** program Ring 3 może utworzyć plik, zapisać dane, odczytać je
przez deskryptor i wylistować katalog.

## 4. Standardowe strumienie i urządzenia znakowe — częściowo ukończone

- [x] Dodać `/dev/console`, `/dev/null` i `/dev/zero` jako węzły devfs.
- [x] Podłączyć stdin, stdout i stderr jako deskryptory `0`, `1` i `2`.
- [x] Przenieść obsługę shella Ring 3 na standardowe strumienie.
- [x] Dodać blokujące odczyty budzone przez IRQ klawiatury.
- [x] Obsługiwać standardowe strumienie przez VFS i backend konsoli.
- [x] Dziedziczyć standardowe strumienie przez `spawn` i `exec`.
- [x] Dodać blokujące potoki między procesami, EOF po zamknięciu piszących końców
  oraz syscalle `pipe`, `dup` i `dup2`.
- [x] Obsłużyć pojedynczy operator `|` w userspace shellu; potok łączy dwa
  zewnętrzne programy, np. `echo test | cat`.
- [ ] Dodać Shift, Caps Lock, klawisze rozszerzone i lepszą edycję linii.

**Gotowe, gdy:** ten sam kod `read`/`write` działa dla zwykłego pliku i konsoli,
a oczekujący proces nie zużywa czasu procesora.

## 5. Ładowanie programów ELF z filesystemu — prawie ukończone

- [x] Odłączyć loader ELF od konkretnego programu testowego.
- [~] Ładować ELF z ramfs; przełączyć loader z bezpośredniego ramfs na VFS.
- [x] Przygotować stos procesu z `argc`, wieloelementowym `argv` i końcowym `NULL`.
- [ ] Dodać zmienne środowiskowe `envp`.
- [x] Dodać ścieżkę i argumenty do `spawn`; rozszerzyć argumenty `exec`.
- [x] Walidować segmenty ELF, zakres użytkownika i uprawnienia W^X.

**Gotowe, gdy:** shell uruchamia `/bin/hello argument` z ramfs, a program odbiera
poprawne `argc` i `argv`.

## 6. Initramfs i podstawowy userspace — następny etap

- [x] Używać initramfs w formacie USTAR.
- [x] Budować osobne ELF-y i pakować je do `initramfs.tar`.
- [x] Ładować initramfs jako moduł Limine i montować go przy starcie.
- [x] Wydzielić shell jako `/bin/shell` działający w Ring 3.
- [x] Uruchamiać `/bin/init` jako PID 1 i automatycznie restartować shell.
- [x] Dodać osobne `/bin/shell`, `/bin/ls`, `/bin/cat`, `/bin/echo` i `/bin/uptime`.
- [x] Usunąć konieczność ręcznego wywoływania `runuser`.

**Gotowe, gdy:** kernel uruchamia `/bin/init`, a wszystkie polecenia użytkownika
są osobnymi plikami ELF i procesami Ring 3.

## 7. Sterownik PCI i urządzenie blokowe — funkcjonalnie ukończone

- [x] Zaimplementować enumerację PCI oraz odczyt BAR dla urządzeń VirtIO.
- [x] Dodać abstrakcję urządzenia blokowego z odczytem i zapisem pojedynczego sektora.
- [x] Wybrać pierwszy sterownik dysku dla QEMU: VirtIO Block (legacy PCI).
- [x] Obsłużyć odczyt i zapis sektorów z limitem czasu oraz błędami urządzenia.
- [~] Dodać test zapisu poza obrazem źródłowym systemu — odczyt sektora 0 jest testowany przy starcie; trwały zapis jest sprawdzany przez FAT32.

**Gotowe, gdy:** kernel stabilnie odczytuje i zapisuje bloki osobnego obrazu
dysku w QEMU bez korzystania z danych osadzonych w kernelu.

## 8. Trwały filesystem — w toku

- [~] Zaimplementować FAT32 jako pierwszy trwały filesystem — montowanie, listowanie i operacje na plikach 8.3 oraz nazwach VFAT działają.
- [~] Podłączyć FAT32 jako backend VFS pod `/disk`; montowanie partycji pozostaje do dodania.
- [~] Obsłużyć tworzenie, zapis, odczyt, listowanie i usuwanie plików w katalogach FAT32, w tym długie nazwy VFAT zapisane jako UTF-16. ABI przyjmuje ścieżki UTF-8 do 255 bajtów; porównywanie bez względu na wielkość liter obsługuje ASCII. Łańcuchy plików większe niż limit VFS pozostają do dodania.
- [~] Dodać synchronizację przez VirtIO FLUSH i syscall `sync`; obsługa journalingu oraz pełne odzyskiwanie po utracie zasilania pozostają do dodania.
- [~] Narzędzia userspace `rm`, `cp`, `mount` i `umount` istnieją; kopiowanie katalogów oraz montowanie innych typów filesystemów pozostają do dodania.

**Gotowe, gdy:** plik utworzony przez program Ring 3 pozostaje na obrazie dysku
po restarcie ConeOS.

## 9. Pamięć procesów i zaawansowane zarządzanie stronami

- [x] Kończyć proces Ring 3 po wyjątku CPU; wyjątek w jądrze nadal wywołuje panic.
- [x] Używać osobnego stosu IST dla page faultów każdego taska.
- [x] Dodać syscall `brk` z limitem sterty przed stosem procesu.
- [x] Udostępnić userspace prosty allocator `malloc`, `free`, `calloc`.
- [x] Dodać syscalle `mmap` i `munmap` dla prywatnych, anonimowych mapowań
  Ring 3 z ochroną stron i częściowym odmapowaniem; mapowania plików i
  współdzielone strony pozostają do dodania.
- [x] Wprowadzić lazy allocation dla sterty `brk` i anonimowych `mmap`;
  brakujące strony są zerowane przy pierwszym dostępie.
- [x] Dodać `fork()` z copy-on-write dla stron użytkownika i odziedziczeniem
  deskryptorów plików.
- [x] Przebudować `exec`, aby zastępował obraz bieżącego procesu, zachowując
  PID i deskryptory oraz przekazując wektor `argv`.
- [x] Ograniczyć pamięć procesu do 16 MiB stron ELF, stosu oraz rezerwacji
  `brk`/`mmap`; przekroczenie zwraca `ENOMEM`, a brak fizycznej strony zachowuje
  4 MiB rezerwy dla kernela i kończy tylko proces, który jej potrzebował.
- [x] Wzmocnić separację mapowań kernela i userspace: kod RX, stałe tylko do
  odczytu także przez aliasy pamięci fizycznej, dane/stosy/sterta NX oraz
  CR0.WP. Włączyć SMEP/SMAP, jeśli CPU je obsługuje, i czyścić flagę AC przy
  wejściu do kernela. `make test-isolation` sprawdza błędne dostępy, syscalle,
  COW i `exec` na modelach CPU QEMU z SMEP/SMAP oraz bez tych rozszerzeń.

**Gotowe, gdy:** proces może dynamicznie zarządzać pamięcią, a błędny dostęp
kończy wyłącznie ten proces, nie cały system.

## 10. Wielordzeniowość, synchronizacja i stabilizacja

- Uruchomić dodatkowe rdzenie przez Limine SMP.
- [x] Odczytać ACPI RSDP, RSDT/XSDT i MADT, wykrywać CPU oraz kontrolery
  przerwań i walidować tablice firmware.
- [~] Dodać Local APIC, IOAPIC i timery per CPU — xAPIC obsługuje przerwania ISA
  na rdzeniu startowym, nadpisania tras IRQ oraz wpisy NMI. PIT nadal dostarcza
  zegar; timery APIC per CPU i tryb x2APIC pozostają do dodania. PIC jest
  używany przy braku ACPI/APIC lub nieobsługiwanej konfiguracji.
- Zaimplementować spinlocki, mutexy, wait queues i dane per CPU.
- Rozszerzyć scheduler o kolejki per CPU i migrację zadań.
- [~] Dodać automatyczne testy bootowania w QEMU, testy regresji syscalli,
  filesystemu i pamięci oraz tryb diagnostyczny kernela — `make test-acpi`,
  `make test-apic` i `make test-isolation` sprawdzają parser MADT, rozruch
  BIOS/UEFI, klawiaturę, timer, przejście na PIC i izolację procesów. Pełne
  testy filesystemu i tryb diagnostyczny pozostają do dodania.

**Gotowe, gdy:** ConeOS przechodzi zestaw testów na co najmniej dwóch vCPU i nie
ma znanych race condition podczas równoległego I/O oraz pracy procesów.

## Zasada rozwoju

Każdy krok powinien zawierać test uruchamiany w QEMU i test ścieżki błędu.
Nowe funkcje kernela powinny być wystawiane userspace przez stabilne ABI zamiast
bezpośrednio rozbudowywać wbudowany shell kernela.
