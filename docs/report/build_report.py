#!/usr/bin/env python3
"""Builds docs/report/Lake_Weather_LoRa_Network_Report.pdf from the project's measured data.

Run: python3 docs/report/build_report.py   (needs reportlab; uses macOS Arial/Menlo fonts)
"""

import re
from pathlib import Path

from reportlab.graphics.charts.barcharts import VerticalBarChart
from reportlab.graphics.charts.lineplots import LinePlot
from reportlab.graphics.shapes import Drawing, Line, Polygon, PolyLine, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (KeepTogether, PageBreak, Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "report" / "Lake_Weather_LoRa_Network_Report.pdf"
RUNS = ROOT / "range_tests" / "chain_runs"

# ---- fonts ----------------------------------------------------------------------------------
SUP = "/System/Library/Fonts/Supplemental/"
pdfmetrics.registerFont(TTFont("Body", SUP + "Arial.ttf"))
pdfmetrics.registerFont(TTFont("Body-Bold", SUP + "Arial Bold.ttf"))
pdfmetrics.registerFont(TTFont("Body-Italic", SUP + "Arial Italic.ttf"))
pdfmetrics.registerFont(TTFont("Body-BoldItalic", SUP + "Arial Bold Italic.ttf"))
pdfmetrics.registerFont(TTFont("Mono", "/System/Library/Fonts/Menlo.ttc", subfontIndex=0))
pdfmetrics.registerFontFamily("Body", normal="Body", bold="Body-Bold", italic="Body-Italic", boldItalic="Body-BoldItalic")

# ---- palette (reference data-viz palette, light mode) ---------------------------------------
INK = colors.HexColor("#0b0b0b")
INK2 = colors.HexColor("#52514e")
MUTED = colors.HexColor("#8a8984")
GRID = colors.HexColor("#e5e4df")
SURF = colors.HexColor("#f6f5f2")
BLUE = colors.HexColor("#2a78d6")
ORANGE = colors.HexColor("#eb6834")
AQUA = colors.HexColor("#1baf7a")
VIOLET = colors.HexColor("#6250d6")
BLUE_L = colors.HexColor("#e8f1fb")
ORANGE_L = colors.HexColor("#fdeee6")
AQUA_L = colors.HexColor("#e4f5ee")
VIOLET_L = colors.HexColor("#eeecfb")
GOOD = colors.HexColor("#1a7f37")
WARN = colors.HexColor("#9a5b00")
BAD = colors.HexColor("#b42318")

# ---- styles ---------------------------------------------------------------------------------
S = {
    "title": ParagraphStyle("title", fontName="Body-Bold", fontSize=24, leading=29, textColor=INK, spaceAfter=6),
    "subtitle": ParagraphStyle("subtitle", fontName="Body", fontSize=12.5, leading=17, textColor=INK2),
    "h1": ParagraphStyle("h1", fontName="Body-Bold", fontSize=16, leading=20, textColor=INK, spaceBefore=6, spaceAfter=8, keepWithNext=1),
    "h2": ParagraphStyle("h2", fontName="Body-Bold", fontSize=12, leading=15, textColor=INK, spaceBefore=10, spaceAfter=5, keepWithNext=1),
    "body": ParagraphStyle("body", fontName="Body", fontSize=9.6, leading=13.6, textColor=INK, spaceAfter=6),
    "small": ParagraphStyle("small", fontName="Body", fontSize=8.3, leading=11, textColor=INK2),
    "cap": ParagraphStyle("cap", fontName="Body-Italic", fontSize=8.3, leading=11, textColor=INK2, spaceBefore=3, spaceAfter=10),
    "cell": ParagraphStyle("cell", fontName="Body", fontSize=8.3, leading=10.6, textColor=INK),
    "cellb": ParagraphStyle("cellb", fontName="Body-Bold", fontSize=8.3, leading=10.6, textColor=INK),
    "mono": ParagraphStyle("mono", fontName="Mono", fontSize=7.6, leading=10, textColor=INK, backColor=SURF,
                           borderPadding=(4, 5, 4, 5), spaceBefore=3, spaceAfter=8),
    "bullet": ParagraphStyle("bullet", fontName="Body", fontSize=9.6, leading=13.4, textColor=INK, leftIndent=12,
                             bulletIndent=2, spaceAfter=2.5),
    "callout": ParagraphStyle("callout", fontName="Body", fontSize=9.4, leading=13.2, textColor=INK, backColor=BLUE_L,
                              borderPadding=(7, 8, 7, 8), spaceBefore=4, spaceAfter=10),
}


def P(text, style="body"):
    return Paragraph(text, S[style])


def bullets(items):
    return [Paragraph(t, S["bullet"], bulletText="•") for t in items]


def table(rows, widths, header=True, zebra=True, align=None):
    data = [[c if not isinstance(c, str) else Paragraph(c, S["cellb" if (header and i == 0) else "cell"])
             for c in row] for i, row in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1 if header else 0)
    st = [("VALIGN", (0, 0), (-1, -1), "TOP"), ("TOPPADDING", (0, 0), (-1, -1), 3.5),
          ("BOTTOMPADDING", (0, 0), (-1, -1), 3.5), ("LEFTPADDING", (0, 0), (-1, -1), 4),
          ("RIGHTPADDING", (0, 0), (-1, -1), 4), ("LINEBELOW", (0, 0), (-1, -1), 0.4, GRID)]
    if header:
        st += [("BACKGROUND", (0, 0), (-1, 0), SURF), ("LINEBELOW", (0, 0), (-1, 0), 0.8, MUTED)]
    if zebra:
        for r in range(2 if header else 1, len(rows), 2):
            st.append(("BACKGROUND", (0, r), (-1, r), colors.HexColor("#fbfaf8")))
    t.setStyle(TableStyle(st + (align or [])))
    return t


# ---- drawing helpers ------------------------------------------------------------------------
def box(d, x, y, w, h, title, lines=(), fill=BLUE_L, stroke=BLUE, title_size=8.6):
    d.add(Rect(x, y, w, h, rx=5, ry=5, fillColor=fill, strokeColor=stroke, strokeWidth=1))
    d.add(String(x + w / 2, y + h - 13, title, fontName="Body-Bold", fontSize=title_size, fillColor=INK, textAnchor="middle"))
    for i, ln in enumerate(lines):
        d.add(String(x + w / 2, y + h - 25 - i * 10, ln, fontName="Body", fontSize=7.2, fillColor=INK2, textAnchor="middle"))


def arrow(d, x1, y1, x2, y2, color=INK2, width=1.2, label=None, dashed=False, label_dy=4):
    ln = Line(x1, y1, x2, y2, strokeColor=color, strokeWidth=width)
    if dashed:
        ln.strokeDashArray = [3, 2]
    d.add(ln)
    import math
    a = math.atan2(y2 - y1, x2 - x1)
    s = 5
    p1 = (x2 - s * math.cos(a - 0.45), y2 - s * math.sin(a - 0.45))
    p2 = (x2 - s * math.cos(a + 0.45), y2 - s * math.sin(a + 0.45))
    d.add(Polygon([x2, y2, p1[0], p1[1], p2[0], p2[1]], fillColor=color, strokeColor=color, strokeWidth=0.5))
    if label:
        d.add(String((x1 + x2) / 2, (y1 + y2) / 2 + label_dy, label, fontName="Body", fontSize=7, fillColor=INK2, textAnchor="middle"))


W = 170 * mm  # usable text width


def fig_architecture():
    d = Drawing(W, 150)
    y = 62
    box(d, 0, y, 92, 70, "Boat (station)", ["Weather sensors", "ESP32 (MicroMod)", "LoRa-E5 mini"], AQUA_L, AQUA)
    box(d, 120, y, 86, 70, "Relay", ["LoRa-E5 mini", "2×18650 battery", "on high ground"], VIOLET_L, VIOLET)
    box(d, 234, y, 96, 70, "Gateway (lake)", ["LoRa-E5 mini", "+ controller", "+ IoT SIM (cellular)"], BLUE_L, BLUE)
    box(d, 358, y, 62, 70, "AWS", ["database", "(+ forecasts)"], ORANGE_L, ORANGE)
    box(d, 438, y, 44, 70, "Website", ["charts"], SURF, MUTED, 8)
    arrow(d, 92, y + 40, 120, y + 40, BLUE, label="LoRa")
    arrow(d, 206, y + 40, 234, y + 40, BLUE, label="LoRa")
    arrow(d, 330, y + 40, 358, y + 40, ORANGE, label="4G/2G")
    arrow(d, 420, y + 40, 438, y + 40, ORANGE)
    arrow(d, 120, y + 22, 92, y + 22, MUTED, 0.9, label="ack", dashed=True, label_dy=-10)
    arrow(d, 234, y + 22, 206, y + 22, MUTED, 0.9, label="ack", dashed=True, label_dy=-10)
    d.add(String(46, 40, "on the lake (moving)", fontName="Body-Italic", fontSize=7.2, fillColor=MUTED, textAnchor="middle"))
    d.add(String(163, 40, "0, 1 or 2 relays", fontName="Body-Italic", fontSize=7.2, fillColor=MUTED, textAnchor="middle"))
    d.add(String(282, 40, "lake edge, has mobile signal", fontName="Body-Italic", fontSize=7.2, fillColor=MUTED, textAnchor="middle"))
    d.add(String(420, 40, "internet", fontName="Body-Italic", fontSize=7.2, fillColor=MUTED, textAnchor="middle"))
    d.add(Line(0, 22, W, 22, strokeColor=GRID, strokeWidth=0.6))
    d.add(String(0, 8, "Every hop is acknowledged separately; a node that cannot reach its next hop stores the reading and retries.",
                 fontName="Body", fontSize=7.4, fillColor=INK2))
    return d


def fig_previous_pipeline():
    d = Drawing(W, 110)
    y = 30
    box(d, 0, y, 110, 66, "Weather board", ["SparkFun MicroMod Weather", "ESP32 prints TELEM {json}"], AQUA_L, AQUA)
    box(d, 150, y, 110, 66, "Raspberry Pi", ["weather_logger.py", "(on Teknopark WiFi)"], SURF, MUTED)
    box(d, 300, y, 120, 66, "AWS EC2 server", ["PostgreSQL 'weather'", "table weather_data"], ORANGE_L, ORANGE)
    arrow(d, 110, y + 33, 150, y + 33, INK2, label="USB serial")
    arrow(d, 260, y + 33, 300, y + 33, ORANGE, label="SSH → psql")
    box(d, 300, y - 28, 120, 22, "Weather Underground (every 3 min)", (), SURF, MUTED, 7)
    arrow(d, 260, y + 14, 300, y - 17, MUTED, 0.9)
    return d


def fig_sequence():
    d = Drawing(W, 230)
    xs = {"Station": 60, "Relay": 240, "Gateway": 420}
    top, bot = 212, 12
    for name, x in xs.items():
        d.add(Rect(x - 38, top - 4, 76, 18, rx=4, ry=4, fillColor=SURF, strokeColor=MUTED, strokeWidth=0.8))
        d.add(String(x, top + 1.5, name, fontName="Body-Bold", fontSize=8.5, fillColor=INK, textAnchor="middle"))
        ln = Line(x, top - 4, x, bot, strokeColor=GRID, strokeWidth=1.2)
        d.add(ln)
    S_, R_, G_ = xs["Station"], xs["Relay"], xs["Gateway"]
    y = 190
    arrow(d, S_, y, R_, y - 10, BLUE, label="FWD (lseq 41, reading #7)", label_dy=6)
    d.add(String(R_ + 6, y - 24, "store #7 durably, then:", fontName="Body-Italic", fontSize=7, fillColor=INK2))
    arrow(d, R_, y - 32, S_, y - 40, MUTED, 1.0, label="HACK 41", dashed=True, label_dy=-11)
    arrow(d, R_, y - 54, G_, y - 64, BLUE, label="FWD (lseq 903, reading #7, hops=2)", label_dy=6)
    d.add(Polygon([G_ - 9, y - 70, G_ + 9, y - 70, G_, y - 79], fillColor=BAD, strokeColor=BAD))
    d.add(String(G_ + 12, y - 78, "ack lost on air", fontName="Body-Italic", fontSize=7, fillColor=BAD))
    arrow(d, G_, y - 86, R_ + 60, y - 92, MUTED, 1.0, dashed=True)
    d.add(String(R_ + 66, y - 101, "× HACK 903 (never arrives)", fontName="Body", fontSize=7, fillColor=BAD))
    d.add(String(R_ + 6, y - 114, "no ack in 2.5 s → retry same lseq", fontName="Body-Italic", fontSize=7, fillColor=INK2))
    arrow(d, R_, y - 130, G_, y - 140, BLUE, label="FWD (lseq 903) again", label_dy=6)
    d.add(String(G_ - 6, y - 154, "already seen → not delivered twice", fontName="Body-Italic", fontSize=7, fillColor=INK2, textAnchor="end"))
    arrow(d, G_, y - 160, R_, y - 168, MUTED, 1.0, label="HACK 903", dashed=True, label_dy=-11)
    d.add(String(G_, bot + 2, "verifies station signature,", fontName="Body", fontSize=7, fillColor=INK2, textAnchor="middle"))
    d.add(String(G_, bot - 7, "publishes reading #7 once", fontName="Body", fontSize=7, fillColor=INK2, textAnchor="middle"))
    return d


def frame_bar(d, x, y, fields, scale, title):
    d.add(String(x, y + 26, title, fontName="Body-Bold", fontSize=8.2, fillColor=INK))
    cx = x
    palette = [AQUA_L, BLUE_L, VIOLET_L, ORANGE_L, SURF]
    for i, (name, nbytes, kind) in enumerate(fields):
        w = max(nbytes * scale, 24)  # small fields get a readable minimum width (not to scale)
        fill = {"hdr": SURF, "id": BLUE_L, "data": AQUA_L, "mac": ORANGE_L, "inner": VIOLET_L}[kind]
        d.add(Rect(cx, y, w, 20, fillColor=fill, strokeColor=colors.white, strokeWidth=1.2))
        if w >= 20:
            d.add(String(cx + w / 2, y + 7, name, fontName="Body", fontSize=6.4, fillColor=INK, textAnchor="middle"))
        d.add(String(cx + w / 2, y - 9, str(nbytes), fontName="Body", fontSize=6, fillColor=MUTED, textAnchor="middle"))
        cx += w
    d.add(String(cx + 4, y + 7, f"= {sum(f[1] for f in fields)} bytes", fontName="Body-Bold", fontSize=7, fillColor=INK2))


def fig_frames():
    d = Drawing(W, 120)
    sc = 10.2
    frame_bar(d, 0, 78, [("ver", 1, "hdr"), ("type", 1, "hdr"), ("stn", 1, "id"), ("boot", 2, "id"), ("seq", 2, "id"),
                         ("temp,RH,press,wind,gust,dir,rain,batt,soil,lightning", 21, "data"), ("flags", 1, "data"),
                         ("MAC", 4, "mac")], sc, "End-to-end reading (signed by the station, opened only by the gateway)")
    frame_bar(d, 0, 18, [("'L'", 1, "hdr"), ("type", 1, "hdr"), ("src", 1, "id"), ("dst", 1, "id"), ("lseq", 2, "id"),
                         ("hops", 1, "id"), ("end-to-end reading (33 B, unchanged)", 33, "inner"), ("MAC", 4, "mac")],
              sc * 0.82, "Hop frame FWD (re-signed at every hop; HACK = same header, no payload, 11 B)")
    return d


def lineplot(series, xr, yr, w, h, xlabel, ylabel, colors_, xticks=None, yticks=None, refline=None, annotations=()):
    d = Drawing(w, h)
    lp = LinePlot()
    lp.x, lp.y, lp.width, lp.height = 42, 30, w - 56, h - 44
    lp.data = series
    lp.xValueAxis.valueMin, lp.xValueAxis.valueMax = xr
    lp.yValueAxis.valueMin, lp.yValueAxis.valueMax = yr
    if xticks:
        lp.xValueAxis.valueSteps = xticks
    if yticks:
        lp.yValueAxis.valueSteps = yticks
    for ax in (lp.xValueAxis, lp.yValueAxis):
        ax.labels.fontName, ax.labels.fontSize, ax.labels.fillColor = "Body", 7, INK2
        ax.strokeColor, ax.strokeWidth = MUTED, 0.6
    lp.yValueAxis.visibleGrid = True
    lp.yValueAxis.gridStrokeColor, lp.yValueAxis.gridStrokeWidth = GRID, 0.5
    lp.xValueAxis.visibleGrid = False
    for i, c in enumerate(colors_):
        lp.lines[i].strokeColor = c
        lp.lines[i].strokeWidth = 1.8
    d.add(lp)
    d.add(String(lp.x + lp.width / 2, 4, xlabel, fontName="Body", fontSize=7.4, fillColor=INK2, textAnchor="middle"))
    from reportlab.graphics.shapes import Group
    lab = Group(String(0, 0, ylabel, fontName="Body", fontSize=7.4, fillColor=INK2, textAnchor="middle"))
    lab.transform = (0, 1, -1, 0, 10, lp.y + lp.height / 2)
    d.add(lab)

    def X(v):
        return lp.x + (v - xr[0]) / (xr[1] - xr[0]) * lp.width

    def Y(v):
        return lp.y + (v - yr[0]) / (yr[1] - yr[0]) * lp.height
    if refline is not None:
        val, text = refline
        ln = Line(lp.x, Y(val), lp.x + lp.width, Y(val), strokeColor=BAD, strokeWidth=0.9)
        ln.strokeDashArray = [3, 2]
        d.add(ln)
        d.add(String(lp.x + lp.width - 2, Y(val) + 3, text, fontName="Body", fontSize=6.8, fillColor=BAD, textAnchor="end"))
    for xv, yv, text, anchor in annotations:
        d.add(Line(X(xv), lp.y, X(xv), lp.y + lp.height, strokeColor=GRID, strokeWidth=0.6))
        d.add(String(X(xv) + (2 if anchor == "start" else -2), Y(yv), text, fontName="Body", fontSize=6.6, fillColor=INK2,
                     textAnchor=anchor))
    return d, X, Y, lp


# ---- data -----------------------------------------------------------------------------------
def range_walk():
    rows = []
    for line in (ROOT / "range_tests" / "2026-10-14_building_walk_raw.txt").read_text().splitlines():
        m = re.match(r"(\d+):(\d+):(\d+) rssi=(-?\d+) snr=(-?[\d.]+)", line)
        if m:
            h, mi, s = map(int, m.group(1, 2, 3))
            t = (h * 3600 + mi * 60 + s) - (10 * 3600 + 3 * 60 + 16)
            rows.append((t / 60, int(m.group(4)), float(m.group(5))))
    return rows


TERRAIN = [(0, 118), (48, 108), (96, 107), (144, 109), (192, 109), (240, 108), (287, 101), (335, 100), (383, 102),
           (431, 102), (479, 102), (527, 102), (575, 102), (623, 105), (671, 109), (719, 109), (767, 103), (815, 103),
           (862, 88), (910, 76), (958, 76)]


def run_results():
    order = ["H1_baseline", "H2_relay_outage", "H3_gateway_outage", "H4_faults", "H4b_faults_backoff", "H5_radio_errors",
             "H6_station_reboot", "H7_duty_limit_868", "H7b_duty_limiter", "H8_relay_crash_holding",
             "R1_baseline", "R2_faults_backoff", "R3_relay_crash_holding", "R4_mqtt_outage", "R4b_mqtt_outage"]
    out = []
    for name in order:
        f = RUNS / name / "result.txt"
        if not f.exists():
            continue
        t = f.read_text()
        g = lambda pat, default="?": (re.search(pat, t).group(1) if re.search(pat, t) else default)
        lat = re.search(r"latency s \(min/median/max\): (\d+) / (\d+) / (\d+)", t)
        out.append(dict(name=name, made=int(g(r"readings made\s+: (\d+)", 0)), got=int(g(r"delivered \(unique\)\s+: (\d+)", 0)),
                        queued=g(r"still queued at stop\s+: (.*)"), dup=int(g(r"duplicates delivered\s+: (\d+)", 0)),
                        unexplained=int(g(r"of which unexplained\s+: (\d+)", 0)), order=g(r"order preserved: (\w+)"),
                        lat=tuple(map(int, lat.groups())) if lat else None, result=g(r"RESULT: ([A-Z\-]+)"),
                        events=t))
    return out


# ---- page decoration ------------------------------------------------------------------------
def on_page(c, doc):
    c.saveState()
    c.setFont("Body", 7.5)
    c.setFillColor(MUTED)
    c.drawString(20 * mm, 12 * mm, "Lake weather station · LoRa hop network · project report")
    c.drawRightString(190 * mm, 12 * mm, f"page {doc.page}")
    c.setStrokeColor(GRID)
    c.setLineWidth(0.5)
    c.line(20 * mm, 15.5 * mm, 190 * mm, 15.5 * mm)
    c.restoreState()


def on_first(c, doc):
    c.saveState()
    c.setFillColor(BLUE)
    c.rect(0, A4[1] - 9 * mm, A4[0], 9 * mm, stroke=0, fill=1)
    c.restoreState()
    on_page(c, doc)


# ---- content --------------------------------------------------------------------------------
def build():
    st = []
    runs = run_results()
    rmap = {r["name"]: r for r in runs}

    # Title
    st += [Spacer(1, 14 * mm), P("Lake Weather Station — LoRa Hop Network", "title"),
           P("Project report: design, hardware findings, hop protocol, measurements and robustness testing", "subtitle"),
           Spacer(1, 4), P("15 October 2026 · repository <font name='Mono'>~/Documents/weather_lora_link</font>", "small"),
           Spacer(1, 10)]
    st.append(P(
        "<b>In one paragraph.</b> A weather station on a boat on a lake measures temperature, humidity, pressure, wind, "
        "rain, soil moisture and lightning. There is no internet at the lake, so its readings travel over LoRa radio, "
        "hop by hop, from the boat through a relay to a gateway at the lake edge that has a cellular IoT SIM, and from "
        "there to AWS and a website. The hop protocol is built and was tested on the three real LoRa-E5 mini boards: "
        "readings arrive exactly once and in order, including when a relay or the gateway is switched off, when "
        "acknowledgements are deliberately dropped, and when radio errors are injected. The radio range around the lake, "
        "the relay firmware on the boards' own chip, and the gateway's cellular link are the main open items.", "callout"))
    st.append(P("Contents", "h2"))
    for i, t in enumerate(["Goal and environment", "Key decisions", "Hardware", "What was found on the existing station",
                           "Radio configuration and regulations", "Data formats", "The hop network",
                           "Site and terrain", "Measurements: range test", "Robustness testing",
                           "Open issues and risks", "Next steps", "Appendix: files and commands"], 1):
        st.append(P(f"{i}. {t}", "small"))
    st.append(PageBreak())

    # 1 Goal
    st.append(P("1. Goal and environment", "h1"))
    st.append(P("<b>End product:</b> readings from the meteorological sensor reach AWS and are displayed on a website. "
                "Later, time-series forecasting runs on the stored history (Section 12)."))
    st += bullets([
        "The weather station is mounted on a boat released onto a lake. The boat moves.",
        "There is no internet at the lake. The gateway will use an <b>IoT SIM card</b> (cellular) for its uplink.",
        "Country: <b>Turkey</b>. LoRa uses the EU 863–870 MHz band, which Turkey normally follows as a CEPT member "
        "(the BTK text itself was not found online; treat as an assumption to confirm).",
        "Office (first gateway idea) at 41.10591, 29.02011; lake at 41.10257, 29.03065; straight-line distance 958 m. "
        "The plan later moved to a network <b>around the lake itself</b> (Section 8).",
    ])
    st.append(Spacer(1, 4))
    st.append(fig_architecture())
    st.append(P("Figure 1. Target architecture. Relays are added only where the terrain blocks a direct link.", "cap"))

    # 2 Decisions
    st.append(P("2. Key decisions", "h1"))
    st.append(table([
        ["Decision", "Chosen", "Why"],
        ["Mesh firmware (Meshtastic)", "Not used", "Meshtastic on ESP32 needs an SPI radio; the Wio-E5 is a UART/AT module. "
         "Its Wio-E5 port is unfinished and erases the factory firmware."],
        ["Radio mode", "LoRa point-to-point (factory AT firmware, TEST mode)", "No reflashing needed for prototypes; full control of "
         "packets and power."],
        ["Topology", "Fixed chain: station → relay(s) → gateway", "Lake sits in a hollow (Section 8); a fixed path is simpler "
         "and more airtime-efficient than flooding."],
        ["Reliability", "Hop-by-hop acknowledgement + store-and-forward", "A broken hop delays readings instead of losing them."],
        ["Security", "Station signs each reading; each hop signs its frame", "Gateway rejects forged or altered data."],
        ["Relay logic (now)", "Prototype on a computer driving the AT firmware", "Proves the protocol on real radios before writing "
         "chip firmware."],
        ["Relay logic (later)", "Native firmware on the STM32WLE5 chip", "Fewer parts, lowest power. No ST-Link is available; "
         "the USB bootloader route must be verified first."],
        ["Relay power", "2 × 18650 in parallel, scheduled wake-up", "About a year per charge if the relay sleeps between "
         "planned slots; weeks if it listens all the time."],
    ], [38 * mm, 52 * mm, 80 * mm]))

    # 3 Hardware
    st.append(PageBreak())
    st.append(P("3. Hardware", "h1"))
    st.append(P("Radio boards", "h2"))
    st.append(P("All three radios are Seeed <b>LoRa-E5 mini</b> development boards carrying a <b>Wio-E5-LE-HF</b> module "
                "(STM32WLE5JC with built-in sub-GHz radio, maximum 14 dBm). Each has its own USB-C serial port (CP2102N), "
                "factory AT firmware 4.0.11 and an EU868 build. Boards are identified by DevEui, not by port name."))
    st.append(table([
        ["#", "DevEui", "Found as", "Desk-test role"],
        ["1", "2C:F7:F1:20:73:40:11:63", "factory, EU868", "gateway (node 3)"],
        ["2", "2C:F7:F1:20:73:40:02:C8", "LoRaWAN ABP, 14 dBm", "station (node 1)"],
        ["3", "2C:F7:F1:20:75:30:AA:8A", "LoRaWAN ABP, 14 dBm, class A", "relay (node 2)"],
    ], [8 * mm, 50 * mm, 60 * mm, 52 * mm]))
    st.append(P("Boards power up in LoRaWAN mode; the node software switches them to TEST mode and re-applies the radio "
                "settings every time, because those settings are not kept across power loss.", "small"))
    st.append(P("The weather station", "h2"))
    st.append(P("The station is a <b>SparkFun MicroMod Weather Carrier Board</b> with a <b>MicroMod ESP32</b> processor and a "
                "SIM800L 2G modem. There is no Raspberry Pi in the station. Pins below are taken from the original builder's "
                "source code, recovered from an SD card (Section 4)."))
    st.append(table([
        ["Sensor", "Measures", "Connection", "Notes"],
        ["BME280", "temperature, humidity, pressure", "I2C (GPIO 21/22), address 0x77", "read every report"],
        ["Weather Meter Kit", "wind speed, direction, rain", "GPIO 14 (pulses), 35 (analog vane), 27 (rain)", "vane read −1 "
         "during inspection: likely disconnected"],
        ["Soil moisture probe", "soil moisture", "GPIO 34 (analog)", "builder's calibration: raw 0 dry, 648 wet"],
        ["AS3935", "lightning distance and count", "SPI, chip-select GPIO 12", "polled (interrupt pin shared with UART1)"],
        ["SIM800L", "2G cellular upload", "UART1 (GPIO 16/17)", "'modem not responding' during inspection"],
    ], [30 * mm, 38 * mm, 52 * mm, 50 * mm]))
    st.append(P("Antennas, cables and power", "h2"))
    st += bullets([
        "Use the 868–915 MHz rubber-duck antennas that came with the boards. A 433 MHz antenna is the wrong band and was rejected.",
        "Extension: <b>SMA male (90°) to SMA female, RG58</b>, 3 m for the boat and up to 5 m for tree mounts "
        "(≈1.8 dB and ≈2.8 dB loss at 868 MHz). Avoid RP-SMA and thin RG174.",
        "Relay power: a dual-18650 shield (cells in parallel, 5 V USB output, HOLD/NORMAL switch). Its idle current could not "
        "be measured yet (Section 11). Set it to HOLD so it does not switch off at low load.",
    ])

    # 4 Findings
    st.append(P("4. What was found on the existing station", "h1"))
    st.append(P("Before anything was changed, the ESP32's complete 16 MB flash was copied (read-only) and the station's "
                "microSD card was imaged. Both copies are kept in <font name='Mono'>~/Documents/original_firmware_backups/</font>."))
    st += bullets([
        "<b>Running firmware:</b> \"Hava Istasyonu (SIM800L)\", built 20 July 2026 with Arduino-ESP32 3.3.11. It reads the "
        "sensors, prints a <font name='Mono'>TELEM {json}</font> line and uploads to Weather Underground over GPRS. "
        "The Weather Underground password is stored in plain text in the firmware.",
        "<b>The SD card</b> in the weather board's slot is not used by the ESP32; it is a Raspberry Pi system card. It holds "
        "the builder's ESP32 source code and the scripts of an <b>earlier pipeline</b> (Figure 2) that wrote about 317,000 "
        "readings into a PostgreSQL database on an AWS EC2 server until 7 August 2026.",
        "That earlier Pi also stored the <b>private key of the AWS server</b> in plain text. It was not copied. Anyone "
        "holding that card can log in to the server, so the key should be rotated.",
        "The source on the card has no SIM800L code, so it is not identical to the firmware now on the ESP32.",
    ])
    st.append(fig_previous_pipeline())
    st.append(P("Figure 2. The earlier pipeline, reconstructed from the SD card. The AWS database still exists and can be reused.", "cap"))

    # 5 Radio
    st.append(PageBreak())
    st.append(P("5. Radio configuration and regulations", "h1"))
    st.append(P("Both ends of every link must use identical settings. They are applied with one AT command:"))
    st.append(P("AT+TEST=RFCFG,868.1,SF9,125,12,15,14,ON,OFF,OFF", "mono"))
    st.append(table([
        ["Field", "Value", "Meaning"],
        ["Frequency", "868.1 MHz", "EU868 sub-band 868.0–868.6 MHz: 25 mW (14 dBm), 1 % airtime"],
        ["Spreading factor", "SF9", "range/speed trade-off; SF12 gives ≈7.5 dB more margin (≈2× range) at ≈5× airtime"],
        ["Bandwidth", "125 kHz", "standard LoRa channel"],
        ["Preamble TX / RX", "12 / 15", "receiver expects at least the transmitted length"],
        ["Power", "14 dBm", "legal with an antenna of ≤ 2.15 dBi; use 13 dBm with 3 dBi, 11 dBm with 5 dBi"],
        ["CRC / IQ / network", "on / off / off", "error check on; private sync word, so public LoRaWAN gateways ignore it"],
    ], [32 * mm, 30 * mm, 108 * mm]))
    st.append(Spacer(1, 6))
    st.append(P("Choosing the spreading factor", "h2"))
    st.append(table([
        ["SF", "Reading on air", "Relay airtime per hour (5-min interval)", "Range vs SF9"],
        ["SF7", "0.10 s", "1.7 s", "≈ 0.6×"], ["SF8", "0.17 s", "3.2 s", "≈ 0.8×"],
        ["<b>SF9</b>", "<b>0.30 s</b>", "<b>5.6 s</b> — room for retries", "1×"],
        ["SF10", "0.57 s", "10.7 s", "≈ 1.4×"], ["SF11", "1.22 s", "22.3 s — little room for retries", "≈ 1.9×"],
        ["SF12", "2.27 s", "<font color='#b42318'><b>42.7 s — over the 32.4 s budget at 1 %</b></font>", "≈ 2.6×"],
    ], [18 * mm, 30 * mm, 82 * mm, 40 * mm]))
    st.append(P("A relay both acknowledges and forwards every reading, so its airtime is the binding limit. Keep SF9 if every "
                "hop shows SNR of −2.5 dB or better (≈10 dB margin for a rocking boat); otherwise step to SF10, then SF11, "
                "and add a relay before SF12. All nodes must share one SF. Moving the network to 869.525 MHz (10 % airtime) "
                "would make even SF11–12 legal with retries; LoRaWAN gateways use that frequency for downlinks, and the Turkish "
                "rules for that sub-band need confirming.", "small"))
    st.append(Spacer(1, 6))
    st.append(P("<b>Airtime.</b> A hop frame (44 bytes) takes 0.30 s at SF9; an acknowledgement 0.16 s. The 1 % rule "
                "allows 36 s per hour per device. The software keeps a sliding one-hour budget at 90 % of the legal limit "
                "and saves it to disk, so a restart cannot reset it. Bench tests ran on 869.525 MHz (sub-band "
                "869.4–869.65 MHz, 10 % airtime) so that fast test rates stayed legal."))

    # 6 Formats
    st.append(P("6. Data formats", "h1"))
    st.append(P("Two layers: the <b>reading</b> is signed once by the station and opened only by the gateway; around it, each "
                "hop adds a small <b>link header</b> that it signs with its own MAC. Signatures are the first 4 bytes of "
                "HMAC-SHA256 with a shared 16-byte key (authentication, not encryption)."))
    st.append(fig_frames())
    st.append(P("Figure 3. Frame layouts (bytes under each field; 1–2 byte fields drawn wider than scale). Values are scaled integers; fields a station cannot "
                "measure carry a 'not measured' marker and are left out of the JSON.", "cap"))
    st.append(P("Rain and lightning are running totals since the station powered on, so a lost packet loses no rainfall; "
                "the database takes differences. Boot number + sequence number identify every reading uniquely.", "small"))

    # 7 Hop network
    st.append(PageBreak())
    st.append(P("7. The hop network", "h1"))
    st.append(P("Each node knows the id of its next hop. A node forwards only frames addressed to it, so nodes that happen "
                "to hear each other directly do not create duplicates or loops."))
    st.append(fig_sequence())
    st.append(P("Figure 4. One reading crossing two hops, including a lost acknowledgement. The relay retries with the same "
                "link sequence number; the gateway recognises the repeat, acknowledges it again, and does not publish it twice.", "cap"))
    st.append(P("Rules that make it robust", "h2"))
    st.append(table([
        ["Mechanism", "What it does", "Shown in test"],
        ["Hop acknowledgement (HACK)", "each hop confirms receipt within 2.5 s", "all"],
        ["Custody before acknowledgement", "a relay stores the reading durably before it acknowledges; a crash can only "
         "cause a harmless repeat", "H5, H8"],
        ["Store-and-forward queue", "3 quick retries, then waits of 5, 10, 20, 40, 60 s; nothing is dropped below 500 queued", "H2, H3, H4b"],
        ["Duplicate memory", "(sender, link sequence, payload digest) per hop; (station, boot, sequence) end to end; "
         "both saved to disk", "H3, H4, H5"],
        ["Duty-cycle limiter", "per EU868 sub-band (0.1 / 1 / 10 %); holds data and refuses new packets (no ack) once the hourly budget is used; log survives restarts", "H7b, unit tests"],
        ["Gateway outbox", "readings wait in a saved outbox until the MQTT broker confirms them (QoS 1)", "R4b"],
        ["Automatic recovery", "radio errors or a lost serial port trigger reconfiguration instead of exiting", "H5"],
        ["Hop limit", "frames that have travelled 8 hops are dropped", "unit test"],
    ], [42 * mm, 92 * mm, 36 * mm]))

    # 8 Site
    st.append(P("8. Site and terrain", "h1"))
    pts = [(x, y) for x, y in TERRAIN]
    sight = [(0, 118 + 6), (958, 76 + 2)]
    d, X, Y, lp = lineplot([pts, sight], (0, 958), (70, 130), W, 160, "distance from office towards lake (m)",
                           "elevation (m)", [INK2, BLUE], xticks=[0, 200, 400, 600, 800, 958], yticks=[70, 80, 90, 100, 110, 120, 130],
                           annotations=[(719, 112, "ridge ~109 m", "end"), (958, 73, "lake 76 m", "end"), (0, 124, " office 118 m", "start")])
    lp.lines[1].strokeDashArray = [3, 2]
    lp.lines[1].strokeWidth = 1.2
    st.append(d)
    st.append(P("Figure 5. Terrain between the office and the lake (Open-Meteo elevation, ~30–90 m grid, bare ground only). "
                "The dashed line is the direct radio path (antenna 6 m above the office, 2 m above the boat): it runs about "
                "20 m below the ridge. The lake sits in a hollow.", "cap"))
    st += bullets([
        "Best relay candidate from the terrain model: <b>41.10509, 29.02815</b> (ground ≈117 m; 350 m to the lake, 681 m to "
        "the office). Runner-up: 41.10257, 29.02898 on the west rim.",
        "The model ignores buildings and trees and the clearance is only 1–7 m, so these are spots to test, not guarantees.",
        "Since then the plan moved to a network <b>around the lake</b> with the gateway at the lake edge on cellular. "
        "Relay positions must be re-checked for that layout.",
    ])

    # 9 Range test
    st.append(PageBreak())
    st.append(P("9. Measurements: range test", "h1"))
    st.append(P("14 October 2026. Receiver: board on the laptop, <b>indoors at desk height</b>. Transmitter: board on a phone "
                "(USB-OTG, Serial USB Terminal) carried outside, one packet every 30 s, SF9, 14 dBm, rubber-duck antennas."))
    walk = range_walk()
    ann = [(5.5, -12, "left the building", "start"), (6.0, -76, "142 steps ≈105 m", "start"), (7.0, -66, "200 ≈150 m", "start"),
           (8.5, -86, "250 ≈190 m", "start"), (10.5, -96, "276 ≈210 m (edge)", "start")]
    d, X, Y, lp = lineplot([[(t, r) for t, r, _ in walk]], (0, 15.1), (-130, 0), W, 150, "minutes since start of walk",
                           "RSSI (dBm)", [BLUE], xticks=list(range(0, 16, 2)), yticks=[-130, -110, -90, -70, -50, -30, -10],
                           refline=(-129, "SF9 sensitivity ≈ −129 dBm"), annotations=ann)
    lp.lines[0].symbol = None
    st.append(d)
    st.append(P("Figure 6. Received signal strength during the walk (out and back). One packet was lost, at 10:14:16.", "cap"))
    d, X, Y, lp = lineplot([[(t, s) for t, _, s in walk]], (0, 15.1), (-15, 15), W, 120, "minutes since start of walk",
                           "SNR (dB)", [AQUA], xticks=list(range(0, 16, 2)), yticks=[-15, -10, -5, 0, 5, 10, 15],
                           refline=(-12.5, "SF9 decoding limit ≈ −12.5 dB"))
    st.append(d)
    st.append(P("Figure 7. Signal-to-noise ratio for the same packets.", "cap"))
    st += bullets([
        "<b>29 of 30</b> packets received; the edge was about <b>210–220 m</b>, set mostly by walls around the indoor receiver.",
        "Outdoors with raised antennas and a clear view, several times this distance is expected; SF12 adds ≈7.5 dB.",
        "This test does not yet measure the real lake links. Field tests at the actual node positions are still required.",
    ])

    # 10 Robustness
    st.append(PageBreak())
    st.append(P("10. Robustness testing", "h1"))
    st.append(P("Method", "h2"))
    st.append(P("The code was first reviewed line by line; every problem found is listed below with its status. Then three "
                "real boards ran as station → relay → gateway on a desk, through scripted failure scenarios. An independent "
                "checker reads all three logs and verifies that every reading the station made arrived <b>exactly once, in "
                "order, with nothing invented</b>. A run that is correct but still had readings in a queue at the end is "
                "reported separately as PASS-UNDRAINED, never as PASS. Desk tests prove logic and recovery; they do not "
                "prove range (signals were around −5 dBm)."))
    st.append(P("Code review findings", "h2"))
    st.append(table([
        ["Finding", "Severity", "Status"],
        ["Serial driver discarded buffered input before each command: packets arriving at that moment were silently lost", "High", "Fixed"],
        ["Any radio or serial error ended the node program (a relay would stay dead)", "High", "Fixed: automatic recovery"],
        ["No airtime limit: retries and catch-up bursts could exceed the legal 1 %", "High", "Fixed: limiter, saved to disk"],
        ["Relay acknowledged before storing: a crash between the two lost the reading", "High", "Fixed: custody before ack, durable queue"],
        ["Duplicate check used only (sender, sequence): a restart could rarely drop a new reading", "Medium", "Fixed: + payload digest"],
        ["Gateway forgot duplicates on restart", "Medium", "Fixed: saved to disk"],
        ["Fixed 30 s retry wait made backlogs grow on bad links (found by test H4)", "Medium", "Fixed: 5→60 s backoff, rerun H4b"],
        ["Airtime log lost on restart (found by test H7)", "Medium", "Fixed"],
        ["Gateway acknowledges a hop even if the reading's signature is wrong (misconfigured key = silent loss)", "Medium", "Open: logged as REJECTED"],
        ["All tests use the placeholder signing key from secrets.example.h", "Critical before deployment", "Open"],
        ["ESP32 station and gateway firmware do not speak the hop protocol yet", "Gap", "Open"],
        ["Station queue lives in memory: readings queued at a power cut are lost", "Medium", "Open: plan SD card"],
        ["<b>Second review (/code-review):</b> 0.1 % sub-band 868.7–869.2 MHz got a 1 % budget (10× over the limit)", "High", "Fixed: full sub-band table; out-of-band refused"],
        ["Full queue could drop the packet in flight, then remove the wrong one after its ack", "High", "Fixed: never drops in-flight item"],
        ["Acknowledgements bypassed the airtime limit", "High", "Fixed: no budget → no custody, no ack"],
        ["Replay of a frame from an old station boot accepted after 10 min", "Medium", "Fixed: last seq kept per boot (16 boots)"],
        ["Gateway → MQTT fire-and-forget; broker outage lost acknowledged readings; crash if broker down at start", "High", "Fixed: durable outbox, QoS 1, async connect"],
        ["Odd-length hex line from serial noise crashed the node", "Medium", "Fixed: line dropped; catch-all recovery"],
        ["Checker matched missing readings by count, not identity", "Medium", "Fixed: nodes log held readings by id"],
        ["Airtime log not saved at shutdown / on every send", "Medium", "Fixed"],
        ["Test switch --duty-budget 0 meant 'full budget'", "Low", "Fixed"],
    ], [98 * mm, 28 * mm, 44 * mm]))
    st.append(P("Scenario results on the real boards", "h2"))
    desc = {"H1_baseline": "Baseline, a reading every 8 s for 4 min",
            "H2_relay_outage": "Relay off for 90 s mid-run, then restarted",
            "H3_gateway_outage": "Gateway off for 90 s, then restarted",
            "H4_faults": "30 % of acks dropped + 10 % of packets ignored on relay and gateway (old 30 s retry)",
            "H4b_faults_backoff": "Same faults with the new 5→60 s backoff",
            "H5_radio_errors": "Simulated radio failure on 10 % of transmissions, all nodes",
            "H6_station_reboot": "Station power loss mid-run (new boot number)",
            "H7_duty_limit_868": "One reading per second on 868.1 MHz (limiter check, capacity)",
            "H7b_duty_limiter": "Airtime budget shrunk to 8 s/h to force the limiter",
            "H8_relay_crash_holding": "Gateway off, then relay crashes while holding readings, both restarted",
            "R1_baseline": "After 2nd review: baseline",
            "R2_faults_backoff": "After 2nd review: heavy faults (as H4b)",
            "R3_relay_crash_holding": "After 2nd review: relay crash while holding (as H8)",
            "R4_mqtt_outage": "MQTT broker off 60 s behind the gateway (harness log bug, see text)",
            "R4b_mqtt_outage": "MQTT broker off 60 s, rerun with fixed harness"}
    rows = [["Scenario", "Made", "Delivered", "Dup.", "Latency s (med/max)", "Result"]]
    for r in runs:
        res = r["result"]
        color = {"PASS": "#1a7f37", "FAIL": "#b42318"}.get(res, "#9a5b00")
        lat = f"{r['lat'][1]} / {r['lat'][2]}" if r["lat"] else "–"
        rows.append([f"<b>{r['name'].split('_')[0]}</b> {desc.get(r['name'], '')}", str(r["made"]), str(r["got"]), str(r["dup"]),
                     lat, f"<font color='{color}'><b>{res}</b></font>"])
    st.append(table(rows, [74 * mm, 12 * mm, 19 * mm, 10 * mm, 23 * mm, 32 * mm]))
    st.append(P("Made = readings created by the station; Delivered = unique readings published by the gateway; Dup. = "
                "readings published twice (must be 0). PASS-UNDRAINED = no loss and no duplicates, but some readings were "
                "still queued when the run ended.", "small"))
    if runs:
        st.append(Spacer(1, 6))
        st.append(latency_chart(runs))
        st.append(P("Figure 8. Median and worst latency per scenario (seconds). Outages and fault injection raise latency; "
                    "readings are delayed, not lost.", "cap"))
    st.append(P("What the scenarios showed", "h2"))
    st += bullets(scenario_notes(rmap))
    st.append(P("Limits of these tests", "h2"))
    st.append(P("All three boards sat <b>next to each other on one desk, connected to one laptop</b>. The results therefore "
                "show that the protocol logic is correct; they say nothing yet about the radio links at the lake.", "callout"))
    st += bullets([
        "<b>No real radio loss:</b> signals were about −5 to −20 dBm. Every loss in the scenarios was injected in software, "
        "not caused by distance, terrain, trees or water.",
        "<b>No hidden nodes:</b> on the desk every board hears every other. At the lake the station and the gateway will "
        "probably not hear each other, which changes how collisions happen.",
        "<b>Not standalone devices:</b> each node was a laptop program driving a board over USB. Final relay and station "
        "firmware will have different timing.",
        "<b>No battery effects:</b> USB power only; sleep, low voltage and brown-outs were not tested.",
        "<b>Short runs:</b> minutes, not days; long-term stability, clock drift and counter wrap-around are untested.",
        "<b>Not the deployment frequency:</b> most runs used 869.525 MHz (10 % airtime band) so that fast test rates stayed legal; "
        "the deployment plan uses 868.1 MHz.",
    ])

    # 11 Open issues
    st.append(PageBreak())
    st.append(P("11. Open issues and risks", "h1"))
    st.append(table([
        ["Item", "Risk", "Plan"],
        ["Real lake links untested", "Hops may be blocked by terrain, trees or the boat's low antenna", "Field-test each hop with "
         "the laptop and phone method; try SF12; place relays high"],
        ["Relay firmware on the STM32 chip", "No ST-Link; the bootloader route via USB is unverified", "Check the LoRa-E5 mini's "
         "BOOT/USB bootloader; otherwise relay = ESP32 + board"],
        ["Gateway hardware for the IoT SIM", "Not chosen; SIM800L is 2G-only", "Pick a 4G/LTE-M modem + controller; confirm SIM "
         "network support and coverage at the lake edge"],
        ["Signing key", "Placeholder key in all tests", "Generate a real key into include/secrets.h before deployment"],
        ["Leaked credentials", "AWS server key and Weather Underground password in plain text on old media", "Rotate both"],
        ["Station queue persistence", "Readings lost on power loss", "Store the queue on the weather board's SD card"],
        ["Capacity", "Chain sustains ≈1 reading every 2 s (half-duplex)", "Ample for 5-min reporting; consider batching for "
         "faster catch-up"],
        ["Battery life", "Shield idle current unmeasured; boost converter may drain cells in weeks", "Measure; use a low-idle "
         "3.3 V regulator if needed"],
        ["Wind on a moving boat", "Vane direction is relative to the bow; rain gauge false tips from waves", "Add compass/GPS; "
         "filter rain tips"],
        ["USB unplug recovery", "Code path exists, not exercised on hardware", "Unplug/replug test"],
        ["Gateway → MQTT duplicates", "QoS 1 is at-least-once: a lost broker confirmation can repeat a reading", "Insert "
         "into the database keyed on (station, boot, seq) so repeats are ignored"],
        ["Desk-only testing", "No real propagation, hidden nodes, batteries or long runs (Section 10)", "Field tests and a "
         "multi-day soak test"],
    ], [38 * mm, 64 * mm, 68 * mm]))

    # 12 Next
    st.append(P("12. Next steps", "h1"))
    st += bullets([
        "<b>Field test around the lake:</b> station position → relay position → gateway position, one hop at a time, logging "
        "RSSI/SNR; decide the number of relays.",
        "<b>Gateway:</b> choose the cellular hardware for the IoT SIM; forward delivered readings into the existing AWS "
        "PostgreSQL database (same table as the earlier pipeline).",
        "<b>Station firmware:</b> add the hop protocol to the ESP32 code, read the real sensors (pins confirmed from the "
        "builder's source), queue on SD card, keep the original firmware backup for rollback.",
        "<b>Relay firmware:</b> port the relay to the STM32WLE5 with scheduled wake-up, or use an ESP32 per relay.",
        "<b>Website and forecasting (end goal):</b> charts from the database; time-series forecasts trained on the stored "
        "history (gap-free, regularly spaced data matters, which is why store-and-forward and station timestamps were designed in).",
    ])

    # Appendix
    st.append(P("Appendix: files and commands", "h1"))
    st.append(table([
        ["Path", "Contents"],
        ["tools/e5_node.py", "station / relay / gateway node (hop protocol, queue, limiter, recovery)"],
        ["tools/hopnet.py, tools/wxframe.py", "link frames; end-to-end reading format"],
        ["tools/chain_run.py, tools/chain_check.py", "scenario orchestrator; end-to-end checker"],
        ["tools/test_hopnet.py, tools/run_tests.sh", "unit tests (Python and C++)"],
        ["tools/e5_bench.py, tools/fake_air.py", "single-board bench tool; radio simulator"],
        ["src/, lib/wxlink/", "ESP32 station (fake and MicroMod sensors) and gateway firmware"],
        ["range_tests/", "range test, terrain check, all chain-run logs and results"],
        ["DEVICES.md", "board inventory"],
        ["~/Documents/original_firmware_backups/", "ESP32 flash backup (16 MB), SD card image, notes"],
    ], [62 * mm, 108 * mm]))
    st.append(Spacer(1, 6))
    st.append(P("python3 tools/e5_node.py -p /dev/cu.usbserial-110 --role gateway --id 3<br/>"
                "python3 tools/e5_node.py -p /dev/cu.usbserial-120 --role relay --id 2 --next 3<br/>"
                "python3 tools/e5_node.py -p /dev/cu.usbserial-130 --role station --id 1 --next 2 --interval 300", "mono"))

    doc = SimpleDocTemplate(str(OUT), pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=18 * mm,
                            bottomMargin=20 * mm, title="Lake Weather Station — LoRa Hop Network",
                            author="weather_lora_link project", subject="Project report")
    doc.build(st, onFirstPage=on_first, onLaterPages=on_page)
    print("wrote", OUT)


def latency_chart(runs):
    names = [r["name"].split("_")[0] for r in runs if r["lat"]]
    med = [r["lat"][1] for r in runs if r["lat"]]
    mx = [r["lat"][2] for r in runs if r["lat"]]
    d = Drawing(W, 150)
    bc = VerticalBarChart()
    bc.x, bc.y, bc.width, bc.height = 42, 30, W - 60, 100
    bc.data = [med, mx]
    bc.categoryAxis.categoryNames = names
    bc.valueAxis.valueMin = 0
    top = max(mx + [10])
    bc.valueAxis.valueMax = (int(top / 50) + 1) * 50
    bc.valueAxis.valueStep = 50
    bc.groupSpacing, bc.barSpacing = 8, 2
    bc.bars[0].fillColor, bc.bars[1].fillColor = BLUE, ORANGE
    bc.bars.strokeColor = None
    for ax in (bc.categoryAxis, bc.valueAxis):
        ax.labels.fontName, ax.labels.fontSize, ax.labels.fillColor = "Body", 7, INK2
        ax.strokeColor, ax.strokeWidth = MUTED, 0.6
    bc.valueAxis.visibleGrid = True
    bc.valueAxis.gridStrokeColor, bc.valueAxis.gridStrokeWidth = GRID, 0.5
    d.add(bc)
    for i, (c, label) in enumerate([(BLUE, "median"), (ORANGE, "worst")]):
        d.add(Rect(W - 120 + i * 60, 140, 8, 8, fillColor=c, strokeColor=None))
        d.add(String(W - 108 + i * 60, 141, label, fontName="Body", fontSize=7.2, fillColor=INK2))
    from reportlab.graphics.shapes import Group
    lab = Group(String(0, 0, "latency (s)", fontName="Body", fontSize=7.4, fillColor=INK2, textAnchor="middle"))
    lab.transform = (0, 1, -1, 0, 10, 80)
    d.add(lab)
    return d


def scenario_notes(r):
    notes = []
    if "H1_baseline" in r:
        notes.append("<b>H1:</b> every reading delivered through 2 hops with no retries; latency 1–2 s end to end.")
    if "H2_relay_outage" in r:
        notes.append("<b>H2/H3:</b> with the relay or the gateway off for 90 s, the upstream node held up to 15 readings and "
                     "delivered all of them, in order, after the restart. The restarted gateway recognised a repeat from its "
                     "saved duplicate memory.")
    if "H4_faults" in r:
        notes.append("<b>H4 → H4b:</b> with heavy injected faults, correctness held in both (no duplicate, no loss). With the "
                     "old 30 s retry wait, 8 of 38 readings were still queued at the end and the median latency was 181 s. "
                     "With the new 5→60 s backoff (H4b) all 38 arrived, median 46 s, worst 85 s; 35 repeats caused by the "
                     "dropped acknowledgements were all recognised. Cost: about two transmissions per reading under these faults.")
    if "H5_radio_errors" in r:
        notes.append("<b>H5:</b> 13 simulated radio failures were each recovered in about 1 s. A failure just before an "
                     "acknowledgement produced a retry that the receiver correctly recognised as a repeat.")
    if "H6_station_reboot" in r:
        notes.append("<b>H6:</b> readings from two station boots arrived cleanly. The station's queue was empty at the moment "
                     "of the power cut, so loss of queued readings on a real power cut is not covered (open issue).")
    if "H7_duty_limit_868" in r:
        notes.append("<b>H7:</b> at one reading per second, half of the station's sends failed because the relay was busy "
                     "forwarding (radios cannot send and receive at once): the chain sustains roughly one reading every 2 s. "
                     "The limiter did not engage within the 3-minute run; H7b forces it.")
    if "H7b_duty_limiter" in r:
        notes.append("<b>H7b:</b> with the hourly budget shrunk to 8 s, the station stopped transmitting at 7.9 s of airtime "
                     "and kept the remaining 76 readings queued (none lost); they would go out as the one-hour window frees up. "
                     "(At that time acknowledgements were still sent over budget; the second review changed this, so a node "
                     "without airtime now refuses new packets and the sender keeps them.)")
    if "H8_relay_crash_holding" in r:
        notes.append("<b>H8:</b> the gateway was switched off, then the relay was killed while holding 8 readings for it. The "
                     "restarted relay reloaded those 8 from disk and, once the gateway returned, all 28 readings arrived exactly "
                     "once and in order (worst latency 123 s).")
    if "R4b_mqtt_outage" in r:
        notes.append("<b>R1–R4b (after the second review):</b> baseline, heavy faults and relay crash all passed again with the "
                     "reworked code. With the MQTT broker stopped for 60 s behind the gateway, all 30 readings reached the broker "
                     "once the broker returned: 0 missing, 0 duplicated (R4b). The first attempt (R4) showed 8 readings missing "
                     "from the subscriber log; the cause was the test harness deleting the subscriber's log file at start, not "
                     "the gateway, and the rerun confirmed it.")
        notes.append("<b>Variance:</b> R2 repeated H4b's fault settings but had higher latency (median 68 s vs 46 s, worst "
                     "147 s vs 85 s). Injected faults depend on timing, so runs differ; both delivered everything exactly once.")
    notes.append("<b>Checker bug found during testing:</b> the first version of the checker double-counted a restarted "
                 "relay's queue and labelled H8 'undrained'. It was fixed and every run was re-checked from its logs.")
    return notes


if __name__ == "__main__":
    build()
