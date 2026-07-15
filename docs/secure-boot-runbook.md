# Secure Boot + Flash Encryption — Stage B burn runbook

**STATUS: not executed.** This is the written procedure for the irreversible
eFuse burn (Phase 25 Stage B). Stage A (signed-OTA enforcement, reversible) is
live; this document is what you'd follow to make the chip *enforce* it in
hardware — and it is deliberately parked here rather than run.

> ⚠️ **eFuses are physical and one-time.** Nothing below can be undone. Read
> the whole document before touching a single command. Do this on a
> **sacrificial board**, never the working radio, the first time.

## What each burn costs, in one table

| Step | Gains | Permanently loses |
|------|-------|-------------------|
| Secure Boot v2 (dev mode) | ROM refuses unsigned bootloader/app | some ROM DL-mode conveniences |
| Secure Boot v2 (release) | full chain of trust | **JTAG disabled**, DL-mode locked |
| Flash encryption (dev) | flash unreadable when desoldered | plaintext `idf.py flash` (limited re-encrypt count remains) |
| Flash encryption (release) | + no plaintext ever | USB reflashing effectively gone; OTA is the ONLY update path |

A lost private key at this point = the board can never boot new firmware
again. Back the key up (below) BEFORE anything else.

## Preconditions

- [ ] `~/.s3-radiko/signing_key.pem` backed up to **two** offline locations
      (encrypted USB, password manager). Verify: `espsecure.py
      signature_info_v2` on a signed build shows a valid block.
- [ ] Public-key digest recorded (from a signed build, so you can confirm the
      eFuse burned the value you expected):
      `espsecure.py digest_rsa_public_key --keyfile signing_key.pem`.
- [ ] The signed profile boots and OTAs a signed release cleanly (Stage A
      verification passed).
- [ ] Sacrificial board on hand. This is not run on the working radio first.
- [ ] `espefuse.py summary` saved for the target board (know the starting
      fuse state).

## Procedure (development mode — keeps JTAG, allows reflash)

Secure boot v2 and flash encryption are enabled via `menuconfig` →
*Security features*, then the FIRST boot burns the fuses. Build/flash with the
signed profile's sdkconfig so signing is already wired.

```
# 1. Enable in config (menuconfig, or add to a Stage-B-only overlay):
#    CONFIG_SECURE_BOOT=y
#    CONFIG_SECURE_BOOT_V2_ENABLED=y
#    CONFIG_SECURE_FLASH_ENC_ENABLED=y
#    CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y   # NOT release, first time

# 2. Build the bootloader + app (signed).
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.signed;sdkconfig.secureboot" build

# 3. Flash ONCE over USB. The bootloader burns the fuses on first boot:
#    - writes the secure-boot key digest
#    - generates + writes the flash-encryption key (never leaves the chip)
#    - encrypts the flash in place, then reboots
idf.py -p PORT flash monitor
#    Watch for: "Secure boot permanently enabled", "Flash encryption
#    completed". This first boot is slow (it encrypts everything).
```

After this, plaintext `idf.py flash` no longer works as-is; use
`idf.py encrypted-flash` (dev mode) or ship signed OTA images.

## Release mode (only after dev mode is fully understood — the true one-way door)

Set `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y` and the secure-boot
release options, rebuild, flash once. This burns the fuses that **disable
JTAG and lock ROM download mode**. From here the ONLY firmware path is signed
OTA — a bug in the OTA client is unrecoverable. Do not do this on any board
you are not prepared to discard.

## Recovery reality

- Dev mode: limited re-encryption count remains; a signed OTA can still fix a
  bad app.
- Release mode: if signed OTA breaks, there is no recovery. The board is done.

## Why this is parked

The working radio is a single, in-use device with no sacrificial twin. Stage A
already delivers signed updates and closes the "anyone with repo access can
push firmware" gap in software. The hardware enforcement here protects against
*physical* attackers — not this project's threat model. Kept as knowledge and
as a factory-ready procedure, executed only on a board bought to be burned.
