# Radio module inventory

All boards are the same type: a **LoRa-E5 mini** development board (USB-C with a CP2102N USB-to-serial chip,
labelled on the back) carrying a **Wio-E5-LE-HF** radio module (labelled on the front). The LE module's
maximum transmit power is 14 dBm. No separate USB-to-serial adapter is needed.

Identify a module by its **DevEui** (`AT+ID`) or its USB chip serial number, never by the Mac port
name. `/dev/cu.usbserial-130` / `-120` only reflect which USB socket is used, so different modules
get the same name.

| # | Module | DevEui | USB chip (CP2102N) serial | Found as | Current mode | Role / notes |
|---|---|---|---|---|---|---|
| 1 | LoRa-E5 mini board + Wio-E5-LE-HF | 2C:F7:F1:20:73:40:11:63 | f233d661ca67f011bbeffff6f3c77411 | factory, firmware 4.0.11, EU868 | TEST (set 2026-10-11) | laptop listener / future gateway radio |
| 2 | LoRa-E5 mini board + Wio-E5-LE-HF | 2C:F7:F1:20:73:40:02:C8 | 7283499cc067f011ba030cf7f3c77411 | factory LoRaWAN ABP, firmware 4.0.11, EU868, 14 dBm | TEST (set 2026-10-11) | range-test module on the phone |
| 3 | LoRa-E5 mini board + Wio-E5-LE-HF | 2C:F7:F1:20:75:30:AA:8A | b03fccd73b1af111ae640d77929fd6db | factory LoRaWAN ABP, firmware 4.0.11, EU868, 14 dBm, class A | LoRaWAN ABP (untouched) | added 2026-10-12, no role yet |

To return a module to its factory LoRaWAN mode: `AT+MODE=LWABP`.
TEST-mode radio settings (`AT+TEST=RFCFG,...`) are lost on power loss and must be resent after every replug.

## Weather station ESP32
MicroMod ESP32, MAC 34:98:7a:4c:b2:00, USB chip CP2102N serial 90f34758ba91ed1187c8ccd13b20a988.
- Original firmware ("Hava Istasyonu (SIM800L)") backed up in ~/Documents/original_firmware_backups/.
- 2026-10-16: reflashed with the builder's earlier WeatherStation.ino (no SIM800L; TELEM every 3 s).
- Plug it directly into the computer: through a USB hub it went silent and could not be programmed.
