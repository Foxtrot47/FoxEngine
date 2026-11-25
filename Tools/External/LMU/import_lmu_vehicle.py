"""Import a Le Mans Ultimate car (rFactor 2 gMotor assets) as FoxEngine vehicle visuals.

Usage:
    python Tools/External/LMU/import_lmu_vehicle.py <raw_dir> <livery.veh> [--out DIR]
        [--view-distance M] [--max-texture-size N]

<raw_dir> is the directory of files extracted from the car's .mas archives
(GMT meshes, .gen, .veh, material .json, DDS textures).

Outputs (default Projects/Showcase/Assets/Vehicles/Porsche911GT3R/):
    body.gltf/.bin                 chassis, bodywork, glass, cockpit and driver
    wheel_{fl,fr,rl,rr}.gltf/.bin  tire, rim and brake disc; spin with the wheel
    spindle_{fl,fr,rl,rr}.gltf/.bin  brake caliper; steers but does not spin
    textures/*.dds                 BC-compressed, full mip chains
    layout.json                    wheel centers/radii, body bounds, collision hull

Pipeline:
    .veh  -> livery, .gen file and its GenString token substitutions
    .gen  -> instance tree; per instance the <MAX>-detail meshes whose LOD range
             contains --view-distance (default 3 m: the exterior LODs, past the
             2 m cockpit-only meshes), excluding Render=False shadow/SDF proxies.
             Instances are classified by name: <LF|RF|LR|RR>TIRE -> wheel,
             ...SPINDLE -> spindle, everything else -> body. Motion-blur rims
             and state-dependent overlays (light glows, displays) are skipped.
    .gmt  -> geometry, plus each mesh's pivot (header 0x114): wheel parts are
             authored around their wheel center, which is where the game puts
             them, so the pivots are the wheel positions.
    material .json + livery -> glTF 2.0 metallic-roughness materials. LMU's
             shaders are baked down to what the engine renders: one UV set,
             base color, tangent-space normal map, packed metallic-roughness
             (glTF layout: G = roughness, B = metallic), alpha mode. LMU's
             metalRough maps (R = metallic, G = roughness) are repacked with the
             material's Metallic/Roughness adjust applied (0.5 = neutral).
             Tiled detail layers (UV_Scale != 1), second-UV-set AO and
             missing shared textures become constant values.
    WCCARBODY (livery) -> livery base color; metallic-roughness baked from the
             livery's region mask (R/G/B/black/white/alpha) and the paint
             finishes the livery JSON selects for each region.

Frames:
    GMT: left-handed, Y up, +X left, +Z rear.
    glTF out: right-handed, Y up, +X right, -Z forward, origin on the ground
              plane midway between the axles: (-x, y, z - zmid).
    Engine (Assimp ConvertToLeftHanded negates Z): +X right, +Z forward;
              layout.json uses this space.
    The X mirror converts GMT's clockwise winding to glTF's counter-clockwise.

Textures reference DDS files directly (MSFT_texture_dds, no PNG fallback);
MaterialTexturePath resolves them as <dir>/textures/<stem>.dds.

Shared GMT/material/texture/glTF code lives in lmu_assets.py.
"""

import argparse
import json
import re
from pathlib import Path

import numpy as np

from lmu_assets import (REPO_ROOT, MaterialBuilder, Primitive, RawDir, Gmt, TextureWriter,
                        load_rgba, rounded, strip_comment, write_gltf)

DEFAULT_OUT = REPO_ROOT / "Projects" / "Showcase" / "Assets" / "Vehicles" / "Porsche911GT3R"

WHEELS = ("fl", "fr", "rl", "rr")
WHEEL_PREFIX = {"LF": "fl", "RF": "fr", "LR": "rl", "RR": "rr"}
GMT_TO_GLTF = np.array([-1.0, 1.0, 1.0])  # X mirror; see "Frames" above

# Instances whose meshes are not part of the static car.
SKIP_INSTANCES = {"HLIGHTDS", "HLIGHTPS", "BLIGHTDS", "BLIGHTPS", "DISPLAYPANEL_992"}
SKIP_MESH = re.compile(r"RIM_?BLUR", re.I)

# Materials rendered by dynamic or emissive-only shaders (glows, displays).
SKIP_MATERIALS = {
    "?", "", "BLGLO_992_BRAKES", "HLGLO_992_Y", "DISCGLOW", "DISPLAYPANELBG_992",
    "992GT3-24_HUD", "992GT3-24_HUDDATA", "992GT3-24_FLAGBG", "992GT3-24_MIRROROVERLAY",
    "992GT3-24_MIRROR_DIRT", "992GT3-24_DECALS_EM", "WCSWHEEL",
    "F488GTE_LIGHTGLOW", "F488GTE_EXTLED", "F488GTE_OVERLAY", "F488GTE_DISPLAYBG",
    "F488GTE_RTECH06",
}

# Constant materials: shared textures that are not in the car archives
# (CommonMaps), or shaders with no texture worth keeping.
# name -> (linear base color, roughness, metallic)
CONSTANT_MATERIALS = {
    "LM100_TIRE_GTE": ((0.030, 0.030, 0.030), 0.88, 0.0),
    "BRAKE_SET": ((0.20, 0.20, 0.21), 0.45, 0.8),
    "LMU_HARNESS": ((0.04, 0.05, 0.08), 0.85, 0.0),
    "MIRROR": ((0.90, 0.90, 0.90), 0.04, 1.0),
    "MIRROR_EXT": ((0.90, 0.90, 0.90), 0.04, 1.0),
    "WCEXTRA1": ((0.02, 0.02, 0.02), 0.75, 0.0),  # gloves (HRX_Black_Albr is shared)
    "WCEXTRA4": ((0.02, 0.02, 0.02), 0.75, 0.0),
    "LMU_BOOTS": ((0.02, 0.02, 0.02), 0.70, 0.0),
    "992GT3-24_PLASTIC_EM": ((0.03, 0.03, 0.03), 0.50, 0.0),  # backlit button caps
}

FINISH_ROUGHNESS = {"Shiny": 0.15, "Medium": 0.32, "Matte": 0.62}


# --------------------------------------------------------------------------- input files

def read_veh(path):
    keys = {}
    for line in path.read_text(encoding="latin-1").splitlines():
        m = re.match(r'(\w+)\s*=\s*"?([^"]*)"?', strip_comment(line))
        if m:
            keys.setdefault(m.group(1).lower(), m.group(2).strip())
    return keys


def read_gen(path, gen_string, view_distance):
    """(instance name, mesh file) of every rendered <MAX>-detail mesh whose
    LODIn..LODOut range contains `view_distance` (meters from the camera)."""
    parts, stack, pending = [], [], None
    for raw in path.read_text(encoding="latin-1").splitlines():
        line = strip_comment(raw)
        instance = re.match(r"Instance\s*=\s*(\S+)", line, re.I)
        if instance:
            pending = instance.group(1)
        elif line.startswith("{"):
            stack.append(pending or "?")
            pending = None
        elif line.startswith("}"):
            stack = stack[:-1]
        elif is_visible_mesh_line(line, view_distance):
            mesh = substitute_gen_string(mesh_file_of(line), gen_string)
            parts.append((stack[-1] if stack else "", mesh))
    return unique_by_mesh(parts)


def mesh_file_of(line):
    m = re.search(r"MeshFile\s*=\s*(\S+\.gmt)", line, re.I)
    return m.group(1) if m else None


def is_visible_mesh_line(line, view_distance):
    if not mesh_file_of(line):
        return False
    tag = re.match(r"<(\w+)>", line)
    if tag and tag.group(1).upper() != "MAX":
        return False
    if re.search(r"CollTarget\s*=\s*True|Render\s*=\s*False", line, re.I):
        return False
    lod_in = gen_number(line, "LODIn")
    lod_out = gen_number(line, "LODOut")
    return not (lod_in is not None and lod_in > view_distance) and \
        not (lod_out is not None and lod_out <= view_distance)


def gen_number(line, key):
    m = re.search(key + r"\s*=\s*\(\s*([\d.]+)", line, re.I)
    return float(m.group(1)) if m else None


def substitute_gen_string(mesh, gen_string):
    """Replace the .gen's <1>, <2>, ... placeholders with the livery's GenString tokens."""
    for i, token in enumerate(gen_string, start=1):
        mesh = mesh.replace(f"<{i}>", token)
    return mesh


def unique_by_mesh(parts):
    seen, unique = set(), []
    for inst, mesh in parts:
        if mesh.lower() not in seen:
            seen.add(mesh.lower())
            unique.append((inst, mesh))
    return unique


# --------------------------------------------------------------------------- materials

class VehicleMaterialBuilder(MaterialBuilder):
    """Adds the livery: WCCARBODY and the WC* driver, helmet, windshield and
    extra materials the .veh replaces."""

    def __init__(self, raw, textures, veh):
        super().__init__(raw, textures, CONSTANT_MATERIALS)
        self.livery = raw.texture(veh["defaultlivery"])
        if self.livery is None:
            raise FileNotFoundError(f"livery {veh['defaultlivery']} not found")
        self.livery_stem = self.livery.stem
        self.extras = {k: v for k, v in veh.items() if re.fullmatch(r"extra\d+", k)}

    def _override_texture(self, name):
        """Livery-specific replacement for a WC* material, following rFactor's naming."""
        suffix = {"WCDRIVER": "Driver", "WCHELMET": "Helmet", "WCWINDSHIELDOUT": "WindshieldOut",
                  "WCWINDSHIELDIN": "WindshieldIn"}.get(name)
        if suffix:
            return self.raw.texture(self.livery_stem + suffix)
        m = re.fullmatch(r"WCEXTRA(\d+)", name)
        if m:
            value = self.extras.get(f"extra{m.group(1)}", "")
            if value.startswith(":"):
                return self.raw.texture(value)
            if value.upper() == "PLATE":
                return self.raw.texture(self.livery_stem + "PLATE")
        return None

    def _build_special(self, mat, name):
        if name != "WCCARBODY":
            return False
        self._build_livery(mat)
        return True

    def _build_livery(self, mat):
        mat.base_color = self.textures.copy(self.livery)
        region = self.raw.texture(self.livery_stem + "_region")
        selected = (self.raw.json(self.livery_stem) or {}).get("meta", {}).get("selectedMaterial", {})
        if region is None or not selected:
            self._solid_mr(mat, 0.3, 0.0)
            self.report.append("WCCARBODY: livery, no region mask")
            return
        rgba = load_rgba(region)
        r, g, b, a = (rgba[..., i] for i in range(4))
        white = np.minimum(np.minimum(r, g), b)
        weights = {"Red": r - white, "Green": g - white, "Blue": b - white, "White": white}
        weights["Black"] = np.clip(1.0 - sum(weights.values()), 0, 1)
        for key in weights:
            weights[key] = weights[key] * a
        weights["Alpha"] = 1.0 - a
        rough = np.zeros_like(r)
        metal = np.zeros_like(r)
        for key, w in weights.items():
            finish_rough, finish_metal = paint_finish(selected.get(key, "vinylHeavyMedium"))
            rough += w * finish_rough
            metal += w * finish_metal
        packed = np.stack([np.ones_like(r), rough, metal, np.ones_like(r)], axis=-1)
        mat.metal_rough = self.textures.encode(f"{mat.name}_mr.dds", packed)
        self.report.append(f"WCCARBODY: livery {self.livery.name}, region finishes {selected}")


def paint_finish(preset):
    """(roughness, metallic) of an LMU paint preset name such as vinylHeavyMatte,
    carPaintMetallicMedium or carbonFibreShinyStrong."""
    roughness = 0.15 if preset == "carPaint" else 0.32
    for finish, value in FINISH_ROUGHNESS.items():
        if finish in preset:
            roughness = value
    metallic = 0.6 if preset.startswith("carPaintMetallic") else 0.0
    return roughness, metallic


# --------------------------------------------------------------------------- layout

def hull_points(points, target=64, max_points=128):
    """Support points of `points` over Fibonacci-sphere directions (all lie on the convex hull)."""
    k = target
    while True:
        i = np.arange(k) + 0.5
        phi = np.arccos(1 - 2 * i / k)
        theta = np.pi * (1 + 5 ** 0.5) * i
        dirs = np.stack([np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)], axis=1)
        idx = np.unique(np.argmax(points @ dirs.T, axis=0))
        if len(idx) >= target or k >= 16384:
            break
        k = int(k * 1.25)
    return points[idx[:max_points]]


# --------------------------------------------------------------------------- main

def classify(instance):
    """'body', ('wheel', key) or ('spindle', key)."""
    m = re.fullmatch(r"(LF|RF|LR|RR)(TIRE|SPINDLE)", instance.upper())
    if not m:
        return "body"
    return ("wheel" if m.group(2) == "TIRE" else "spindle", WHEEL_PREFIX[m.group(1)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", type=Path, help="directory of files extracted from the car's .mas archives")
    ap.add_argument("veh", help="livery .veh file name inside <raw>")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--view-distance", type=float, default=3.0)
    ap.add_argument("--max-texture-size", type=int, default=2048)
    args = ap.parse_args()

    raw = RawDir(args.raw)
    veh_path = raw.find(args.veh)
    if veh_path is None:
        raise SystemExit(f"{args.veh} not found in {args.raw}")
    veh = read_veh(veh_path)
    gen = raw.find(veh["graphics"])
    parts = read_gen(gen, veh.get("genstring", ""), args.view_distance)

    args.out.mkdir(parents=True, exist_ok=True)
    textures = TextureWriter(args.out, args.max_texture_size)
    builder = VehicleMaterialBuilder(raw, textures, veh)

    meshes = load_meshes(raw, parts)
    wheel_centers = symmetric_wheel_centers(meshes)
    z_mid = 0.5 * (wheel_centers["fl"][2] + wheel_centers["rl"][2])
    groups, tire_extent = build_primitives(meshes, wheel_centers, z_mid, builder)

    out_files = {}
    for group, prims in sorted(groups.items(), key=lambda kv: str(kv[0])):
        stem = "body" if group == "body" else f"{group[0]}_{group[1]}"
        verts, tris = write_gltf(args.out / f"{stem}.gltf", stem, list(prims.values()),
                                 "FoxEngine Tools/import_lmu_vehicle.py")
        out_files[stem] = (verts, tris, len(prims))

    layout = build_layout(groups, tire_extent, wheel_centers, z_mid)
    (args.out / "layout.json").write_text(json.dumps(layout, indent=2), encoding="ascii")
    textures.cleanup()
    print_summary(args.out, veh_path, veh, builder, textures, out_files, layout)


def load_meshes(raw, parts):
    """[(group, Gmt)] for the static car's .gen parts."""
    meshes = []
    for instance, mesh_file in parts:
        if instance in SKIP_INSTANCES or SKIP_MESH.search(mesh_file):
            continue
        path = raw.find(mesh_file)
        if path is None:
            print(f"warning: {mesh_file} (instance {instance}) not found")
            continue
        meshes.append((classify(instance), Gmt(path)))
    return meshes


def symmetric_wheel_centers(meshes):
    """Wheel -> GMT-space wheel center, taken from the first wheel part's pivot."""
    wheel_centers = {}
    for group, gmt in meshes:
        if group != "body":
            wheel_centers.setdefault(group[1], gmt.pivot)
    missing = [w for w in WHEELS if w not in wheel_centers]
    if missing:
        raise SystemExit(f"no wheel meshes for {missing}")
    # Mesh pivots carry sub-millimetre authoring noise between left and right;
    # the simulated car pulls to one side on an asymmetric layout, so each
    # axle is mirrored about the centerline.
    for left, right in (("fl", "fr"), ("rl", "rr")):
        half_track = 0.5 * (abs(wheel_centers[left][0]) + abs(wheel_centers[right][0]))
        y, z = 0.5 * (wheel_centers[left][1:] + wheel_centers[right][1:])
        for w in (left, right):
            wheel_centers[w] = np.array([np.sign(wheel_centers[w][0]) * half_track, y, z])
    return wheel_centers


def build_primitives(meshes, wheel_centers, z_mid, builder):
    """({group: {material name: Primitive}}, {wheel: tire positions}) in glTF space:
    body around the axle midpoint, wheel parts around their wheel center."""
    groups, tire_extent = {}, {}
    for group, gmt in meshes:
        center = [0.0, 0.0, z_mid] if group == "body" else wheel_centers[group[1]]
        pos = (gmt.pos + (gmt.pivot - center)) * GMT_TO_GLTF
        add_mesh_primitives(groups.setdefault(group, {}), gmt, pos, builder)
        if group != "body" and "TIRE" in gmt.name.upper():
            tire_extent[group[1]] = pos
    return groups, tire_extent


def add_mesh_primitives(prims, gmt, pos, builder):
    """Append the mesh's non-degenerate faces to one primitive per material."""
    nrm = gmt.nrm * GMT_TO_GLTF
    tan = gmt.tangent * GMT_TO_GLTF
    btan = gmt.bitangent * GMT_TO_GLTF
    a, b, c = (pos[gmt.tris[:, k]] for k in range(3))
    has_area = np.linalg.norm(np.cross(b - a, c - a), axis=1) > 1e-12
    for mat_index in np.unique(gmt.face_mat):
        mat_name, gmt_textures = gmt.materials[mat_index]
        select = (gmt.face_mat == mat_index) & has_area
        if mat_name in SKIP_MATERIALS or not select.any():
            continue
        mat = builder.get(mat_name, gmt_textures)
        prim = prims.setdefault(mat.name, Primitive(mat))
        prim.add(pos, nrm, gmt.uv, gmt.uv_b, gmt.uv_c, tan, btan, gmt.tris[select])


def build_layout(groups, tire_extent, wheel_centers, z_mid):
    """layout.json in engine space (glTF space with Z negated)."""
    to_engine = np.array([1.0, 1.0, -1.0])
    wheels = {}
    for w in WHEELS:
        tire = tire_extent[w]
        center = (wheel_centers[w] - [0.0, 0.0, z_mid]) * GMT_TO_GLTF * to_engine
        wheels[w] = {"center": rounded(center),
                     "radius": round(float(np.hypot(tire[:, 1], tire[:, 2]).max()), 5),
                     "width": round(float(np.ptp(tire[:, 0])), 5)}
    body_pts = np.concatenate([np.concatenate(p.pos) for p in groups["body"].values()]) * to_engine
    return {
        "wheels": wheels,
        "bodyBounds": {"min": rounded(body_pts.min(axis=0)), "max": rounded(body_pts.max(axis=0))},
        "hullPoints": [rounded(p) for p in hull_points(body_pts)],
    }


def print_summary(out_dir, veh_path, veh, builder, textures, out_files, layout):
    print(f"Output: {out_dir}")
    print(f"Livery: {builder.livery.name} ({veh_path.name}, {veh.get('description', '')})")
    for stem, (verts, tris, nprims) in out_files.items():
        print(f"  {stem}.gltf: {verts} verts, {tris} tris, {nprims} materials")
    for w in WHEELS:
        print(f"  wheel {w}: {layout['wheels'][w]}")
    print(f"  bodyBounds {layout['bodyBounds']}, {len(layout['hullPoints'])} hull points")
    print(f"  {len(textures.written)} textures")
    print("Materials:")
    for line in builder.report:
        print("  " + line)


if __name__ == "__main__":
    main()
