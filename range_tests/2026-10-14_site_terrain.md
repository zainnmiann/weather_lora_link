# Site terrain check 2026-10-14 (office → lake)

- Office (gateway): 41.1059129, 29.0201098, ground about 118 m
- Lake (boat): 41.1025725, 29.0306532, water about 76 m
- **Straight-line distance: 958 m**, bearing 113° (east-south-east) from the office. The user's 2.2–2.5 km figure is likely the walking or road distance.

Terrain profile along the direct line (Open-Meteo elevation, ~30–90 m grid, terrain only: **buildings and trees not included**):

```
elev (m)
 120 |O                                                    O = office (118 m)
 110 | ▀▀▀▀▀▀              ▄▄▄▄                            ridge 105–109 m, 620–720 m out
 100 |      ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀    ▀▀▀▀▄
  90 |                              ▀▄
  80 |                                ▀▀ L                 L = lake (76 m)
     +----+----+----+----+----+----+----+---- distance (m)
     0   120  240  360  480  600  720  840  958
```

- **Direct link blocked:** a straight line from an antenna 6 m above the office to 2 m above the boat passes ~20 m **below** the ridge near the lake. The lake sits in a hollow.
- **Best relay spot found (terrain only):** **41.10509, 29.02815**, ground ~117 m. 350 m to the lake, 681 m to the office, with positive clearance to both for an antenna about 6 m up. Runner-up: 41.10257, 29.02898 (west rim, ~103 m ground), 140 m to the lake, 832 m to the office.
- Clearance is only 1–7 m over bare terrain, less than the ~5–9 m Fresnel zone, and the model ignores trees and buildings. Treat these spots as **candidates to test**, not guarantees.

Plan: one relay on the rim gives boat → relay → office gateway, which uses exactly the 3 boards already owned. Test each hop with the phone/laptop method.
