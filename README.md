# ConeOS

A hobby x86-64 operating system kernel using the Limine 9.x bootloader.

## Build and run

Install the host dependencies on Nobara/Fedora:

```sh
sudo dnf install gcc binutils make git xorriso qemu-system-x86-core
```

Then run:

```sh
git submodule update --init
make run
```

Flanterm is included as a Git submodule. The first build downloads the binary
release branch of Limine 9.x. Use `make` to only build `ConeOS.iso`, or
`make clean` to remove generated build files. `make run` starts QEMU with two
vCPUs by default; use `make run QEMU_CPUS=4` to boot with four. The SMP boot
messages appear on the serial console.

## Isolation tests

```sh
make test-isolation
```

This boots a separate test ISO on QEMU's `max` and `qemu64` CPU models. It
checks that kernel memory access, writes to user code and execution from the
user stack terminate only the offending process. It also checks syscall
pointer validation, W^X, user AC flags, copy-on-write and `exec`. Logs are saved
to `build/isolation-*.log`; the normal init and `ConeOS.iso` retain their usual
boot behavior. Set `CONEOS_TEST_TIMEOUT` to change the default 20 seconds per
CPU. The test runner requires `timeout` and `ripgrep` in addition to build tools.

The same userspace test is available as `/bin/isolation-test` in ConeOS.

## ACPI and APIC tests

```sh
make test-acpi
make test-apic
```

`test-acpi` checks MADT parsing, including damaged checksums, truncated records,
duplicate entries, invalid interrupt flags and topology limits. `test-apic`
also boots the normal ISO on Q35 with four vCPUs and i440FX with two, types
commands through the PS/2 keyboard using QMP, and checks that uptime advances.
It repeats keyboard and timer checks with ACPI disabled and with the CPU APIC
feature disabled to verify the PIC fallback. Logs are in `build/apic-*.log`.
Python 3 is required for the boot tests; `CONEOS_TEST_TIMEOUT` controls the
default 30-second wait for each expected guest response.

When an OVMF code image is available, the runner also tests UEFI boot. Set
`CONEOS_OVMF_CODE` and `CONEOS_OVMF_VARS` to select a matching pair of raw
firmware images. Firmware variables are copied to a temporary file for testing.

ACPI support currently covers RSDP, RSDT/XSDT and MADT discovery. APIC uses
xAPIC MMIO and routes ISA interrupts to the boot CPU, including MADT overrides
and NMI configuration. The scheduler still runs on that CPU, and PIT supplies
the system clock. Secondary CPUs run periodic local APIC timers with separate
tick counters; scheduling on those CPUs, x2APIC mode and PCI interrupt routing
through ACPI AML remain future work. The parser supports up to 64 CPU
records, eight IOAPICs and one MiB per ACPI table; unsupported or invalid
topologies use PIC when available. A valid MADT reporting no legacy PIC causes
boot to stop with a diagnostic if APIC cannot be initialized.

## Implemented

- framebuffer
- PS/2 driver
- CMOS real-time clock
- Ring 3 processes loaded from ELF64 executables
- `fork()` with copy-on-write user pages and inherited file descriptors
- `exec` replaces the current process image and accepts an `argv` vector
- in-memory ramfs with `ls`, `cat` and `write`
- process pipes with `pipe`, `dup`, `dup2` and one `|` between shell commands
- private anonymous `mmap`/`munmap` page ranges for Ring 3 processes
- lazy zero-filled page allocation on first access for `mmap` and `brk`
- 16 MiB committed user-memory limit per process and a 4 MiB kernel page reserve
- kernel code mapped RX, constants read-only, and writable memory non-executable
- read-only kernel code and constants in physical-memory aliases as well
- supervisor write protection, with SMEP and SMAP enabled when supported
- ACPI MADT CPU and interrupt-controller discovery
- Local APIC and IOAPIC routing for PIT and PS/2, with a legacy PIC fallback
- Secondary CPUs started through Limine SMP with CPU data sized from the boot
  response and separate guarded kernel stacks, GDT/TSS and loaded IDT; they
  handle local APIC timer interrupts while task scheduling stays on the boot CPU
- IRQ-saving spinlocks protect physical pages, the kernel heap, virtual memory
  tables and kernel stack allocation across CPUs
