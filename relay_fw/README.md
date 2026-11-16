# LakeLink relay firmware (STM32WLE5JC on the LoRa-E5 mini)

Runs the relay directly on the radio board: no computer, no ESP32. Same hop protocol as
`tools/hopnet.py` / `tools/e5_node.py --role relay` (frames verified byte-identical, see test/host).

Build: `pio run`. Host tests: see test/host (RFC 4231 HMAC vectors + parity with hopnet.py).
Flash (ERASES the factory AT firmware for good): ST-Link V2 on SWDIO, SWCLK, RST, GND, 3V3, board USB-C unplugged.

## Status
| Item | State |
|---|---|
| Crypto + link frames | done, host-tested (RFC 4231, byte parity with Python) |
| Relay logic (ack, repeat detection, queue, retries/backoff, hop limit) | written, compiles, **not yet run on hardware** |
| Duty-cycle limiter per EU868 sub-band (incl. acknowledgements) | done (RAM only) |
| Watchdog (8 s) | done |
| **Antenna switch mode for the Wio-E5-LE (LP vs HP path)** | **UNVERIFIED** - `RFSW_USE_LP` build flag; confirm before full power |
| Queue persistence in flash (custody across power loss) | **not yet** - power loss loses queued packets |
| Airtime log persistence across reboot | not yet |
| Scheduled wake-up / sleep | not yet - listens continuously (~6-7 mA, ~5 weeks on 2x18650) |
| Which UART `Serial` uses on this board variant | to confirm on hardware (log output) |
