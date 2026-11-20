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

Requires numpy and Pillow (DDS decoding); compression uses Tools/texconv.exe.
"""

import argparse
import json
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_OUT = REPO_ROOT / "Projects" / "Showcase" / "Assets" / "Vehicles" / "Porsche911GT3R"
TEXCONV = REPO_ROOT / "Tools" / "texconv.exe"

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

# Base colors of shared tiling textures (UV-scaled detail layers that cannot
# be expressed with one UV set); textures present in the archive use their mean.
TILE_FALLBACK = {
    "_CARBON10_ALB": (0.030, 0.030, 0.033),
    "_METAL03_ALB": (0.45, 0.45, 0.46),
    "_PLASTIC02_ALB": (0.035, 0.035, 0.035),
    "_RUBBER01_ALB": (0.025, 0.025, 0.025),
    "_DEFTEX_ALB_BC7": (0.80, 0.80, 0.80),
}

FINISH_ROUGHNESS = {"Shiny": 0.15, "Medium": 0.32, "Matte": 0.62}
DEFAULT_MR = (0.0, 0.5)  # _DEFTEX_MR_BC5: metallic, roughness

DXGI_TEXCONV = {"BC7": "BC7_UNORM", "BC7_SRGB": "BC7_UNORM_SRGB", "BC5U": "BC5_UNORM",
                "DXT1": "BC1_UNORM", "DXT5": "BC3_UNORM", "BC1": "BC1_UNORM",
                "BC1_SRGB": "BC1_UNORM_SRGB", "BC3": "BC3_UNORM", "BC3_SRGB": "BC3_UNORM_SRGB"}
DXGI_NAMES = {71: "BC1", 72: "BC1_SRGB", 74: "BC2", 77: "BC3", 78: "BC3_SRGB", 80: "BC4",
              83: "BC5U", 95: "BC6H", 98: "BC7", 99: "BC7_SRGB", 28: "RGBA8", 29: "RGBA8_SRGB",
              87: "BGRA8"}


# --------------------------------------------------------------------------- input files

class RawDir:
    """Case-insensitive file lookup in the extracted archive directory."""

    def __init__(self, root):
        self.root = Path(root)
        self.files = {p.name.lower(): p for p in self.root.iterdir() if p.is_file()}

    def find(self, name, exts=("",)):
        key = name.lower()
        for ext in exts:
            if key + ext in self.files:
                return self.files[key + ext]
        return None

    def texture(self, stem):
        stem = stem.lstrip(":")
        stem = re.sub(r"\.(dds|tga|bmp)$", "", stem, flags=re.I)
        return self.find(stem, (".dds",))

    def json(self, name):
        path = self.find(name, (".json",))
        return json.loads(path.read_text(encoding="utf-8")) if path else None


def strip_comment(line):
    return line.split("//", 1)[0].strip()


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


# --------------------------------------------------------------------------- GMT

FACE_DTYPE = np.dtype([("i", "<u2", 3), ("mesh", "<u2"), ("mat", "<u4"), ("pad", "<u4"),
                       ("cen", "<f4", 3), ("pad2", "<u4")])
MATERIAL_HEADER = 0x104
TEXTURE_RECORD = 0xB4


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


class Gmt:
    """GMOTOR_GMT_2.5x mesh (layout: lmu-gamerip docs/FORMAT.md section 3).

    The vertex stride is chosen by the per-face centroid each face record stores,
    so a parse is validated against the file itself."""

    def __init__(self, path):
        d = path.read_bytes()
        u32 = lambda o: struct.unpack_from("<I", d, o)[0]
        self.name = path.name
        vcount, mcount, fcount, kcount = u32(0x13C), u32(0x14C), u32(0x154), u32(0x15C)
        matcount = u32(0x164)
        self.pivot = np.array(struct.unpack_from("<3f", d, 0x114), dtype=np.float64)
        skip = 10 if u32(0x158) > u32(0x140) else 0
        face_start = 0x378 + skip + mcount * 156
        faces = np.frombuffer(d, FACE_DTYPE, fcount, face_start)
        vert_start = face_start + fcount * FACE_DTYPE.itemsize + kcount * 16 + matcount * 240
        self.tris = faces["i"].astype(np.int64)
        err, self.stride, raw, self.pos = self._best_vertex_stride(d, vert_start, vcount, self.tris, faces["cen"])
        if not err < 5e-3:  # stored centroids carry a little authoring noise
            raise ValueError(f"{path.name}: no vertex stride reproduces the face centroids (err {err})")
        self.nrm = raw[:, 12:24].copy().view("<f4").astype(np.float64)
        self.uv = raw[:, 64:72].copy().view("<f4").astype(np.float64)
        self.materials = self._materials(d)
        self.face_mat = self._face_materials(faces["mat"])

    @staticmethod
    def _best_vertex_stride(d, vert_start, vcount, tris, centroids):
        """(centroid error, stride, raw vertex bytes, positions) of the candidate
        stride whose positions best reproduce the stored face centroids."""
        best = None
        for stride in (96, 128, 112, 104, 120, 136, 144):
            if vert_start + vcount * stride > len(d):
                continue
            raw = np.frombuffer(d, np.uint8, vcount * stride, vert_start).reshape(vcount, stride)
            with np.errstate(invalid="ignore", over="ignore"):  # wrong strides read garbage floats
                pos = raw[:, 0:12].copy().view("<f4").astype(np.float64)
            err = centroid_error(pos, tris, centroids)
            if best is None or err < best[0]:
                best = (err, stride, raw, pos)
        return best

    def _face_materials(self, face_mat):
        # Some single-material meshes (e.g. 488 GTE belt, rims) keep per-group
        # material ids on their faces that all resolve to the one material.
        if len(self.materials) == 1:
            return np.zeros_like(face_mat, dtype=np.int64)
        face_mat = face_mat.astype(np.int64)
        if len(face_mat) and face_mat.max() >= len(self.materials):
            raise ValueError(f"{self.name}: face material {face_mat.max()} "
                             f"outside the {len(self.materials)}-entry material block")
        return face_mat

    @staticmethod
    def _materials(d):
        """[(name, [texture names])] from the material block: u32 count, then per
        material a 0x104-byte header (texture count at +0x100) and 0xB4 per texture.
        Some files carry a short trailer after the block."""
        n = len(d)
        base = max(0, n - 400000)
        fallback = None
        for m in re.finditer(rb"[\x01-\x40]\x00\x00\x00[A-Za-z0-9_?]", d[base:]):
            records, end = read_material_records(d, base + m.start())
            if records is None:
                continue
            if end == n:
                return records
            if 0 <= n - end <= 0x200 and fallback is None:
                fallback = records
        if fallback is None:
            raise ValueError("material block not found")
        return fallback


def centroid_error(pos, tris, centroids):
    """Largest per-axis difference between the stored face centroids and those of
    `pos` (infinite when a wrong stride yields non-finite floats)."""
    if len(tris) == 0:
        return 0.0
    with np.errstate(invalid="ignore", over="ignore"):
        err = np.abs(pos[tris].mean(axis=1) - centroids).max()
    return err if np.isfinite(err) else np.inf


def read_material_records(d, offset):
    """(records, end offset) of a candidate material block at `offset`, or
    (None, None) when its declared count of records does not parse."""
    count = struct.unpack_from("<I", d, offset)[0]
    p, records = offset + 4, []
    for _ in range(count):
        if p + MATERIAL_HEADER > len(d):
            return None, None
        ntex = struct.unpack_from("<I", d, p + 0x100)[0]
        if ntex > 32:
            return None, None
        texs = [cstr(d[p + MATERIAL_HEADER + k * TEXTURE_RECORD:][:64]) for k in range(ntex)]
        records.append((cstr(d[p:p + 64]), texs))
        p += MATERIAL_HEADER + ntex * TEXTURE_RECORD
    return records, p


# --------------------------------------------------------------------------- textures

def dds_info(path):
    d = path.read_bytes()[:148]
    height, width = struct.unpack_from("<II", d, 12)
    mips = max(1, struct.unpack_from("<I", d, 28)[0])
    fourcc = d[84:88]
    if fourcc == b"DX10":
        fmt = DXGI_NAMES.get(struct.unpack_from("<I", d, 128)[0], "?")
    else:
        fmt = {b"DXT1": "DXT1", b"DXT5": "DXT5", b"ATI2": "BC5U", b"BC5U": "BC5U"}.get(fourcc, "RGBA8")
    return width, height, mips, fmt


def load_rgba(path):
    image = Image.open(path)
    image.load()
    return np.asarray(image.convert("RGBA")).astype(np.float32) / 255.0


def binary_alpha(path):
    """True when an albedo's alpha is a cutout (nearly every texel opaque or
    clear), which renders correctly as MASK without sorting."""
    alpha = load_rgba(path)[..., 3]
    return float(((alpha < 0.1) | (alpha > 0.9)).mean()) > 0.97


def linear_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def write_solid_dds(path, rgba8, size=4):
    """Uncompressed DDS (DXGI_FORMAT_R8G8B8A8_UNORM masks), single mip."""
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    pixel_format = struct.pack("<8I", 32, 0x41, 0, 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000)
    header = struct.pack("<7I", 124, flags, size, size, size * 4, 0, 1)
    header += b"\0" * 44 + pixel_format + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    path.write_bytes(b"DDS " + header + bytes(rgba8) * (size * size))


def adjust(value, amount):
    """LMU Material_Adjust: 0.5 keeps the map value, 0 drives it to 0, 1 to 1."""
    if amount <= 0.5:
        return value * (amount / 0.5)
    return value + (1.0 - value) * ((amount - 0.5) / 0.5)


class TextureWriter:
    def __init__(self, out_dir, max_size):
        self.dir = out_dir / "textures"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.max_size = max_size
        self.written = set()
        self.tmp = Path(tempfile.mkdtemp(prefix="lmu_import_"))

    def _texconv(self, src, fmt, width, height):
        cmd = [str(TEXCONV), "-nologo", "-y", "-m", "0", "-f", fmt, "-w", str(width),
               "-h", str(height), "-o", str(self.dir), str(src)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"texconv failed for {src}:\n{result.stdout}{result.stderr}")

    def _fit(self, width, height):
        scale = min(1.0, self.max_size / max(width, height))
        return max(4, int(width * scale)), max(4, int(height * scale))

    def copy(self, src):
        """Source DDS as-is (or downscaled to max_size in its own format)."""
        name = src.stem.lower() + ".dds"
        if name not in self.written:
            width, height, mips, fmt = dds_info(src)
            w, h = self._fit(width, height)
            if (w, h) == (width, height) and mips > 1:
                shutil.copyfile(src, self.dir / name)
            else:
                staged = self.tmp / name
                shutil.copyfile(src, staged)
                self._texconv(staged, DXGI_TEXCONV[fmt], w, h)
            self.written.add(name)
        return "textures/" + name

    def encode(self, name, rgba, fmt="BC7_UNORM"):
        """Float RGBA image -> compressed DDS with mips."""
        name = name.lower()
        png = self.tmp / (Path(name).stem + ".png")
        Image.fromarray((np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(png)
        w, h = self._fit(rgba.shape[1], rgba.shape[0])
        self._texconv(png, fmt, w, h)
        self.written.add(name)
        return "textures/" + name

    def solid(self, name, rgba8):
        name = name.lower()
        write_solid_dds(self.dir / name, rgba8)
        self.written.add(name)
        return "textures/" + name

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


# --------------------------------------------------------------------------- materials

class Material:
    def __init__(self, name):
        self.name = name
        self.base_color = None   # texture uri
        self.normal = None
        self.metal_rough = None
        self.alpha_mode = "OPAQUE"
        self.alpha_cutoff = 0.5


class LmuMaterialDefinition:
    def __init__(self, definition, raw):
        self.definition = definition
        self.raw = raw
        cb = (definition.get("parameters") or {}).get("cbParameters") or {}
        self.adjust_values = cb.get("Material_Adjust") or {}
        self.uv_scale = cb.get("UV_Scale") or {}
        self.uv_flags = definition.get("preprocessorFlags") or {}
        self.stages = {t.get("Name"): t.get("Texture") for t in definition.get("textures") or []
                       if t.get("Texture") and not t.get("inactive")}
        self.shader = definition.get("shader", "")
        self.is_glass = self.shader.startswith("L2WINDSCREEN")

    def texture(self, stage, scale_key, uv_key):
        """(stem, path) of a stage's texture; path is None when the file is
        missing or the stage is tiled / on the second UV set."""
        stem = self.stages.get(stage)
        if not stem:
            return None, None
        path = self.raw.texture(stem)
        usable = path and self._untiled(scale_key) and self._on_first_uv_set(uv_key)
        return stem, path if usable else None

    def _untiled(self, scale_key):
        return self.uv_scale.get(scale_key, [1.0, 1.0]) == [1.0, 1.0]

    def _on_first_uv_set(self, uv_key):
        return self.uv_flags.get(uv_key, "UV_A") in ("UV_A", "@Undef")

    def albedo(self):
        stem, path = self.texture("albedoMap", "Albedo", "ALBEDOUV")
        if stem is None:
            stem, path = self.texture("sTex0", "Albedo", "ALBEDOUV")
        return stem, path

    def adjust_amount(self, key):
        return self.adjust_values.get(key, 0.5)

    def alpha_mode(self, albedo, default):
        """Glass and sorted alpha blend; other alpha-blended materials, and sorted
        ones whose albedo alpha is a cutout, render as MASK."""
        if self.is_glass:
            return "BLEND"
        if self.definition.get("srcBlend") != "CUBEAB_SRCALPHA":
            return default
        sorted_alpha = "CUBEMAT_SORTEDALPHA" in (self.definition.get("flags") or [])
        if sorted_alpha and not (albedo and binary_alpha(albedo)):
            return "BLEND"
        return "MASK"


class MaterialBuilder:
    def __init__(self, raw, textures, veh):
        self.raw = raw
        self.textures = textures
        self.livery = raw.texture(veh["defaultlivery"])
        if self.livery is None:
            raise FileNotFoundError(f"livery {veh['defaultlivery']} not found")
        self.livery_stem = self.livery.stem
        self.extras = {k: v for k, v in veh.items() if re.fullmatch(r"extra\d+", k)}
        self.cache = {}
        self.report = []

    def get(self, name, gmt_textures):
        if name not in self.cache:
            self.cache[name] = self._build(name, gmt_textures)
        return self.cache[name]

    def _solid_color(self, mat, rgb_linear):
        rgba8 = list((linear_to_srgb(rgb_linear) * 255 + 0.5).astype(int)) + [255]
        mat.base_color = self.textures.solid(f"{mat.name}_basecolor.dds", rgba8)

    def _solid_mr(self, mat, roughness, metallic):
        rgba8 = [255, int(roughness * 255 + 0.5), int(metallic * 255 + 0.5), 255]
        mat.metal_rough = self.textures.solid(f"{mat.name}_mr.dds", rgba8)

    def _repack_mr(self, mat, src, metal_adjust, rough_adjust):
        """LMU metalRough (R metallic, G roughness) -> glTF (G roughness, B metallic)."""
        lmu = load_rgba(src)
        metal = adjust(lmu[..., 0], metal_adjust)
        rough = adjust(lmu[..., 1], rough_adjust)
        packed = np.stack([np.ones_like(metal), rough, metal, np.ones_like(metal)], axis=-1)
        mat.metal_rough = self.textures.encode(f"{mat.name}_mr.dds", packed)

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

    def _build(self, name, gmt_textures):
        mat = Material(re.sub(r"[^A-Za-z0-9_]", "_", name).lower())
        if name == "WCCARBODY":
            self._build_livery(mat)
        elif name in CONSTANT_MATERIALS:
            rgb, roughness, metallic = CONSTANT_MATERIALS[name]
            self._solid_color(mat, rgb)
            self._solid_mr(mat, roughness, metallic)
            self.report.append(f"{name}: constant")
        else:
            definition = self.raw.json(name)
            if definition:
                self._build_from_json(mat, name, definition)
            else:
                self._build_from_gmt(mat, name, gmt_textures)
        return mat

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

    def _build_from_json(self, mat, name, definition):
        lmu = LmuMaterialDefinition(definition, self.raw)
        albedo = self._json_base_color(mat, name, lmu)
        self._json_normal(mat, lmu)
        mr = self._json_metal_rough(mat, lmu)
        mat.alpha_mode = lmu.alpha_mode(albedo, mat.alpha_mode)
        self.report.append(f"{name}: {lmu.shader} base={'tex' if albedo else 'const'} "
                           f"normal={'tex' if mat.normal else '-'} mr={'tex' if mr else 'const'} "
                           f"{mat.alpha_mode}")

    def _json_base_color(self, mat, name, lmu):
        """Livery override, else the albedo map; tiled or missing maps become
        their mean color. Returns the texture used, if any."""
        albedo_stem, albedo = lmu.albedo()
        albedo = self._override_texture(name) or albedo
        if albedo:
            mat.base_color = self.textures.copy(albedo)
        else:
            self._solid_color(mat, self._mean_color(albedo_stem))
        return albedo

    def _json_normal(self, mat, lmu):
        _, normal = lmu.texture("normalMap", "Normal", "NORMALUV")
        if normal and not normal.stem.upper().startswith("_DEFTEX"):
            mat.normal = self.textures.copy(normal)

    def _json_metal_rough(self, mat, lmu):
        """Glass is smooth dielectric; otherwise the repacked metalRough map, or
        its mean (default when missing). Returns the map used, if any."""
        metal_adjust = lmu.adjust_amount("Metallic")
        rough_adjust = lmu.adjust_amount("Roughness")
        mr_stem, mr = lmu.texture("metalRoughMap", "MetalRough", "METALROUGHUV")
        if lmu.is_glass:
            self._solid_mr(mat, 0.05, 0.0)
            return mr
        if mr:
            self._repack_mr(mat, mr, metal_adjust, rough_adjust)
            return mr
        metallic, roughness = self._mean_metal_rough(mr_stem)
        self._solid_mr(mat, adjust(roughness, rough_adjust), adjust(metallic, metal_adjust))
        return None

    def _mean_metal_rough(self, stem):
        path = self.raw.texture(stem) if stem else None
        if path is None:
            return DEFAULT_MR
        mean = load_rgba(path).reshape(-1, 4).mean(axis=0)
        return float(mean[0]), float(mean[1])

    def _build_from_gmt(self, mat, name, gmt_textures):
        albedo = self._override_texture(name)
        if albedo is None:
            for stem in gmt_textures:
                if not stem.upper().startswith("_DEFTEX") and self.raw.texture(stem):
                    albedo = self.raw.texture(stem)
                    break
        if albedo:
            mat.base_color = self.textures.copy(albedo)
        else:
            self._solid_color(mat, TILE_FALLBACK["_DEFTEX_ALB_BC7"])
        self._solid_mr(mat, 0.5, 0.0)
        self.report.append(f"{name}: no material JSON, base={albedo.name if albedo else 'const'}")

    def _mean_color(self, stem):
        if stem:
            path = self.raw.texture(stem)
            if path:
                return srgb_to_linear(load_rgba(path)[..., :3].reshape(-1, 3).mean(axis=0))
            if stem.upper() in TILE_FALLBACK:
                return TILE_FALLBACK[stem.upper()]
        return TILE_FALLBACK["_DEFTEX_ALB_BC7"]


def paint_finish(preset):
    """(roughness, metallic) of an LMU paint preset name such as vinylHeavyMatte,
    carPaintMetallicMedium or carbonFibreShinyStrong."""
    roughness = 0.15 if preset == "carPaint" else 0.32
    for finish, value in FINISH_ROUGHNESS.items():
        if finish in preset:
            roughness = value
    metallic = 0.6 if preset.startswith("carPaintMetallic") else 0.0
    return roughness, metallic


# --------------------------------------------------------------------------- glTF

class Primitive:
    def __init__(self, material):
        self.material = material
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []
        self.count = 0

    def add(self, pos, nrm, uv, tris):
        used, remap = np.unique(tris, return_inverse=True)
        self.pos.append(pos[used])
        self.nrm.append(nrm[used])
        self.uv.append(uv[used])
        self.idx.append(remap.reshape(-1, 3) + self.count)
        self.count += len(used)


def write_gltf(path, name, primitives):
    bin_path = path.with_suffix(".bin")
    blob = bytearray()
    views, accessors, gl_prims = [], [], []
    materials, textures, images = [], [], []
    image_index, material_index = {}, {}

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(views) - 1

    def add_accessor(array, kind, component, target, with_bounds=False):
        view = add_view(array.tobytes(), target)
        accessor = {"bufferView": view, "componentType": component, "count": len(array), "type": kind}
        if with_bounds:
            accessor["min"] = array.min(axis=0).tolist()
            accessor["max"] = array.max(axis=0).tolist()
        accessors.append(accessor)
        return len(accessors) - 1

    def texture(uri):
        if uri not in image_index:
            images.append({"uri": uri, "mimeType": "image/vnd-ms.dds"})
            textures.append({"source": len(images) - 1,
                             "extensions": {"MSFT_texture_dds": {"source": len(images) - 1}}})
            image_index[uri] = len(textures) - 1
        return {"index": image_index[uri]}

    def material(mat):
        if mat.name not in material_index:
            pbr = {"metallicFactor": 1.0, "roughnessFactor": 1.0}
            if mat.base_color:
                pbr["baseColorTexture"] = texture(mat.base_color)
            if mat.metal_rough:
                pbr["metallicRoughnessTexture"] = texture(mat.metal_rough)
            entry = {"name": mat.name, "pbrMetallicRoughness": pbr, "alphaMode": mat.alpha_mode}
            if mat.normal:
                entry["normalTexture"] = texture(mat.normal)
            if mat.alpha_mode == "MASK":
                entry["alphaCutoff"] = mat.alpha_cutoff
            if mat.alpha_mode != "OPAQUE":
                entry["doubleSided"] = True
            materials.append(entry)
            material_index[mat.name] = len(materials) - 1
        return material_index[mat.name]

    for prim in sorted(primitives, key=lambda p: (p.material.alpha_mode != "OPAQUE", p.material.name)):
        pos = np.concatenate(prim.pos).astype("<f4")
        nrm = np.concatenate(prim.nrm)
        lengths = np.linalg.norm(nrm, axis=1, keepdims=True)
        nrm = np.where(lengths > 1e-8, nrm / np.maximum(lengths, 1e-8), [0.0, 1.0, 0.0]).astype("<f4")
        uv = np.concatenate(prim.uv).astype("<f4")
        idx = np.concatenate(prim.idx).astype("<u4").reshape(-1)
        gl_prims.append({
            "attributes": {
                "POSITION": add_accessor(pos, "VEC3", 5126, 34962, True),
                "NORMAL": add_accessor(nrm, "VEC3", 5126, 34962),
                "TEXCOORD_0": add_accessor(uv, "VEC2", 5126, 34962),
            },
            "indices": add_accessor(idx, "SCALAR", 5125, 34963),
            "material": material(prim.material),
        })

    doc = {
        "asset": {"version": "2.0", "generator": "FoxEngine Tools/import_lmu_vehicle.py"},
        "extensionsUsed": ["MSFT_texture_dds"],
        "extensionsRequired": ["MSFT_texture_dds"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": gl_prims}],
        "materials": materials,
        "textures": textures,
        "images": images,
        "buffers": [{"uri": bin_path.name, "byteLength": len(blob)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    bin_path.write_bytes(bytes(blob))
    path.write_text(json.dumps(doc, indent=1), encoding="ascii")
    tris = sum(len(np.concatenate(p.idx)) for p in primitives)
    verts = sum(p.count for p in primitives)
    return verts, tris


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


def rounded(values):
    return [round(float(v), 5) for v in values]


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
    builder = MaterialBuilder(raw, textures, veh)

    meshes = load_meshes(raw, parts)
    wheel_centers = symmetric_wheel_centers(meshes)
    z_mid = 0.5 * (wheel_centers["fl"][2] + wheel_centers["rl"][2])
    groups, tire_extent = build_primitives(meshes, wheel_centers, z_mid, builder)

    out_files = {}
    for group, prims in sorted(groups.items(), key=lambda kv: str(kv[0])):
        stem = "body" if group == "body" else f"{group[0]}_{group[1]}"
        verts, tris = write_gltf(args.out / f"{stem}.gltf", stem, list(prims.values()))
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
    a, b, c = (pos[gmt.tris[:, k]] for k in range(3))
    has_area = np.linalg.norm(np.cross(b - a, c - a), axis=1) > 1e-12
    for mat_index in np.unique(gmt.face_mat):
        mat_name, gmt_textures = gmt.materials[mat_index]
        select = (gmt.face_mat == mat_index) & has_area
        if mat_name in SKIP_MATERIALS or not select.any():
            continue
        mat = builder.get(mat_name, gmt_textures)
        prim = prims.setdefault(mat.name, Primitive(mat))
        prim.add(pos, nrm, gmt.uv, gmt.tris[select])


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
