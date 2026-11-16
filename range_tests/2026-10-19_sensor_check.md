# Sensor check 2026-10-19 (real weather station through station -> relay -> gateway, 10 s readings)

| Sensor | Action | Seen at the gateway | Result |
|---|---|---|---|
| Wind speed | cups spun by hand | 3.44 / 1.89 / 3.75 / 2.33 m/s, gust up to 4.0, then back to 0 | pass |
| Wind direction | vane held N, E, S, W | 0, 90, 180, 270 deg (68 deg while turning past ENE) | pass |
| Rain | water poured slowly | 0 -> 0.6 -> 1.4 mm (2 then 5 tips of 0.2794 mm), steady afterwards | pass |
| Soil moisture | wet test skipped (water near electronics) | 2-7 % with probe in air, responds | not tested wet |
| Lightning | not possible indoors | 0 | not tested |
| Temperature | none | rose 21.5 -> 25.8 C over 11 min: likely board self-heating / hand near sensor | note |

Notes:
- The tipping-bucket gauge drains through its bottom by design: mount it so it never drains onto electronics.
- Wind direction is sent in whole degrees: 67.5 deg arrives as 68 (vane positions are 22.5 deg apart).
