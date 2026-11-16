# Range test 2026-10-14: walk away from the building

- **Receiver:** LoRa-E5 mini (DevEui …73:40:02:C8) on the laptop, **indoors at desk height**, stock rubber-duck antenna.
- **Transmitter:** LoRa-E5 mini on the phone (USB-OTG, Serial USB Terminal), stock rubber-duck antenna, carried by hand.
- **RF:** 868.1 MHz, SF9, 125 kHz, 14 dBm. One 33-byte packet every 30 s (repeat of the same test frame).
- **Result:** 29 of 30 packets received (10:03:16 to 10:18:17). One lost at the far end (10:14:16).

| Steps from laptop (~m at 0.75 m/step) | RSSI (dBm) | SNR (dB) | Notes |
|---|---|---|---|
| 0 | −12 to −14 | 10–11 | next to the laptop |
| up to ~70 | −22 to −47 | 10–11 | inside / near the building |
| 142 (~105 m) | −92 to −103 | 5–10 | just outside the building; big drop from the walls |
| 200 (~150 m) | −90 to −95 | 9–10 | |
| 250 (~190 m) | −108 to −110 | −6 to 0 | |
| 276+ (~210–220 m) | −111 | −8 | edge: 1 packet lost, then recovered |

SF9 decodes down to about SNR −12.5 dB / RSSI ≈ −129 dBm.

## Interpretation

- The limit here is set mainly by the **receiver being indoors**: walls cost most of the budget before the first 100 m.
- Over open water, with both antennas outdoors and raised, expect several times this distance.
- Levers if more range is needed:
  - move the receiver outdoors or high up (biggest gain)
  - SF12, about +7.5 dB, roughly 2× distance; ~1.5 s airtime per packet still fits a 5-minute interval under the 1% duty cycle
  - higher-gain shore antenna (5–6 dBi, with transmit power lowered accordingly)

Raw log: `2026-10-14_building_walk_raw.txt`.
