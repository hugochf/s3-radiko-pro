# Debugging runbook

Practical how-to for diagnosing this board. The war stories behind these live in
[../PLAN.md](../PLAN.md) *Lessons learned*; this is the operational cheat-sheet.

## Serial console

The board uses the ESP32-S3 **native USB-Serial-JTAG** (one cable does flash +
monitor + JTAG). It enumerates as `/dev/cu.usbmodem*` on macOS.

```sh
. ~/esp/v5.3.5/esp-idf/export.sh
cd idf
idf.py -p /dev/cu.usbmodem2101 monitor      # Ctrl-] to quit
```

### Passive capture (does NOT reset the chip)

`idf.py monitor` and esptool toggle RTS/DTR, which resets the S3 — bad when you're
trying to observe a *running* fault. For read-only capture, use a passive reader
that leaves RTS/DTR alone:

```python
import serial, sys, time
s = serial.Serial()
s.port = '/dev/cu.usbmodem2101'; s.baudrate = 115200
s.timeout = 0.2; s.rts = False; s.dtr = False      # never toggle — that wedges USB-JTAG
s.open()
t0 = time.time()
while time.time() - t0 < 30:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode('utf-8', 'replace')); sys.stdout.flush()
s.close()
```

Run it with the IDF Python env (which already has `pyserial`):
`~/.espressif/python_env/idf5.3_py3.14_env/bin/python reader.py`.

## USB-JTAG wedge recovery

**Symptom:** esptool reports `A fatal error occurred: Failed to connect to
ESP32-S3: No serial data received`, or serial capture returns nothing at all.

**Cause:** the firmware is hard-wedged (an exception the panic handler can't
service — e.g. a fault while the flash cache is disabled). The USB-Serial-JTAG
peripheral is on the same die, so it dies with the CPU. Toggling RTS/DTR while in
this state makes it worse.

**Recovery:** physically **unplug and replug** the USB cable, then flash
immediately. There is no software reset that recovers a fully wedged S3.

> A silent wedge with *no* serial output and *no* coredump is the signature of a
> fault while the cache was disabled (typically an ISR or code running from
> uncached flash during a flash write). A normal exception prints a *Guru
> Meditation* backtrace and writes a coredump — see below.

## Reading a crash

### Live backtrace

A catchable panic (LoadProhibited, assert, or a task-watchdog trip) prints a
`Backtrace: 0xADDR:0xADDR ...` line. Symbolize it against the ELF:

```sh
xtensa-esp32s3-elf-addr2line -pfiaC -e build/s3_radiko_pro.elf 0x4201e1b1 0x4201e62e ...
```

### Coredump from flash

Coredump-to-flash is enabled (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, ELF format;
256 KB `coredump` partition). Since Phase 18, task-watchdog starvation also
panics, so wedges land here too instead of freezing silently.

**On-device (no host needed):** every boot, `crashlog_check()` looks for a
stored dump and logs the decoded summary — crashed task, PC, and an
addr2line-ready backtrace line — and Settings ▸ System Info shows a
`Last crash:` one-liner. The dump stays in flash until the next crash
overwrites it. (Verified end-to-end with a deliberate NULL-store: panic →
dump saved → reboot → summary named the exact task and source line.)

**Host-side, for the full post-mortem:**

```sh
idf.py -p /dev/cu.usbmodem2101 coredump-info      # summary + backtrace
idf.py -p /dev/cu.usbmodem2101 coredump-debug     # GDB session on the dump
```

If it reports `Core dump version "0xffff"` / incorrect size, the partition is
**erased** — no dump was written, i.e. the panic handler never ran (hard wedge, or
the reboot came from a USB re-enumeration rather than a caught fault).
`coredump-info` must run against the **same build's ELF** that crashed — after a
reflash with new code, the stored dump symbolizes wrong.

### Persistent event log (elog)

Warnings/errors plus a boot marker (reset reason, version) are captured into a
64 KB raw flash ring (`elog` partition) that survives reboots, crashes, and
reflashes — "what happened before it died" is answerable after the fact:

```sh
parttool.py -p /dev/cu.usbmodem2101 read_partition --partition-name=elog --output elog.bin
python3 tools/elog_dump.py elog.bin      # oldest-first, ring reassembled
```

INFO chatter is deliberately excluded (rate + flash wear); the console stream
itself is unaffected. Staged lines flush to flash every 10 s, so the very last
seconds before a hard power cut can be missing — a crash via panic is fine
(the coredump covers it).

### Live debugging over USB-JTAG (Phase 24)

The board's one USB cable also carries JTAG (the USB-Serial-JTAG peripheral
exposes both interfaces). This gives breakpoints, watchpoints, single-step,
and live inspection of every task — on the running radio.

**Terminal flow:**

```sh
# Terminal 1 — the bridge (leave running):
source ~/esp/v5.3.5/esp-idf/export.sh
openocd -f board/esp32s3-builtin.cfg

# Terminal 2 — the debugger:
xtensa-esp32s3-elf-gdb idf/build/s3_radiko_pro.elf \
    -ex "target extended-remote :3333"
```

Essentials at the `(gdb)` prompt: `break ev_next` / `break ui.c:899`,
`continue`, `bt`, `info threads`, `print 'ui.c'::s_cur` (file-qualify
statics), `next`/`step`, `watch var`, `delete`, `detach`.

**IDE flow:** `.vscode/launch.json` (machine-specific, untracked) attaches
Cursor/VS Code's visual debugger to OpenOCD — gutter breakpoints, variable
hover, thread list. Needs the C/C++ extension and OpenOCD already running.

**Rules learned the hard way:**

- **The ELF must be the EXACT build running on the chip.** A mismatched ELF
  sets breakpoints at addresses where nothing lives — they simply never fire
  (an OTA'd device runs the CI build, not your last local build: reflash
  locally before debugging, or fetch the release ELF). Same law as
  `coredump-info`.
- **GDB answers `print` from the ELF file when no target is attached** —
  plausible-looking values (the compile-time initializers) that are
  completely fake. Check for "The program is not being run".
- Halting the CPUs does NOT stop audio immediately: I2S DMA is hardware and
  drains the 30 s PCM ring while both cores stand frozen.
- A killed GDB can leave OpenOCD's gdb port stale ("Remote connection
  closed" on reconnect) — restart OpenOCD.

## Diagnostic build options

Enabled in `idf/sdkconfig.defaults` (were the tools that isolated the Phase 13
hang; kept for the Tier D hardening work):

| Option | Effect |
|--------|--------|
| `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` | Hardware watchpoint at each task's stack end → an overflow faults *immediately* with a backtrace, instead of silently corrupting and wedging. Stronger than the default canary (which only checks at context-switch). |
| `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` (+ `_DATA_FORMAT_ELF`, `_CHECKSUM_CRC32`) | Save an ELF coredump for post-mortem `coredump-info`/`-debug`. |

`CONFIG_COMPILER_STACK_CHECK_MODE_NONE` is still the default, so **"no crash" does
not prove "no stack overflow"** — trust the watchpoint, not the absence of output.

## Task watchdog

Configured at 5 s, watching both idle tasks
(`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1`). A trip prints which task starved
the idle task and its backtrace, e.g.:

```
task_wdt: ... did not reset ... - IDLE1 (CPU 1)
task_wdt: CPU 1: lvgl
```

That means the named task ran without yielding for 5 s. On this board the usual
cause is a busy-wait on core 1 (the classic was LVGL's default flush spin — see
[architecture.md](architecture.md) *Concurrency & flush notes*). Fix the
non-yielding loop; do not just raise the timeout.

## Quick reference

| Thing | Value |
|-------|-------|
| Port | `/dev/cu.usbmodem2101` (native USB-Serial-JTAG) |
| IDF Python (has pyserial) | `~/.espressif/python_env/idf5.3_py3.14_env/bin/python` |
| ELF for symbolizing | `idf/build/s3_radiko_pro.elf` |
| Toolchain source | `. ~/esp/v5.3.5/esp-idf/export.sh` |
| Recover a wedge | physical unplug/replug, then flash |
