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
256 KB `coredump` partition). After a crash that reboots, read it back:

```sh
idf.py -p /dev/cu.usbmodem2101 coredump-info      # summary + backtrace
idf.py -p /dev/cu.usbmodem2101 coredump-debug     # GDB session on the dump
```

If it reports `Core dump version "0xffff"` / incorrect size, the partition is
**erased** — no dump was written, i.e. the panic handler never ran (hard wedge, or
the reboot came from a USB re-enumeration rather than a caught fault).

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
