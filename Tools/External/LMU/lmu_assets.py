"""Shared readers and writers for the Le Mans Ultimate (rFactor 2 gMotor) importers.

Used by import_lmu_vehicle.py and import_lmu_track.py: case-insensitive lookup
in an extracted archive directory, GMT meshes, LMU material JSON baked to glTF
2.0 metallic-roughness, BC-compressed DDS output and the glTF writer.

Requires numpy and Pillow (DDS decoding); compression uses Tools/texconv.exe.
"""

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
TEXCONV = REPO_ROOT / "Tools" / "texconv.exe"

# Base colors of shared tiling textures (UV-scaled detail layers that cannot
# be expressed with one UV set); textures present in the archive use their mean.
TILE_FALLBACK = {
    "_CARBON10_ALB": (0.030, 0.030, 0.033),
    "_METAL03_ALB": (0.45, 0.45, 0.46),
    "_PLASTIC02_ALB": (0.035, 0.035, 0.035),
    "_RUBBER01_ALB": (0.025, 0.025, 0.025),
    "_DEFTEX_ALB_BC7": (0.80, 0.80, 0.80),
}

DEFAULT_MR = (0.0, 0.5)  # _DEFTEX_MR_BC5: metallic, roughness

DXGI_TEXCONV = {"BC7": "BC7_UNORM", "BC7_SRGB": "BC7_UNORM_SRGB", "BC5U": "BC5_UNORM",
                "DXT1": "BC1_UNORM", "DXT5": "BC3_UNORM", "BC1": "BC1_UNORM",
                "BC1_SRGB": "BC1_UNORM_SRGB", "BC3": "BC3_UNORM", "BC3_SRGB": "BC3_UNORM_SRGB"}
DXGI_NAMES = {71: "BC1", 72: "BC1_SRGB", 74: "BC2", 77: "BC3", 78: "BC3_SRGB", 80: "BC4",
              83: "BC5U", 95: "BC6H", 98: "BC7", 99: "BC7_SRGB", 28: "RGBA8", 29: "RGBA8_SRGB",
              87: "BGRA8"}


# --------------------------------------------------------------------------- input files

class RawDir:
    """Case-insensitive file lookup in the extracted archive directory, then in
    `shared` directories (e.g. a dump of the game's shared Commonmaps.mas);
    the archive directory wins on name clashes."""

    def __init__(self, root, shared=()):
        self.root = Path(root)
        self.files = {}
        for directory in reversed([self.root, *map(Path, shared)]):
            self.files.update({p.name.lower(): p for p in directory.iterdir() if p.is_file()})

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
    so a parse is validated against the file itself. `with_materials=False`
    reads geometry only (collision proxies carry unusable material ids).

    Face indices are 16-bit global vertex numbers, so in meshes over 65536
    vertices they wrap: see _unwrap_indices."""

    def __init__(self, path, with_materials=True):
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
        # Stored centroids carry authoring noise that grows with the mesh size
        # (about 1 cm on track meshes hundreds of metres across).
        extent = float(np.ptp(self.pos, axis=0).max()) if err < np.inf and vcount else 0.0
        if not err < max(5e-3, 1e-3 * extent):
            raise ValueError(f"{path.name}: no vertex stride reproduces the face centroids (err {err})")
        self.nrm = raw[:, 12:24].copy().view("<f4").astype(np.float64)
        self.uv = raw[:, 64:72].copy().view("<f4").astype(np.float64)
        self.tangent = raw[:, 40:52].copy().view("<f4").astype(np.float64)
        self.bitangent = raw[:, 52:64].copy().view("<f4").astype(np.float64)
        self.uv_b = raw[:, 72:80].copy().view("<f4").astype(np.float64)  # LMU UV_B
        self.uv_c = raw[:, 80:88].copy().view("<f4").astype(np.float64)  # LMU UV_C
        if with_materials:
            self.materials = self._materials(d)
            self.face_mat = self._face_materials(faces["mat"])
        else:
            self.materials = [("", [])]
            self.face_mat = np.zeros(fcount, dtype=np.int64)
        if vcount > 65536:
            records = [struct.unpack_from("<10I", d, 0x378 + skip + k * 156) for k in range(mcount)]
            self.tris = self._unwrap_indices(records)

    def _unwrap_indices(self, records):
        """Face indices without the 16-bit wrap. Each mesh record owns a vertex
        range (start at +24, count at +4; the ranges tile the vertex array) and
        every face lies in one record, so its true index is the one congruent
        to the stored index mod 2^16 inside that range. The record is the
        face's material when records and materials correspond one to one (e.g.
        Road Atlanta's Bld_MichelinTower); otherwise the only record the face
        fits, or, when several fit, the one giving the smallest triangle
        (TyreStacks_01). The stored centroids were computed from the wrapped
        indices, so they validate the stride but cannot choose the record."""
        starts = np.array([r[6] for r in records], dtype=np.int64)
        counts = np.array([r[1] for r in records], dtype=np.int64)
        if counts.sum() != len(self.pos) or np.any(starts != np.concatenate([[0], np.cumsum(counts)[:-1]])):
            raise ValueError(f"{self.name}: mesh records do not tile the {len(self.pos)} vertices")
        # unwrapped[r]: the faces' indices unwrapped into record r; fits[r]: all three lie in it.
        unwrapped = starts[:, None, None] + np.mod(self.tris[None] - starts[:, None, None], 65536)
        fits = np.all(unwrapped < (starts + counts)[:, None, None], axis=2)
        if len(self.materials) == len(records):
            choice = self.face_mat
        else:
            p = self.pos[np.minimum(unwrapped, len(self.pos) - 1)]
            edge = np.maximum.reduce([np.linalg.norm(p[:, :, i] - p[:, :, (i + 1) % 3], axis=2) for i in range(3)])
            choice = np.argmin(np.where(fits, edge, np.inf), axis=0)
        faces = np.arange(len(self.tris))
        if not fits[choice, faces].all():
            raise ValueError(f"{self.name}: {int((~fits[choice, faces]).sum())} faces fit no mesh record")
        return unwrapped[choice, faces]

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

class TextureSlot:
    """One glTF material texture reference: path, TEXCOORD set index (0..2) and
    KHR_texture_transform scale. LMU has no UV offsets, so they stay 0."""

    __slots__ = ("path", "uv_set", "scale")

    def __init__(self, path=None, uv_set=0, scale=(1.0, 1.0)):
        self.path = path
        self.uv_set = int(uv_set)
        self.scale = (float(scale[0]), float(scale[1]))

    def __bool__(self):
        return bool(self.path)

    @property
    def tiled(self):
        return any(abs(value - 1.0) > 1e-6 for value in self.scale)


class _Slot:
    """A Material texture slot. Assigning a path string sets the slot's texture
    and keeps its mapping, so builders can rewrite one texture in place."""

    def __set_name__(self, owner, name):
        self.attribute = "_" + name

    def __get__(self, instance, owner=None):
        return instance.__dict__[self.attribute]

    def __set__(self, instance, value):
        if isinstance(value, TextureSlot):
            instance.__dict__[self.attribute] = value
        else:
            instance.__dict__[self.attribute].path = value


SLOTS = ("base_color", "normal", "metal_rough", "occlusion", "detail_albedo", "detail_normal")


class Material:
    base_color = _Slot()
    normal = _Slot()
    metal_rough = _Slot()
    occlusion = _Slot()
    detail_albedo = _Slot()
    detail_normal = _Slot()

    def __init__(self, name):
        self.name = name
        self.base_color = TextureSlot()
        self.normal = TextureSlot()
        self.metal_rough = TextureSlot()
        self.occlusion = TextureSlot()
        self.detail_albedo = TextureSlot()
        self.detail_normal = TextureSlot()
        # glTF extras fox.detailAlbedo.mode/strength: LMU's DETAILALBEDO blend
        # ("overlay" is the engine's default when absent) and the Detail.Albedo
        # weight of the layer.
        self.detail_albedo_mode = "overlay"
        self.detail_albedo_strength = 1.0
        self.alpha_mode = "OPAQUE"
        self.alpha_cutoff = 0.5
        # Texture coordinates of TEXCOORD_0: the mesh's first UV set, or world
        # X/Z for LMU's WorldPlanar mapping; scaled by uv_scale.
        self.uv_source = "UV_A"
        self.uv_scale = (1.0, 1.0)
        # glTF extras fox.vertexTint: "albedoAlphaMask" or "multiply" when the
        # LMU material colours itself from the mesh's COLOR_0 stream.
        self.vertex_tint = None

    def __copy__(self):
        """Copy with independent texture slots (the palette tint rewrites one)."""
        clone = Material(self.name)
        clone.__dict__.update(self.__dict__)
        for name in SLOTS:
            slot = getattr(self, name)
            clone.__dict__["_" + name] = TextureSlot(slot.path, slot.uv_set, slot.scale)
        return clone


# Per role, the LMU stages that can provide it in shader order of preference:
# (texture stage name, UV_Scale key, preprocessor flags naming the stage's UV set,
# flag that defines the stage). LMU writes "@Null" for a flag it defines without a
# value, so only "@Undef" (or a missing flag) means the stage is off. The detail
# stages fall back to DETAILMAP, the flag the clearcoat shader form uses to name
# its detail UV set instead of DETAILUV.
STAGES = {
    "albedo": (("albedoMap", "Albedo", ("ALBEDOUV", "ALBEDOROUGHUV", "ALBEDOMAP"), None),
               ("sTex0", "Albedo", ("ALBEDOUV",), None)),
    "normal": (("normalMap", "Normal", ("NORMALUV", "NORMALMAP"), None),),
    "metal_rough": (("metalRoughMap", "MetalRough", ("METALROUGHUV",), None),
                    ("roughnessMap", "MetalRough", ("ROUGHNESSUV", "ROUGHNESSMAP"), None)),
    "occlusion": (("aoSpecularMap", "aoSpec", ("AOSPECUV",), "AOSPECMAP"),
                  ("aoSpecMap", "aoSpec", ("AOSPECUV",), "AOSPECMAP")),
    "detail_albedo": (("detailMap", "Detail", ("DETAILUV", "DETAILMAP"), "DETAILMAP"),),
    "detail_normal": (("detailNormalMap", "Detail", ("DETAILUV", "DETAILMAP"), "DETAILNORMAL"),),
}
UV_SETS = ("UV_A", "UV_B", "UV_C", "UV_D", "WorldPlanar")
UV_SET_INDEX = {"UV_A": 0, "UV_B": 1, "UV_C": 2}
# LMU DETAILALBEDO shader function -> engine detail albedo blend mode.
DETAIL_MODES = {"Multiply": "multiply", "AlphaBlend": "alphaBlend"}


class LmuMaterialDefinition:
    def __init__(self, definition, raw, report=None):
        self.definition = definition
        self.raw = raw
        self.report = report
        cb = (definition.get("parameters") or {}).get("cbParameters") or {}
        self.adjust_values = cb.get("Material_Adjust") or {}
        self.uv_scale = cb.get("UV_Scale") or {}
        self.detail_values = cb.get("Detail") or {}
        self.uv_flags = definition.get("preprocessorFlags") or {}
        self.stages = {t.get("Name"): t.get("Texture") for t in definition.get("textures") or []
                       if t.get("Texture") and not t.get("inactive")}
        self.shader = definition.get("shader", "")
        self.is_glass = self.shader.startswith(("L2WINDSCREEN", "L2IBLGLASS"))
        # The UV set and scale baked into the output primitive's TEXCOORD_0: set
        # by MaterialBuilder when it bakes the albedo stage's mapping (LMU stages
        # on that set are then divided by it, see slot()).
        self.base_uv_set = "UV_A"
        self.base_uv_scale = (1.0, 1.0)

    def stage(self, role):
        """(stem, uv set, uv scale) of the first active stage providing `role`, or None."""
        for stage, scale_key, uv_keys, flag in STAGES[role]:
            stem = self.stages.get(stage)
            if stem and self._defined(flag):
                return stem, self._uv_set(uv_keys), tuple(self.uv_scale.get(scale_key, [1.0, 1.0]))
        return None

    def _defined(self, flag):
        """A stage with no defining flag is active; LMU stores "@Null" for a flag
        it defines without a value and "@Undef" for an undefined one."""
        return flag is None or self.uv_flags.get(flag) not in (None, "@Undef")

    def _uv_set(self, uv_keys):
        for key in uv_keys:
            value = self.uv_flags.get(key)
            if value in UV_SETS:
                return value
        return "UV_A"

    @property
    def detail_albedo_strength(self):
        """LMU Detail.Albedo weight of the detail albedo layer."""
        return float(self.detail_values.get("Albedo", 1.0))

    def detail_albedo(self):
        """(TextureSlot, engine blend mode) of the detail albedo layer, or
        (None, None). DETAILALBEDO names the shader's blend: Multiply multiplies
        the albedo, AlphaBlend composites the detail as a decal over it."""
        value = self.uv_flags.get("DETAILALBEDO")
        mode = DETAIL_MODES.get(value)
        if mode is not None:
            return self.slot("detail_albedo"), mode
        stem = self.stages.get("detailMap")
        if stem:
            name = self.definition.get("name") or stem
            self._log(f"{name}: dropped detail albedo (DETAILALBEDO={value or '@Undef'})")
        return None, None

    def slot(self, role):
        """TextureSlot of a role's stage on the output UV streams, or None when
        the stage is absent, its texture is missing or its mapping cannot be
        expressed on the primitives' UVs.

        TEXCOORD_0 carries the baked albedo mapping, so a stage on that set is
        divided by its UV_Scale; UV_B/UV_C stages use their own stream as stored."""
        found = self.stage(role)
        if found is None:
            return None
        stem, uv_set, scale = found
        path = self.raw.texture(stem)
        if path is None:
            return None
        if uv_set not in UV_SET_INDEX:
            # UV_D has no stream; WorldPlanar would need world X/Z on the stage's
            # own UV set, which the primitives only carry on TEXCOORD_0.
            self._drop(role, stem, f"{uv_set} has no TEXCOORD set")
            return None
        if self.base_uv_set == "WorldPlanar" and uv_set == "UV_A":
            self._drop(role, stem, "TEXCOORD_0 carries world X/Z for this material")
            return None
        if uv_set == self.base_uv_set:
            uv_set = "UV_A"
            scale = tuple(scale[i] / (self.base_uv_scale[i] or 1.0) for i in (0, 1))
        return TextureSlot(path, UV_SET_INDEX[uv_set], scale)

    def _log(self, message):
        if self.report is not None:
            self.report.append(message)

    def _drop(self, role, stem, reason):
        name = self.definition.get("name") or stem
        self._log(f"{name}: dropped {role} texture '{stem}' ({reason})")

    def texture(self, role):
        """(stem, source path) of a role's texture on the output UV streams; the
        path is None when the stage is missing or cannot be mapped."""
        found = self.stage(role)
        if found is None:
            return None, None
        slot = self.slot(role)
        return found[0], slot.path if slot else None

    def albedo(self):
        return self.texture("albedo")

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
    """LMU material JSON (or the GMT's own texture list) -> glTF material.

    `constants`: name -> (linear base color, roughness, metallic) for shared
    textures that are not in the archives or shaders with nothing worth keeping.
    `bake_albedo_mapping`: when the albedo stage is on UV_A or WorldPlanar, carry
    its mapping in the primitive's UVs (Material.uv_source, uv_scale) instead of
    emitting a texture transform; every other stage is emitted with its own
    TEXCOORD set and KHR_texture_transform."""

    def __init__(self, raw, textures, constants=None, bake_albedo_mapping=False):
        self.raw = raw
        self.textures = textures
        self.constants = constants or {}
        self.bake_albedo_mapping = bake_albedo_mapping
        self.cache = {}
        self.report = []

    def get(self, name, gmt_textures):
        if name not in self.cache:
            self.cache[name] = self._build(name, gmt_textures)
        return self.cache[name]

    def _solid_color(self, mat, rgb_linear):
        rgba8 = list((linear_to_srgb(rgb_linear) * 255 + 0.5).astype(int)) + [255]
        mat.base_color = TextureSlot(self.textures.solid(f"{mat.name}_basecolor.dds", rgba8))

    def _solid_mr(self, mat, roughness, metallic):
        rgba8 = [255, int(roughness * 255 + 0.5), int(metallic * 255 + 0.5), 255]
        mat.metal_rough = TextureSlot(self.textures.solid(f"{mat.name}_mr.dds", rgba8))

    def _repack_mr(self, mat, src, metal_adjust, rough_adjust):
        """LMU metalRough (R metallic, G roughness) -> glTF (G roughness, B metallic)."""
        lmu = load_rgba(src)
        metal = adjust(lmu[..., 0], metal_adjust)
        rough = adjust(lmu[..., 1], rough_adjust)
        packed = np.stack([np.ones_like(metal), rough, metal, np.ones_like(metal)], axis=-1)
        mat.metal_rough = TextureSlot(self.textures.encode(f"{mat.name}_mr.dds", packed))

    def _copy_slot(self, mat, attribute, slot):
        """Stores a stage slot's texture, TEXCOORD set and tiling on the material."""
        setattr(mat, attribute, TextureSlot(self.textures.copy(slot.path), slot.uv_set, slot.scale))

    def _override_texture(self, name):
        """Replacement base color for a named material (vehicle liveries)."""
        return None

    def _build_special(self, mat, name):
        """Builds a material the JSON cannot describe; False when `name` is not one."""
        return False

    def _build(self, name, gmt_textures):
        mat = Material(material_slug(name))
        if self._build_special(mat, name):
            return mat
        if name in self.constants:
            rgb, roughness, metallic = self.constants[name]
            self._solid_color(mat, rgb)
            self._solid_mr(mat, roughness, metallic)
            self.report.append(f"{name}: constant")
            return mat
        definition = self.raw.json(name)
        if definition:
            self._build_from_json(mat, name, definition)
        else:
            self._build_from_gmt(mat, name, gmt_textures)
        return mat

    def _build_from_json(self, mat, name, definition):
        lmu = LmuMaterialDefinition(definition, self.raw, self.report)
        albedo_stage = lmu.stage("albedo")
        if self.bake_albedo_mapping and albedo_stage and albedo_stage[1] in ("UV_A", "WorldPlanar"):
            lmu.base_uv_set = mat.uv_source = albedo_stage[1]
            lmu.base_uv_scale = mat.uv_scale = albedo_stage[2]
        albedo = self._json_base_color(mat, name, lmu)
        self._json_normal(mat, lmu)
        mr = self._json_metal_rough(mat, lmu)
        self._json_occlusion(mat, lmu)
        self._json_detail(mat, lmu, name)
        mat.alpha_mode = self._alpha_mode(lmu, albedo)
        mapping = "" if (mat.uv_source, mat.uv_scale) == ("UV_A", (1.0, 1.0)) else \
            f" {mat.uv_source}*{list(mat.uv_scale)}"
        detail = [label for label, attribute in (("alb", "detail_albedo"), ("nrm", "detail_normal"))
                  if getattr(mat, attribute)]
        self.report.append(f"{name}: {lmu.shader} base={'tex' if albedo else 'const'} "
                           f"normal={'tex' if mat.normal else '-'} mr={'tex' if mr else 'const'} "
                           f"ao={'tex' if mat.occlusion else '-'} detail={','.join(detail) or '-'} "
                           f"{mat.alpha_mode}{mapping}")

    def _alpha_mode(self, lmu, albedo):
        return lmu.alpha_mode(albedo, "OPAQUE")

    def _json_base_color(self, mat, name, lmu):
        """Override, else the albedo map with its mapping, else its mean color.
        Returns the texture used, if any."""
        override = self._override_texture(name)
        if override:
            mat.base_color = TextureSlot(self.textures.copy(override))
            return override
        slot = lmu.slot("albedo")
        if slot:
            self._copy_slot(mat, "base_color", slot)
            return slot.path
        found = lmu.stage("albedo")
        self._solid_color(mat, self._mean_color(found[0] if found else None))
        return None

    def _json_normal(self, mat, lmu):
        slot = lmu.slot("normal")
        if slot and not slot.path.stem.upper().startswith("_DEFTEX"):
            self._copy_slot(mat, "normal", slot)

    def _json_metal_rough(self, mat, lmu):
        """Glass is smooth dielectric; otherwise the repacked metalRough map, or
        its mean (default when missing). Returns the map used, if any."""
        metal_adjust = lmu.adjust_amount("Metallic")
        rough_adjust = lmu.adjust_amount("Roughness")
        slot = lmu.slot("metal_rough")
        if lmu.is_glass:
            self._solid_mr(mat, 0.05, 0.0)
            return slot.path if slot else None
        if slot:
            self._repack_mr(mat, slot.path, metal_adjust, rough_adjust)
            mat.metal_rough.uv_set, mat.metal_rough.scale = slot.uv_set, slot.scale
            return slot.path
        found = lmu.stage("metal_rough")
        metallic, roughness = self._mean_metal_rough(found[0] if found else None)
        self._solid_mr(mat, adjust(roughness, rough_adjust), adjust(metallic, metal_adjust))
        return None

    def _json_occlusion(self, mat, lmu):
        slot = lmu.slot("occlusion")
        if slot:
            self._copy_slot(mat, "occlusion", slot)

    def _json_detail(self, mat, lmu, name):
        self._json_detail_albedo(mat, lmu, name)
        slot = lmu.slot("detail_normal")
        if slot:
            self._copy_slot(mat, "detail_normal", slot)

    def _json_detail_albedo(self, mat, lmu, name):
        """Detail albedo layer; `name` lets importers that cannot use the stage
        for a material skip it (see the track importer's palette tint)."""
        slot, mode = lmu.detail_albedo()
        if slot:
            self._copy_slot(mat, "detail_albedo", slot)
            mat.detail_albedo_mode = mode
            mat.detail_albedo_strength = lmu.detail_albedo_strength

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


# --------------------------------------------------------------------------- glTF

class Primitive:
    def __init__(self, material):
        self.material = material
        self.pos, self.nrm, self.uv, self.uv_b, self.uv_c = [], [], [], [], []
        self.tan, self.btan, self.idx, self.color = [], [], [], []
        # Terrain bake input: (n, 2) world X/Z per chunk, else None.
        self.world = []
        self.count = 0

    def add(self, pos, nrm, uv, uv_b, uv_c, tangent, bitangent, tris, world=None, color=None):
        """Append a chunk: positions/normals/tangents, UV_A as `uv`, LMU UV_B and
        UV_C (their own glTF streams), the per-vertex linear RGB `color` of the
        material's vertex tint (None = no COLOR_0 stream) and, for terrain
        placeholders, the world X/Z the bake maps the texture through."""
        used, remap = np.unique(tris, return_inverse=True)
        if self.color and (self.color[-1] is None) != (color is None):
            raise ValueError(f"{self.material.name}: mixed vertex-tint chunks in one primitive")
        self.pos.append(pos[used])
        self.nrm.append(nrm[used])
        self.uv.append(uv[used])
        self.uv_b.append(uv_b[used])
        self.uv_c.append(uv_c[used])
        self.tan.append(tangent[used])
        self.btan.append(bitangent[used])
        self.color.append(None if color is None else np.asarray(color, np.float32)[used])
        self.world.append(None if world is None else world[used])
        self.idx.append(remap.reshape(-1, 3) + self.count)
        self.count += len(used)


def write_gltf(path, name, primitives, generator):
    bin_path = path.with_suffix(".bin")
    blob = bytearray()
    views, accessors, gl_prims = [], [], []
    materials, textures, images = [], [], []
    extensions = ["MSFT_texture_dds"]
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

    def texture_info(slot):
        """glTF textureInfo: the slot's TEXCOORD set and, when tiled, the
        KHR_texture_transform that carries the rest of its mapping."""
        info = texture(slot.path)
        if slot.uv_set:
            info["texCoord"] = slot.uv_set
        if slot.tiled:
            transform = {"scale": rounded(slot.scale, 6)}
            if slot.uv_set:
                transform["texCoord"] = slot.uv_set
            info["extensions"] = {"KHR_texture_transform": transform}
            if "KHR_texture_transform" not in extensions:
                extensions.append("KHR_texture_transform")
        return info

    def fox_texture(slot):
        """Material extras texture reference: index, TEXCOORD set, tiling (LMU
        has no UV offsets)."""
        return {"index": texture(slot.path)["index"], "texCoord": slot.uv_set,
                "uvScale": rounded(slot.scale, 6), "uvOffset": [0.0, 0.0]}

    def material(mat):
        if mat.name not in material_index:
            pbr = {"metallicFactor": 1.0, "roughnessFactor": 1.0}
            if mat.base_color:
                pbr["baseColorTexture"] = texture_info(mat.base_color)
            if mat.metal_rough:
                pbr["metallicRoughnessTexture"] = texture_info(mat.metal_rough)
            entry = {"name": mat.name, "pbrMetallicRoughness": pbr, "alphaMode": mat.alpha_mode}
            if mat.normal:
                entry["normalTexture"] = texture_info(mat.normal)
            if mat.occlusion:
                occlusion = texture_info(mat.occlusion)
                occlusion.setdefault("texCoord", 0)
                occlusion["strength"] = 1.0
                entry["occlusionTexture"] = occlusion
            if mat.occlusion or mat.detail_albedo or mat.detail_normal or mat.vertex_tint:
                # LMU AOS maps pack ambient occlusion in R (occlusionChannel 0).
                fox = {"occlusionChannel": 0}
                if mat.detail_albedo:
                    detail = fox_texture(mat.detail_albedo)
                    detail["mode"] = mat.detail_albedo_mode
                    detail["strength"] = round(float(mat.detail_albedo_strength), 6)
                    fox["detailAlbedo"] = detail
                if mat.detail_normal:
                    fox["detailNormal"] = fox_texture(mat.detail_normal)
                if mat.vertex_tint:
                    fox["vertexTint"] = mat.vertex_tint
                entry["extras"] = {"fox": fox}
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
        uv = (np.concatenate(prim.uv) * prim.material.uv_scale).astype("<f4")
        uv_b = np.concatenate(prim.uv_b).astype("<f4")
        uv_c = np.concatenate(prim.uv_c).astype("<f4")
        tan = np.concatenate(prim.tan).astype(np.float64)
        tlen = np.linalg.norm(tan, axis=1, keepdims=True)
        tan /= np.maximum(tlen, 1e-8)
        # The stored tangent leans slightly off the stored normal, so
        # orthogonalise it (Gram-Schmidt) to emit an orthonormal basis. A
        # tangent that is zero or parallel to the normal leaves no direction:
        # cross the normal with the unit axis it leans on least instead.
        n = nrm.astype(np.float64)
        axis = np.zeros_like(n)
        axis[np.arange(len(n)), np.argmin(np.abs(n), axis=1)] = 1.0
        tan -= np.einsum("ij,ij->i", n, tan)[:, None] * n
        tlen = np.linalg.norm(tan, axis=1, keepdims=True)
        tan = np.where(tlen > 1e-6, tan / np.maximum(tlen, 1e-6), np.cross(n, axis))
        tan /= np.maximum(np.linalg.norm(tan, axis=1, keepdims=True), 1e-8)
        # glTF bitangent = cross(N, T.xyz) * T.w, so w is the handedness the
        # stored bitangent has against the tangent and the (mirrored) normal.
        w = np.where(np.einsum("ij,ij->i", np.cross(n, tan), np.concatenate(prim.btan)) < 0,
                     -1.0, 1.0)
        tangent = np.concatenate([tan, w[:, None]], axis=1).astype("<f4")
        idx = np.concatenate(prim.idx).astype("<u4").reshape(-1)
        attributes = {
            "POSITION": add_accessor(pos, "VEC3", 5126, 34962, True),
            "NORMAL": add_accessor(nrm, "VEC3", 5126, 34962),
            "TEXCOORD_0": add_accessor(uv, "VEC2", 5126, 34962),
            "TEXCOORD_1": add_accessor(uv_b, "VEC2", 5126, 34962),
            "TEXCOORD_2": add_accessor(uv_c, "VEC2", 5126, 34962),
            "TANGENT": add_accessor(tangent, "VEC4", 5126, 34962),
            "COLOR_0": add_accessor(
                np.concatenate([np.c_[c, np.ones(len(c))] for c in prim.color
                                if c is not None]).astype("<f4"), "VEC4", 5126, 34962)
            if any(c is not None for c in prim.color) else None,
        }
        gl_prims.append({
            "attributes": {name: index for name, index in attributes.items() if index is not None},
            "indices": add_accessor(idx, "SCALAR", 5125, 34963),
            "material": material(prim.material),
        })

    doc = {
        "asset": {"version": "2.0", "generator": generator},
        "extensionsUsed": extensions,
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


def material_slug(name):
    """Lower-case glTF material/file stem of an LMU material name."""
    return re.sub(r"[^A-Za-z0-9_]", "_", name).lower()


def rounded(values, digits=5):
    return [round(float(v), digits) for v in values]
