"""Export a Le Mans Ultimate car's tire and aero physics as FoxEngine model files.

Usage:
    python Tools/External/LMU/export_lmu_physics.py <raw_dir> --hdv <file.hdv>
        --out <vehicle dir>

<raw_dir> holds files extracted from the car's .mas archives: the .hdv (aero
map, setup defaults, TireBrand and compound indices) and the TireBrand .tbc
(slip curves, compounds). <out>/layout.json (importer output) gives the axle
Z positions. Writes <out>/tires.json (FoxTires 1) and <out>/aero.json
(FoxAero 1); a sanity table (Cl*A, Cd*A, front balance, tire peaks at 200 and
250 km/h) is printed, never written.

Choices:
    - Only the Dry compound is exported; mu = DryLatLong * AIGripMult (the
      TBC tire is LMU's simplified AI tire; AIGripMult scales it to the TGM
      tire players drive). TabPeak values are exported as written: the engine
      compares them with sin(alpha).
    - rF2 applies HDV aero coefficients per rho*v^2; FoxAero uses 0.5*rho*v^2,
      so they are doubled (RF2_COEFFICIENT_SCALE).
    - Element centres are offsets from the axle at static ride height:
      engine centre = (-cx, rideHeight[axle] + cy, axleZ - cz); directions
      map as (-x, y, -z). Values use the default setup settings.
    - Rake terms: nose-down = rF2 Pos*, nose-up = rF2 Neg*.
"""

import argparse
import json
import math
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_OUT = REPO_ROOT / "Projects" / "Showcase" / "Assets" / "Vehicles" / "Porsche911GT3R"
SPEEDS_KMH = (200.0, 250.0)
TIRE_LOADS = (2000.0, 4000.0, 6000.0)
AIR_DENSITY = 1.225


# --------------------------------------------------------------------------- INI parsing

def strip_comment(line):
    i = line.find("//")
    return line if i < 0 else line[:i]


class Section:
    __slots__ = ("name", "values", "subs", "sub_lines", "lines")

    def __init__(self, name):
        self.name = name
        self.values = {}      # keys at section level (empty sub-block)
        self.subs = {}        # sub-block name -> {key: value}
        self.sub_lines = {}   # sub-block name -> list of bare numeric tokens
        self.lines = []       # stripped lines, for repeated keys and labels

    def value(self, key):
        if key not in self.values:
            raise SystemExit(f"[{self.name}]: missing key {key}")
        return self.values[key]

    def sub(self, name, required=True):
        if name not in self.subs:
            if required:
                raise SystemExit(f"[{self.name}]: missing sub-block {name}:")
            return {}
        return self.subs[name]


def read_sections(path):
    text = Path(path).read_text(encoding="latin-1")
    sections, cur, sub = [], None, ""
    for raw in text.splitlines():
        line = strip_comment(raw).strip()
        if not line:
            if sub == "data":
                sub = ""  # Data: values run until a blank line or the next section
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = Section(line[1:-1].strip().lower())
            sections.append(cur)
            sub = ""
            continue
        if cur is None:
            continue
        cur.lines.append(line)
        m = re.fullmatch(r"([A-Za-z_]\w*)\s*:", line)
        if m:
            sub = m.group(1).lower()
            cur.subs.setdefault(sub, {})
            cur.sub_lines.setdefault(sub, [])
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            key, value = key.strip().lower(), value.strip()
            if sub:
                cur.subs.setdefault(sub, {})[key] = value
            else:
                cur.values[key] = value
            continue
        if sub:
            cur.sub_lines.setdefault(sub, []).extend(line.split())
    return sections


def section(sections, name, required=True):
    for s in sections:
        if s.name == name:
            return s
    if required:
        raise SystemExit(f"missing section [{name}]")
    return None


def unquote(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        return text[1:-1]
    return text


def split_tuple(text):
    """Split a parenthesised tuple into its top-level items, quoted strings intact."""
    text = text.strip()
    if not (text.startswith("(") and text.endswith(")")):
        raise SystemExit(f"expected a tuple, got {text!r}")
    items, buf, in_quotes = [], [], False
    for ch in text[1:-1]:
        if ch == '"':
            in_quotes = not in_quotes
            buf.append(ch)
        elif ch == "," and not in_quotes:
            items.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    items.append("".join(buf).strip())
    while items and items[-1] == "":
        items.pop()
    return items


def numbers(text):
    return [float(item) for item in split_tuple(text) if item != ""]


def triple(text):
    values = numbers(text)
    if len(values) != 3:
        raise SystemExit(f"expected 3 numbers, got {text!r}")
    return values


def pair(text):
    values = numbers(text)
    if len(values) != 2:
        raise SystemExit(f"expected 2 numbers, got {text!r}")
    return values


def scalar(text):
    text = text.strip()
    if not text.startswith("("):
        return float(text)
    values = numbers(text)
    if len(values) != 1:
        raise SystemExit(f"expected 1 number, got {text!r}")
    return values[0]


def setting_value(range_text, setting_text):
    """A setting index i of a Range=(min, step, n) has value min + step * i."""
    minimum, step, _count = triple(range_text)
    return minimum + step * int(setting_text)


def special_label(sec, key, index, join_unit=False):
    """Display label of RangeSpecial=(index,"label","unit",...) for the given index.

    rF2 puts the readable value in the second field; some LMU brake-duct
    settings leave it empty and put e.g. "33%" in the third instead.
    """
    pattern = re.compile(rf'^{key}\s*=\s*\(\s*{index}\s*,\s*"([^"]*)"\s*(?:,\s*"([^"]*)")?', re.I)
    for line in sec.lines:
        m = pattern.match(line)
        if not m:
            continue
        first, second = m.group(1).strip(), (m.group(2) or "").strip()
        if join_unit:
            if first:
                return first + (second if second and not m.group(2)[0].isspace() else "")
            return second
        return first
    return ""


def label_and_value(sec, range_key, setting_key, special_key, join_unit=False):
    index = int(sec.value(setting_key))
    value = setting_value(sec.value(range_key), index)
    label = special_label(sec, special_key, index, join_unit)
    return index, value, label if label else f"{value:g}"


# --------------------------------------------------------------------------- tire export

def read_curve(sections, name):
    for sec in sections:
        if sec.name == "slipcurve" and unquote(sec.value("name")) == name:
            tokens = sec.sub_lines.get("data", [])
            try:
                data = [float(tok) for tok in tokens]
            except ValueError as exc:
                raise SystemExit(f"slip curve {name}: bad Data token ({exc})") from exc
            return {
                "step": float(sec.value("step")),
                "dropoff": float(sec.value("dropofffunction")),
                "data": data,
            }
    raise SystemExit(f"slip curve {name} not found")


def axle_tire(compound, pressure_kpa):
    # AIGripMult scales LMU's simplified AI tire (.tbc) to the TGM tire players
    # drive; without it the grip lands 12-20% low.
    grip_mult = scalar(compound["aigripmult"])
    dry_lat, dry_long = pair(compound["drylatlong"])
    return {
        "lateral": {
            "mu": dry_lat * grip_mult,
            "loadSensitivity": triple(compound["loadsenslat"]),
            "peakSlip": triple(compound["latpeak"]),
            "curve": unquote(compound["latcurve"]),
        },
        "longitudinal": {
            "mu": dry_long * grip_mult,
            "loadSensitivity": triple(compound["loadsenslong"]),
            "peakSlip": triple(compound["longpeak"]),
            "tractionCurve": unquote(compound["tractivecurve"]),
            "brakingCurve": unquote(compound["brakingcurve"]),
        },
        "speedEffects": pair(compound["speedeffects"]),
        "camber": triple(compound["camberlatlong"]),
        "rollingResistance": scalar(compound["rollingresistance"]),
        "verticalStiffness": scalar(compound["springbase"])
        + scalar(compound["springkpa"]) * pressure_kpa,
    }


# --------------------------------------------------------------------------- aero export

def clamp(value, low, high):
    return max(low, min(high, value))


def r2_direction(text):
    x, y, z = triple(text)
    return [-x, y, -z]


def r2_center(text, axle_z, ride_height):
    cx, cy, cz = triple(text)
    return [-cx, ride_height + cy, axle_z - cz]


def yaw_block(sec, sideways_key, peak_key):
    return {
        "sideways": scalar(sec.value(sideways_key)),
        "peakDeg": pair(sec.value(peak_key))[0],
        "peakMult": pair(sec.value(peak_key))[1],
    }


def require_zero(sec, key, label):
    if key not in sec.values:
        return
    if any(abs(v) > 1e-12 for v in numbers(sec.value(key))):
        raise SystemExit(f"{label}: unsupported non-zero {key}={sec.value(key)}")


def polynomial(text, value):
    a, b, c = triple(text)
    return a + b * value + c * value * value


# rF2 applies aero coefficients per rho*v^2, FoxAero per 0.5*rho*v^2, so
# Cl*A and Cd*A are twice the HDV numbers (confirmed against LMU telemetry).
RF2_COEFFICIENT_SCALE = 2.0


def coeff(value):
    return value * RF2_COEFFICIENT_SCALE


def coeffs(values):
    return [coeff(v) for v in values]


def rake_block(sec, prefix, kind):
    """rF2 <prefix>PosRake<kind> / NegRake<kind> as nose-down / nose-up terms.

    rF2 rake is rear minus front ride height, so positive rake is nose down
    (the HDV reference comments). Each term is (c1 * deg, c2 * deg^2, maxDeg).
    """
    def term(key):
        c1, c2, max_deg = triple(sec.value(key))
        return [coeff(c1), coeff(c2), max_deg]
    return {"noseDown": term(f"{prefix}posrake{kind}"),
            "noseUp": term(f"{prefix}negrake{kind}")}


def direction(text):
    return coeffs(r2_direction(text))


def build_aero(sections, layout, hdv_name):
    general = section(sections, "general")
    frontleft = section(sections, "frontleft")
    rearleft = section(sections, "rearleft")
    fw = section(sections, "frontwing")
    rw = section(sections, "rearwing")
    body = section(sections, "bodyaero")
    diff = section(sections, "diffuser")

    # Reject the parts of the rF2 aero map this model cannot represent.
    if "fwliftheightplus" in fw.values:
        raise SystemExit("FWLiftHeightPlus: unsupported (front-wing lift vs ride height slope)")
    for sec in (fw, rw, body, diff):
        for key in sec.values:
            if key.startswith("flap"):
                raise SystemExit(f"Flap*: unsupported aero key {key} in [{sec.name}]")
    for name in ("leftfender", "rightfender"):
        fender = section(sections, name, required=False)
        if fender is None:
            continue
        require_zero(fender, "fenderdragparams", f"[{name}]")
        require_zero(fender, "fenderliftparams", f"[{name}]")
    require_zero(diff, "diffuseroffsetz", "[DIFFUSER]")

    front_rh = setting_value(frontleft.value("rideheightrange"), frontleft.value("rideheightsetting"))
    rear_rh = setting_value(rearleft.value("rideheightrange"), rearleft.value("rideheightsetting"))

    wheels = layout["wheels"]
    front_axle_z = (wheels["fl"]["center"][2] + wheels["fr"]["center"][2]) / 2.0
    rear_axle_z = (wheels["rl"]["center"][2] + wheels["rr"]["center"][2]) / 2.0

    fw_max_height = scalar(fw.value("fwmaxheight"))
    lut_range = pair(fw.value("fwliftheightrange"))
    if abs(fw_max_height - lut_range[1]) > 1e-9:
        raise SystemExit(
            f"FWMaxHeight {fw_max_height} != FWLiftHeightRange max {lut_range[1]}")

    fw_index, fw_value, fw_label = label_and_value(fw, "fwrange", "fwsetting", "fwspecial")
    rw_index, rw_value, rw_label = label_and_value(rw, "rwrange", "rwsetting", "rwspecial")

    body_drag = scalar(body.value("bodydragbase"))
    body_lift = triple(body.value("bodyfore"))[1]
    duct_terms = []
    for prefix in ("WaterRadiator", "OilRadiator", "Radiator", "BrakeDuct", "BrakeDuctRear"):
        range_key = f"{prefix.lower()}range"
        if range_key not in body.values:
            continue
        setting_key = f"{prefix.lower()}setting"
        drag_key = f"{prefix.lower()}drag"
        lift_key = f"{prefix.lower()}lift"
        index, value, label = label_and_value(body, range_key, setting_key,
                                               f"{prefix.lower()}special",
                                               join_unit=prefix.startswith("BrakeDuct"))
        drag = scalar(body.value(drag_key)) if drag_key in body.values else 0.0
        lift = scalar(body.value(lift_key)) if lift_key in body.values else 0.0
        body_drag += drag * value
        body_lift += lift * value
        duct_terms.append(f"{prefix} {index} ({label})")

    source = (
        f"{hdv_name}; RWSetting {rw_index} ({rw_label}), FWSetting {fw_index} ({fw_label})"
        + (", " + ", ".join(duct_terms) if duct_terms else "")
    )

    diffuser_base = numbers(diff.value("diffuserbaseplus"))
    front_height = numbers(diff.value("diffuserfrontheightplus"))
    diffuser_rake = triple(diff.value("diffuserrake"))
    aero = {
        "format": "FoxAero",
        "version": 1,
        "source": source,
        "airDensity": AIR_DENSITY,
        "staticRideHeight": [front_rh, rear_rh],
        "frontAxleZ": front_axle_z,
        "rearAxleZ": rear_axle_z,
        "frontWing": {
            "center": r2_center(fw.value("fwcenter"), front_axle_z, front_rh),
            "drag": coeff(polynomial(fw.value("fwdragparams"), fw_value)),
            "lift": coeff(polynomial(fw.value("fwliftparams"), fw_value)),
            "rakeDrag": rake_block(fw, "fw", "drag"),
            "rakeLift": rake_block(fw, "fw", "lift"),
            "yaw": yaw_block(fw, "fwliftsideways", "fwliftpeakyaw"),
            "directional": {
                "left": direction(fw.value("fwleft")),
                "right": direction(fw.value("fwright")),
                "up": direction(fw.value("fwup")),
                "down": direction(fw.value("fwdown")),
                "back": direction(fw.value("fwaft")),
            },
            "heightLut": {
                "range": lut_range,
                "values": numbers(fw.value("fwliftheightlut")),
            },
        },
        "rearWing": {
            "center": r2_center(rw.value("rwcenter"), rear_axle_z, rear_rh),
            "drag": coeff(polynomial(rw.value("rwdragparams"), rw_value)),
            "lift": coeff(polynomial(rw.value("rwliftparams"), rw_value)),
            "rakeDrag": rake_block(rw, "rw", "drag"),
            "rakeLift": rake_block(rw, "rw", "lift"),
            "yaw": yaw_block(rw, "rwliftsideways", "rwliftpeakyaw"),
            "directional": {
                "left": direction(rw.value("rwleft")),
                "right": direction(rw.value("rwright")),
                "up": direction(rw.value("rwup")),
                "down": direction(rw.value("rwdown")),
                "back": direction(rw.value("rwaft")),
            },
        },
        "body": {
            "center": r2_center(body.value("bodycenter"), rear_axle_z, rear_rh),
            "drag": coeff(body_drag),
            "lift": coeff(body_lift),
            "rakeDrag": rake_block(body, "body", "drag"),
            "rakeLift": rake_block(body, "body", "lift"),
            "directional": {
                "left": direction(body.value("bodyleft")),
                "right": direction(body.value("bodyright")),
                "up": direction(body.value("bodyup")),
                "down": direction(body.value("bodydown")),
                "back": direction(body.value("bodyaft")),
            },
            "dragHeightAvg": coeff(scalar(body.value("bodydragheightavg"))),
            "dragHeightDiff": coeff(scalar(body.value("bodydragheightdiff"))),
            "maxHeight": scalar(body.value("bodymaxheight")),
        },
        "diffuser": {
            "center": r2_center(diff.value("diffusercenter"), rear_axle_z, rear_rh),
            "lift": coeff(diffuser_base[0]),
            "yaw": yaw_block(diff, "diffusersideways", "diffuserpeakyaw"),
            "rearHeight": coeffs(diffuser_base[1:4]),
            "frontHeight": coeffs(front_height[:3]) + front_height[3:],
            "rake": [diffuser_rake[0]] + coeffs(diffuser_rake[1:]),
            "limits": triple(diff.value("diffuserlimits")),
            "stall": pair(diff.value("diffuserstall")),
            "undertrayHalfWidth": abs(triple(general.value("undertray00"))[0]),
        },
    }
    setup = {
        "frontRideHeight": front_rh,
        "rearRideHeight": rear_rh,
        "fw": (fw_index, fw_label),
        "rw": (rw_index, rw_label),
        "ducts": duct_terms,
    }
    return aero, setup


# --------------------------------------------------------------------------- sanity evaluation

def lut_value(lut_range, values, height):
    lo, hi = lut_range
    if hi <= lo or len(values) == 1:
        return values[0]
    t = (clamp(height, lo, hi) - lo) / (hi - lo) * (len(values) - 1)
    i = int(math.floor(t))
    if i >= len(values) - 1:
        return values[-1]
    return values[i] + (values[i + 1] - values[i]) * (t - i)


def rake_value(rake_triple, rake_deg):
    c1, c2, maximum = rake_triple
    t = min(abs(rake_deg), maximum)
    return c1 * t + c2 * t * t


def aero_elements(aero):
    """(center, drag coefficient, lift coefficient) at rest, zero yaw, per element."""
    front_rh, rear_rh = aero["staticRideHeight"]
    fz, rz = aero["frontAxleZ"], aero["rearAxleZ"]
    wheelbase = fz - rz
    rake = rear_rh - front_rh
    rake_deg = math.degrees(math.atan2(rake, wheelbase))

    def height(z):
        return rear_rh + (front_rh - rear_rh) * (z - rz) / wheelbase

    def rake_terms(element, rake_deg):
        key = "noseDown" if rake_deg >= 0 else "noseUp"
        drag = rake_value(element["rakeDrag"][key], rake_deg)
        lift = rake_value(element["rakeLift"][key], rake_deg)
        return drag, lift

    fw = aero["frontWing"]
    fw_rake_drag, fw_rake_lift = rake_terms(fw, rake_deg)
    fw_lift = (fw["lift"] * lut_value(fw["heightLut"]["range"], fw["heightLut"]["values"],
                                      height(fw["center"][2]))
               + fw_rake_lift)
    fw_drag = fw["drag"] + fw_rake_drag

    rw = aero["rearWing"]
    rw_rake_drag, rw_rake_lift = rake_terms(rw, rake_deg)
    rw_lift = rw["lift"] + rw_rake_lift
    rw_drag = rw["drag"] + rw_rake_drag

    body = aero["body"]
    body_rake_drag, body_rake_lift = rake_terms(body, rake_deg)
    max_height = body["maxHeight"]
    body_drag = (body["drag"]
                 + body["dragHeightAvg"] * clamp((front_rh + rear_rh) / 2.0, 0.0, max_height)
                 + body["dragHeightDiff"] * clamp(rake, -max_height, max_height)
                 + body_rake_drag)
    body_lift = body["lift"] + body_rake_lift

    diff = aero["diffuser"]
    rear_height, front_height = diff["rearHeight"], diff["frontHeight"]
    limits = diff["limits"]
    h_rear = clamp(rear_rh, 0.0, limits[1])
    h_front = clamp(front_rh, 0.0, front_height[3])
    d = clamp(rake - diff["rake"][0], -limits[2], limits[2])
    diff_lift = (diff["lift"]
                 + rear_height[0] * math.sqrt(h_rear) + rear_height[1] * h_rear
                 + rear_height[2] * h_rear * h_rear
                 + front_height[0] * math.sqrt(h_front) + front_height[1] * h_front
                 + front_height[2] * h_front * h_front
                 + diff["rake"][1] * d + diff["rake"][2] * d * d)
    corners = [height(fz), height(fz), height(rz), height(rz)]
    corner_min, corner_mean = min(corners), sum(corners) / len(corners)
    hs = corner_min + (corner_mean - corner_min) * diff["stall"][0]
    if limits[0] > 0.0 and hs < limits[0]:
        diff_lift *= 1.0 - diff["stall"][1] * (1.0 - max(hs, 0.0) / limits[0])

    return [
        (fw["center"], fw_drag, fw_lift),
        (rw["center"], rw_drag, rw_lift),
        (body["center"], body_drag, body_lift),
        (diff["center"], 0.0, diff_lift),
    ], rake_deg


def aero_totals(aero):
    """Cl*A (positive downforce), Cd*A and front aerodynamic balance %."""
    elements, rake_deg = aero_elements(aero)
    fz, rz = aero["frontAxleZ"], aero["rearAxleZ"]
    wheelbase = fz - rz
    total_downforce = sum(-lift for _, _, lift in elements)
    front_downforce = sum(-lift * (center[2] - rz) / wheelbase for center, _, lift in elements)
    drag = sum(drag for _, drag, _ in elements)
    balance = 100.0 * front_downforce / total_downforce if total_downforce else 0.0
    return total_downforce, drag, balance, rake_deg


def load_factor(a, m, fmax, load):
    if load >= fmax:
        return m
    c = (m - 1.0 - a * fmax) / (fmax * fmax)
    return 1.0 + a * load + c * load * load


def peak_slip(p0, p1, lref, load):
    return p0 + (p1 - p0) * clamp(load / lref, 0.0, 1.0)


def tire_sanity(tire, speed_kmh):
    v = speed_kmh / 3.6
    half_grip, load_equiv = tire["speedEffects"]
    speed_mult = 1.0 / (1.0 + v / half_grip) if half_grip else 1.0
    rows = []
    for load in TIRE_LOADS:
        a, m, fmax = tire["lateral"]["loadSensitivity"]
        mu = tire["lateral"]["mu"] * load_factor(a, m, fmax, load) * speed_mult
        p0, p1, lref = tire["lateral"]["peakSlip"]
        sp = peak_slip(p0, p1, lref, load + load_equiv * v)
        rows.append((load, mu, math.degrees(math.asin(clamp(sp, 0.0, 1.0)))))
    return rows


def print_report(tire_source, setup, aero, tires):
    print("Setup")
    print(f"  tires   {tire_source}")
    print(f"  aero    {aero['source']}")
    fw_index, fw_label = setup["fw"]
    rw_index, rw_label = setup["rw"]
    print(f"  wing    FW {fw_index} ({fw_label}), RW {rw_index} ({rw_label})")
    print(f"  ride    front {setup['frontRideHeight']:.4f} m, rear {setup['rearRideHeight']:.4f} m")
    print(f"  press   front {setup['frontPressure']:g} kPa, rear {setup['rearPressure']:g} kPa")
    print(f"  camber  front {setup['frontCamber']:+.2f} deg, rear {setup['rearCamber']:+.2f} deg")
    print()
    print("Derived")
    print(f"  axle Z  front {aero['frontAxleZ']:+.5f} m, rear {aero['rearAxleZ']:+.5f} m")
    print(f"  wheelbase {aero['frontAxleZ'] - aero['rearAxleZ']:.5f} m")
    for name in ("frontWing", "rearWing", "body", "diffuser"):
        center = aero[name]["center"]
        print(f"  {name:9s} center [{center[0]:+.4f}, {center[1]:.4f}, {center[2]:+.4f}]")
    for name in ("front", "rear"):
        t = tires[name]
        print(f"  {name:5s} tire mu lat/long {t['lateral']['mu']:.4f}/{t['longitudinal']['mu']:.4f}"
              f", vertical stiffness {t['verticalStiffness']:.0f} N/m")
    print()
    print("Aero at static ride height, zero yaw")
    print("  speed(km/h)   Cl*A     Cd*A   downforce(N)   drag(N)   front balance   rake(deg)")
    for speed in SPEEDS_KMH:
        cla, cda, balance, rake_deg = aero_totals(aero)
        q = 0.5 * aero["airDensity"] * (speed / 3.6) ** 2
        print(f"  {speed:10.0f}   {cla:6.3f}   {cda:6.3f}   {cla * q:10.0f}   {cda * q:8.0f}"
              f"   {balance:9.1f} %   {rake_deg:9.3f}")
    print()
    print("Tires: peak lateral mu and peak slip angle (load sensitivity + speed effects)")
    print("  speed(km/h)   axle   load(N)     mu   slip(deg)")
    for speed in SPEEDS_KMH:
        for name in ("front", "rear"):
            for load, mu, angle in tire_sanity(tires[name], speed):
                print(f"  {speed:10.0f}   {name:5s}  {load:7.0f}   {mu:5.3f}   {angle:8.2f}")
    print()


def clean_json(value):
    """Drop -0.0 and float noise (12 significant digits) for plain, stable JSON."""
    if isinstance(value, float):
        if value == 0.0:
            return 0.0
        return float(f"{value:.12g}")
    if isinstance(value, list):
        return [clean_json(item) for item in value]
    if isinstance(value, dict):
        return {key: clean_json(item) for key, item in value.items()}
    return value


# --------------------------------------------------------------------------- entry point

def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("raw_dir", type=Path, help="directory of files extracted from the car's .mas")
    parser.add_argument("--hdv", required=True, help="the car's .hdv file name inside raw_dir")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="vehicle directory holding layout.json")
    args = parser.parse_args()

    hdv_path = args.raw_dir / args.hdv
    out_dir = args.out
    hdv = read_sections(hdv_path)
    layout = json.loads((out_dir / "layout.json").read_text(encoding="utf-8"))

    general = section(hdv, "general")
    tbc_name = unquote(general.value("tirebrand"))
    tbc = read_sections(args.raw_dir / tbc_name)

    front_index = int(general.value("fronttirecompoundsetting"))
    rear_index = int(general.value("reartirecompoundsetting"))
    compounds = [s for s in tbc if s.name == "compound"]
    if not compounds:
        raise SystemExit(f"{tbc_name}: no [Compound] sections")
    front_compound = compounds[front_index]
    rear_compound = compounds[rear_index]

    frontleft = section(hdv, "frontleft")
    rearleft = section(hdv, "rearleft")
    front_pressure = setting_value(frontleft.value("pressurerange"), frontleft.value("pressuresetting"))
    rear_pressure = setting_value(rearleft.value("pressurerange"), rearleft.value("pressuresetting"))

    front_tire = axle_tire(front_compound.sub("front"), front_pressure)
    rear_tire = axle_tire(rear_compound.sub("rear"), rear_pressure)

    curve_names = []
    for tire in (front_tire, rear_tire):
        for name in (tire["lateral"]["curve"], tire["longitudinal"]["tractionCurve"],
                     tire["longitudinal"]["brakingCurve"]):
            if name not in curve_names:
                curve_names.append(name)
    curves = {name: read_curve(tbc, name) for name in curve_names}

    front_name = unquote(front_compound.value("name"))
    rear_name = unquote(rear_compound.value("name"))
    if front_index == rear_index:
        tire_source = f'{tbc_name}, compound {front_index} "{front_name}" (front and rear)'
    else:
        tire_source = (f'{tbc_name}, compound {front_index} "{front_name}" front, '
                       f'compound {rear_index} "{rear_name}" rear')
    tires = {
        "format": "FoxTires",
        "version": 1,
        "source": tire_source,
        "curves": curves,
        "front": front_tire,
        "rear": rear_tire,
    }

    aero, aero_setup = build_aero(hdv, layout, hdv_path.name)

    front_camber = setting_value(frontleft.value("camberrange"), frontleft.value("cambersetting"))
    rear_camber = setting_value(rearleft.value("camberrange"), rearleft.value("cambersetting"))

    setup = dict(aero_setup)
    setup["frontPressure"] = front_pressure
    setup["rearPressure"] = rear_pressure
    setup["frontCamber"] = front_camber
    setup["rearCamber"] = rear_camber

    out_dir.mkdir(parents=True, exist_ok=True)
    for name, payload in (("tires.json", tires), ("aero.json", aero)):
        path = out_dir / name
        with path.open("w", encoding="ascii", newline="\n") as stream:
            stream.write(json.dumps(clean_json(payload), indent=2) + "\n")
        print(f"wrote {path}")

    print()
    print_report(tire_source, setup, aero, tires)
    print(f"vehicle.json camberDeg: front {front_camber:g}, rear {rear_camber:g}")


if __name__ == "__main__":
    main()
