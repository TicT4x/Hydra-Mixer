# Builds resources/headphones.txt: the built-in headphone database of Headphone Correction.
#
# Source: the AutoEq project (https://github.com/jaakkopasanen/AutoEq, MIT license), measurements by
# oratory1990, Rtings, Kuulokenurkka, Innerfidelity and others. For every headphone we store its deviation from
# the Harman target *of the rig it was measured on* (AutoEq's "error" = raw - target). That makes headphones from
# different rigs comparable: "A sounds like B" = EQ (deviation B - deviation A).
#
# Also stores the rig targets (to turn a raw measurement the user imports into a deviation) and a few sound targets
# relative to Harman (diffuse field, Harman without bass, ...).
#
# Run:  python -I tools/fetch-headphones.py   (needs network; writes resources/headphones.txt)
import csv, io, math, sys, urllib.parse, urllib.request
from pathlib import Path

RAW = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/"
OUT = Path(__file__).resolve().parent.parent / "resources" / "headphones.txt"

# (shown name, kind, AutoEq name, [preferred source dirs in results/])
OVER = "over-ear"
IN = "in-ear"
HEADPHONES = [
    # --- the user's own ---
    ("Beyerdynamic DT 770 Pro 80 Ohm", OVER, "Beyerdynamic DT 770 Pro (80 Ohm)", ["Kuulokenurkka/over-ear"]),
    ("SteelSeries Arctis 7 Wireless (2017)", OVER, "SteelSeries Arctis 7 Wireless 2017", ["Rtings/HMS II.3 over-ear"]),
    ("SteelSeries Arctis 7 (2019)", OVER, "SteelSeries Arctis 7 2019 Edition", ["Rtings/HMS II.3 over-ear"]),
    # --- studio / hifi ---
    ("Beyerdynamic DT 770 Pro 250 Ohm", OVER, "Beyerdynamic DT 770 Pro", ["oratory1990/over-ear"]),
    ("Beyerdynamic DT 990 Pro", OVER, "Beyerdynamic DT 990 Pro", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Beyerdynamic DT 880", OVER, "Beyerdynamic DT 880", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Beyerdynamic DT 1990 Pro", OVER, "Beyerdynamic DT 1990 (balanced earpads)", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Beyerdynamic DT 900 Pro X", OVER, "Beyerdynamic DT 900 Pro X", ["oratory1990/over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Beyerdynamic DT 700 Pro X", OVER, "Beyerdynamic DT 700 Pro X", ["oratory1990/over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Beyerdynamic MMX 300", OVER, "Beyerdynamic MMX 300", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sennheiser HD 600", OVER, "Sennheiser HD 600", ["oratory1990/over-ear"]),
    ("Sennheiser HD 650", OVER, "Sennheiser HD 650", ["oratory1990/over-ear"]),
    ("Sennheiser HD 6XX", OVER, "Sennheiser HD 6XX", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sennheiser HD 660 S", OVER, "Sennheiser HD 660 S", ["oratory1990/over-ear"]),
    ("Sennheiser HD 560S", OVER, "Sennheiser HD 560S", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sennheiser HD 800 S", OVER, "Sennheiser HD 800 S", ["oratory1990/over-ear"]),
    ("Sennheiser HD 599", OVER, "Sennheiser HD 599", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sennheiser HD 280 Pro", OVER, "Sennheiser HD 280 Pro", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sennheiser HD 25", OVER, "Sennheiser HD 25", ["Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Sennheiser Momentum 4 Wireless", OVER, "Sennheiser Momentum 4 Wireless", ["oratory1990/over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("AKG K371", OVER, "AKG K371", ["oratory1990/over-ear"]),
    ("AKG K702", OVER, "AKG K702", ["oratory1990/over-ear"]),
    ("AKG K712 Pro", OVER, "AKG K712", ["oratory1990/over-ear"]),
    ("AKG K240 Studio", OVER, "AKG K240 Studio", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("AKG K361", OVER, "AKG K361", ["oratory1990/over-ear", "Kuulokenurkka/over-ear"]),
    ("Audio-Technica ATH-M50x", OVER, "Audio-Technica ATH-M50x", ["oratory1990/over-ear"]),
    ("Audio-Technica ATH-M40x", OVER, "Audio-Technica ATH-M40x", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Audio-Technica ATH-M20x", OVER, "Audio-Technica ATH-M20x", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Audio-Technica ATH-R70x", OVER, "Audio-Technica ATH-R70x", ["oratory1990/over-ear"]),
    ("Audio-Technica ATH-AD700X", OVER, "Audio-Technica ATH-AD700X", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sony MDR-7506", OVER, "Sony MDR-7506", ["oratory1990/over-ear"]),
    ("Sony MDR-V6", OVER, "Sony MDR-V6", ["Super Review/over-ear", "Innerfidelity/over-ear"]),
    ("Sony WH-1000XM4", OVER, "Sony WH-1000XM4", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Sony WH-1000XM5", OVER, "Sony WH-1000XM5", ["oratory1990/over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("HIFIMAN Sundara", OVER, "HIFIMAN Sundara (post-2020 earpads)", ["oratory1990/over-ear"]),
    ("HIFIMAN Ananda", OVER, "HIFIMAN Ananda", ["oratory1990/over-ear"]),
    ("HIFIMAN HE400se", OVER, "HIFIMAN HE400se", ["oratory1990/over-ear"]),
    ("HIFIMAN Edition XS", OVER, "HIFIMAN Edition XS", ["oratory1990/over-ear"]),
    ("HIFIMAN Arya", OVER, "HIFIMAN Arya", ["oratory1990/over-ear"]),
    ("Focal Clear", OVER, "Focal Clear", ["oratory1990/over-ear"]),
    ("Focal Utopia", OVER, "Focal Utopia", ["oratory1990/over-ear"]),
    ("Focal Elegia", OVER, "Focal Elegia", ["oratory1990/over-ear"]),
    ("Focal Bathys", OVER, "Focal Bathys", ["oratory1990/over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Philips SHP9500", OVER, "Philips SHP9500", ["oratory1990/over-ear"]),
    ("Philips Fidelio X2HR", OVER, "Philips Fidelio X2HR", ["oratory1990/over-ear"]),
    ("Bose QuietComfort 35 II", OVER, "Bose QuietComfort 35 II", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Bose QuietComfort 45", OVER, "Bose QuietComfort 45", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Bose Noise Cancelling Headphones 700", OVER, "Bose Noise Cancelling Headphones 700", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Apple AirPods Max", OVER, "Apple AirPods Max", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Audeze LCD-2", OVER, "Audeze LCD-2", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Koss Porta Pro", OVER, "Koss Porta Pro", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Koss KPH30i", OVER, "Koss KPH30i", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Samson SR850", OVER, "Samson SR850", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Superlux HD 681", OVER, "Superlux HD 681", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Meze 99 Classics", OVER, "Meze 99 Classics", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Shure SRH840", OVER, "Shure SRH840", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
        # --- gaming headsets ---
    ("SteelSeries Arctis 7+", OVER, "SteelSeries Arctis 7+", ["Rtings/HMS II.3 over-ear"]),
    ("SteelSeries Arctis 7P", OVER, "SteelSeries Arctis 7P", ["Rtings/HMS II.3 over-ear"]),
    ("SteelSeries Arctis 1", OVER, "SteelSeries Arctis 1", ["Rtings/HMS II.3 over-ear"]),
    ("SteelSeries Arctis 5 (2019)", OVER, "SteelSeries Arctis 5 2019 Edition", ["Rtings/HMS II.3 over-ear"]),
    ("SteelSeries Arctis 9", OVER, "SteelSeries Arctis 9", ["Rtings/HMS II.3 over-ear"]),
    ("SteelSeries Arctis Pro", OVER, "SteelSeries Arctis Pro", ["Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("SteelSeries Arctis Nova 7", OVER, "SteelSeries Arctis Nova 7", ["Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("SteelSeries Arctis Nova Pro Wireless", OVER, "SteelSeries Arctis Nova Pro Wireless (wired)", ["oratory1990/over-ear"]),
    ("HyperX Cloud II", OVER, "HyperX Cloud II", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("HyperX Cloud Alpha", OVER, "HyperX Cloud Alpha", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("HyperX Cloud Alpha Wireless", OVER, "HyperX Cloud Alpha Wireless", ["Rtings/Bruel & Kjaer 5128 over-ear", "Rtings/HMS II.3 over-ear"]),
    ("HyperX Cloud III", OVER, "HyperX Cloud III", ["Rtings/HMS II.3 over-ear"]),
    ("HyperX Cloud Flight", OVER, "HyperX Cloud Flight", ["Rtings/HMS II.3 over-ear"]),
    ("Logitech G Pro X 2", OVER, "Logitech G PRO X 2 LIGHTSPEED", ["Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Logitech G733", OVER, "Logitech G733 LIGHTSPEED", ["Rtings/HMS II.3 over-ear"]),
    ("Logitech G435", OVER, "Logitech G435 LIGHTSPEED", ["Rtings/HMS II.3 over-ear"]),
    ("Razer BlackShark V2", OVER, "Razer BlackShark V2", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("Razer BlackShark V2 Pro", OVER, "Razer BlackShark V2 Pro", ["Rtings/HMS II.3 over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Razer Kraken Ultimate", OVER, "Razer Kraken Ultimate", ["Rtings/HMS II.3 over-ear"]),
    ("Corsair HS80 RGB Wireless", OVER, "Corsair HS80 RGB WIRELESS", ["Rtings/HMS II.3 over-ear"]),
    ("Corsair Virtuoso RGB Wireless XT", OVER, "Corsair VIRTUOSO RGB Wireless XT", ["Rtings/HMS II.3 over-ear"]),
            ("Audeze Maxwell", OVER, "Audeze Maxwell", ["oratory1990/over-ear", "Rtings/Bruel & Kjaer 5128 over-ear"]),
    ("Sony INZONE H9", OVER, "Sony INZONE H9", ["Rtings/HMS II.3 over-ear"]),
    ("Sennheiser PC38X", OVER, "Sennheiser PC38X", ["oratory1990/over-ear", "Rtings/HMS II.3 over-ear"]),
    ("EPOS Sennheiser GSP 600", OVER, "EPOS Sennheiser GSP 600", ["Rtings/HMS II.3 over-ear"]),
    # --- in-ear ---
    ("Apple AirPods Pro 2", IN, "Apple Airpods Pro 2", ["Filk/in-ear", "Harpo/in-ear"]),
    ("Apple AirPods Pro", IN, "Apple AirPods Pro", ["oratory1990/in-ear", "Rtings/HMS II.3 in-ear"]),
    ("Samsung Galaxy Buds2 Pro", IN, "Samsung Galaxy Buds2 Pro", ["oratory1990/in-ear", "Rtings/Bruel & Kjaer 5128 in-ear"]),
    ("Sony WF-1000XM4", IN, "Sony WF-1000XM4", ["oratory1990/in-ear", "Rtings/HMS II.3 in-ear"]),
    ("Sony WF-1000XM5", IN, "Sony WF-1000XM5", ["oratory1990/in-ear", "Rtings/Bruel & Kjaer 5128 in-ear"]),
    ("Moondrop Aria", IN, "Moondrop Aria", ["oratory1990/in-ear", "Super Review/in-ear"]),
    ("Truthear Zero", IN, "Truthear x Crinacle Zero", ["oratory1990/in-ear", "Super Review/in-ear"]),
    ("Shure SE215", IN, "Shure SE215", ["oratory1990/in-ear", "Rtings/HMS II.3 in-ear"]),
    ("KZ ZSN Pro", IN, "KZ ZSN Pro", ["oratory1990/in-ear", "Super Review/in-ear"]),
    ("Sennheiser IE 200", IN, "Sennheiser IE 200", ["oratory1990/in-ear", "Super Review/in-ear"]),
    ("Sennheiser IE 600", IN, "Sennheiser IE 600", ["oratory1990/in-ear", "Super Review/in-ear"]),
    ("Etymotic ER2XR", IN, "Etymotic ER2XR", ["oratory1990/in-ear", "Super Review/in-ear"]),
    ("7Hz Salnotes Zero", IN, "7Hz Salnotes Zero", ["oratory1990/in-ear", "Super Review/in-ear"]),
]

# Rig targets (raw = what the user's file shows; deviation = raw - rig target): taken from a result of that rig.
RIGS = [
    ("gras", "Ear simulator (GRAS 43AG, oratory1990, squig.link)", "oratory1990/over-ear", "Sennheiser HD 600"),
    ("hms", "Head and torso HMS II.3 (Rtings until 2023)", "Rtings/HMS II.3 over-ear", "SteelSeries Arctis 7 Wireless 2017"),
    ("5128", "Bruel & Kjaer 5128 (Rtings since 2023)", "Rtings/Bruel & Kjaer 5128 over-ear", "AKG K361"),
    ("iem", "IEC 711 coupler (in-ears, squig.link)", "oratory1990/in-ear", "Apple AirPods Pro"),
]

# Sound targets besides "Neutral (Harman)": a target file minus the Harman file of the same rig.
TARGETS = [
    ("Harman without bass boost", "Harman over-ear 2018 without bass.csv", "Harman over-ear 2018.csv"),
    ("Diffuse field", "Diffuse field GRAS KEMAR.csv", "Harman over-ear 2018.csv"),
    ("oratory1990 optimum hifi", "oratory1990 optimum hifi over-ear.csv", "Harman over-ear 2018.csv"),
]

# Storage grid: 1/12 octave from 20 Hz.
GRID = [20.0 * 2 ** (i / 12) for i in range(0, 120) if 20.0 * 2 ** (i / 12) <= 20000.0 * 1.0001]


def get(path):
    url = RAW + urllib.parse.quote(path)
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.read().decode("utf-8")
    except Exception:
        return None


def read_csv(text):
    rows = list(csv.DictReader(io.StringIO(text)))
    return rows


def interp(fs, vs, f):
    if f <= fs[0]: return vs[0]
    if f >= fs[-1]: return vs[-1]
    lo, hi = 0, len(fs) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if fs[m] <= f: lo = m
        else: hi = m
    t = (math.log(f) - math.log(fs[lo])) / (math.log(fs[hi]) - math.log(fs[lo]))
    return vs[lo] + t * (vs[hi] - vs[lo])


def resample(fs, vs):
    # mean over each 1/12-octave cell (light smoothing), on a log axis
    out = []
    for g in GRID:
        a, b = g * 2 ** (-1 / 24), g * 2 ** (1 / 24)
        pts = [v for f, v in zip(fs, vs) if a <= f < b]
        out.append(sum(pts) / len(pts) if pts else interp(fs, vs, g))
    return out


target_cache = {}


def source_target(src):
    """Target column of any result CSV of this source dir (same rig)."""
    if src in target_cache: return target_cache[src]
    target_cache[src] = None
    for name in ["Sennheiser HD 600", "AKG K361", "AKG K371", "SteelSeries Arctis 7 Wireless 2017", "AKG K240 MKII",
                 "Apple AirPods Pro", "Moondrop Aria", "7Hz Salnotes Zero", "Audio-Technica ATH-M50x"]:
        t = get(f"results/{src}/{name}/{name}.csv")
        if t:
            rows = read_csv(t)
            target_cache[src] = ([float(r["frequency"]) for r in rows], [float(r["target"]) for r in rows])
            break
    return target_cache[src]


def measurement_paths(src, name):
    who, rig = src.split("/")
    kind = "in-ear" if "in-ear" in rig else "over-ear"
    sub = rig.replace(" over-ear", "").replace(" in-ear", "")
    paths = [f"measurements/{who}/data/{kind}/{name}.csv"]
    if sub not in ("over-ear", "in-ear"):
        paths.append(f"measurements/{who}/data/{kind}/{sub}/{name}.csv")
        paths.append(f"measurements/{who.lower()}/data/{kind}/{sub}/{name}.csv")
    return paths


def deviation(src, name):
    t = get(f"results/{src}/{name}/{name}.csv")
    if t:
        rows = read_csv(t)
        fs = [float(r["frequency"]) for r in rows]
        return fs, [float(r["raw"]) - float(r["target"]) for r in rows]
    for p in measurement_paths(src, name):
        m = get(p)
        if not m: continue
        tgt = source_target(src)
        if tgt is None: return None
        rows = read_csv(m)
        fs = [float(r["frequency"]) for r in rows]
        raw = [float(r["raw"]) for r in rows]
        return fs, [v - interp(tgt[0], tgt[1], f) for f, v in zip(fs, raw)]
    return None


def norm(vs):
    # 0 dB = mean between 300 Hz and 3 kHz
    sel = [v for g, v in zip(GRID, vs) if 300 <= g <= 3000]
    m = sum(sel) / len(sel)
    return [v - m for v in vs]


def line(vs):
    return " ".join(f"{v:.1f}".replace("-0.0", "0.0") for v in vs)


def main():
    out = ["# Hydra headphone database (Headphone Correction). Built by tools/fetch-headphones.py, do not edit.",
           "# Data: AutoEq project, https://github.com/jaakkopasanen/AutoEq (MIT license); measurements by the sources named.",
           "# grid: 1/12 octave from 20 Hz, " + str(len(GRID)) + " points",
           "# @hp name|kind|source  -> deviation from the Harman target of its rig (dB)",
           "# @rig id|name          -> the rig's Harman target (dB, absolute: a raw measurement minus it = deviation)",
           "# @target name          -> sound target relative to Harman (dB)"]
    missing = []
    for shown, kind, name, srcs in HEADPHONES:
        dev = None
        for s in srcs:
            dev = deviation(s, name)
            if dev:
                who = s.split("/")[0]
                break
        if not dev:
            missing.append(shown); print("MISSING", shown, file=sys.stderr); continue
        vs = norm(resample(*dev))
        out.append(f"@hp {shown}|{kind}|{who}")
        out.append(line(vs))
        print("ok", shown, "<-", s, file=sys.stderr)
    for rid, rname, src, hp in RIGS:
        tgt = source_target(src)
        t = get(f"results/{src}/{hp}/{hp}.csv")
        if t:
            rows = read_csv(t)
            tgt = ([float(r["frequency"]) for r in rows], [float(r["target"]) for r in rows])
        if tgt is None:
            print("MISSING rig", rid, file=sys.stderr); continue
        out.append(f"@rig {rid}|{rname}")
        out.append(line(norm(resample(*tgt))))
    for tname, a, b in TARGETS:
        ta, tb = get("targets/" + a), get("targets/" + b)
        if not ta or not tb:
            print("MISSING target", tname, file=sys.stderr); continue
        ra, rb = read_csv(ta), read_csv(tb)
        ka = [k for k in ra[0].keys() if k != "frequency"][0]
        kb = [k for k in rb[0].keys() if k != "frequency"][0]
        fa, va = [float(r["frequency"]) for r in ra], [float(r[ka]) for r in ra]
        fb, vb = [float(r["frequency"]) for r in rb], [float(r[kb]) for r in rb]
        da = resample(fa, va); db = resample(fb, vb)
        out.append(f"@target {tname}")
        out.append(line(norm([x - y for x, y in zip(da, db)])))
    OUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes), missing: {missing}", file=sys.stderr)


if __name__ == "__main__":
    main()
