"""Import a Le Mans Ultimate track (rFactor 2 gMotor .scn + GMT meshes) as a FoxEngine level.

Usage:
    python Tools/External/LMU/import_lmu_track.py <raw_dir> [--shared DIR]... [--out DIR]
        [--scene FILE] [--name NAME] [--cell-size M] [--max-texture-size N] [--terrain-texel M]

<raw_dir> is the directory of files extracted from the track's .mas archives
(.scn, .AIW, GMT meshes, material .json, DDS textures). --shared adds
directories searched after it: the game's shared Core/Locations/Commonmaps.mas
(parked vehicles, crowds, flags, tents, pit equipment), which lmu_mas.py cannot
decrypt, dumped from the running game by lmu-gamerip tools/dump_mas_entries.py.

Outputs (default Projects/Showcase/Assets/Tracks/RoadAtlanta/ and
Projects/Showcase/Assets/Scenes/roadatlanta.json):
    <kind>_<x>_<z>.gltf/.bin   rendered geometry, binned by triangle centroid
                               into --cell-size grid cells so each file is
                               frustum culled on its own:
        surface   CollTarget instances (asphalt, kerbs, grass, gravel, pit
                  lane): static mesh collider, no shadow casting
        scenery   ShadowCaster instances (buildings, barriers, trees, props)
        ground    everything else (far terrain, road lines, verges, decals)
    <kind>_wide.gltf/.bin      meshes over 4 cells wide (far terrain, horizon,
                               far tree lines), kept whole
    collision.gltf/.bin        the .scn's Render=False collision proxies (walls,
                               armco, tyre walls): collider only, never drawn
    textures/*.dds             BC-compressed, full mip chains
    <scene>                    FoxLevel with one mesh entity per file, a
                               CarSpawn on pole position and a camera behind it

Pipeline:
    .scn  -> instances. Skipped: Render=False helpers other than collision
             proxies (shadow casters, rain/irradiance volumes) and the x*
             timing triggers (xFinish, xSector1, ...), which the game collides
             with but cars pass through; ShadowObject shadow-only proxies;
             AnimFile instances (marshals, birds, helicopter), which need
             skeletal animation. Moveable signs render but do not collide.
    .gmt  -> world-space geometry (LMU track instances carry no transforms),
             except Moveable instances (ground ad and speed-limit signs): their
             vertices are rotated in place but centred on the origin, and the
             game puts them at the GMT header pivot, so the pivot is added.
             Every other track mesh has a zero pivot.
    material .json -> glTF metallic-roughness (lmu_assets.MaterialBuilder),
             plus what track shaders need: the albedo stage's UV scale or
             WorldPlanar mapping is baked into TEXCOORD_0, _ALBR albedo alpha
             becomes roughness when no metalRough map exists, cutout
             _ALBA/_DCA albedo alpha becomes MASK. Materials without JSON use
             their GMT texture list (_ALB*, _MR, _NM). Night glows, puddles and
             runtime screens are dropped.
    terrain (L2IBLTERRAIN grass) -> its splat-map blend of four tiled layers
             is evaluated offline into one albedo per material and output
             file (TerrainBaker; see there for what is approximated).
    palette tint (parked vehicles, trucks, pit equipment) -> the palette swatch
             each vertex's UV_B picks becomes COLOR_0 (linear RGB), the albedo
             alpha stays the mask, and the material gets the albedoAlphaMask
             vertex tint (PaletteTint). VERTEXCOLOR=AlbedoMultiply materials get
             a white COLOR_0 and the multiply tint (their true colours are not
             decoded).
    .AIW  -> pole position (GridIndex 0); heading along the starting grid,
             height from the drivable surface below it.

Frames:
    GMT world: left-handed, Y up; FoxEngine's frame is the same, so .scn/.AIW
    coordinates are engine coordinates. glTF out: (x, y, -z), which Assimp's
    ConvertToLeftHanded undoes; the Z mirror also turns GMT's clockwise
    winding into glTF's counter-clockwise. UVs are used as stored (like cars).
"""

import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

from lmu_assets import (REPO_ROOT, LmuMaterialDefinition, Material, MaterialBuilder, Primitive,
                        RawDir, Gmt, TextureWriter, adjust, load_rgba, material_slug,
                        rounded, srgb_to_linear, strip_comment, write_gltf)

PROJECT_ASSETS = REPO_ROOT / "Projects" / "Showcase" / "Assets"
DEFAULT_OUT = PROJECT_ASSETS / "Tracks" / "RoadAtlanta"
DEFAULT_SCENE = PROJECT_ASSETS / "Scenes" / "roadatlanta.json"
GMT_TO_GLTF = np.array([1.0, 1.0, -1.0])  # see "Frames" above
GENERATOR = "FoxEngine Tools/import_lmu_track.py"

TRIGGER_INSTANCE = re.compile(r"^x[A-Z]")

# Dropped materials: wet-weather decals and screens/glows driven at runtime.
SKIP_MATERIALS = {"", "PUDDLE_DECAL", "SCREENFRAMEBUFFER", "BLD_MICHELINTOWER_GLOW"}
SKIP_SHADERS = re.compile(r"LUM")  # L2HIGHLUMTRACKSIDE, L2LOWLUMEMISSIVE: night lights

COLLISION_MATERIAL = "COLLISION_PROXY"
CONSTANT_MATERIALS = {COLLISION_MATERIAL: ((0.5, 0.5, 0.5), 1.0, 0.0)}
# Base color of props whose albedo is missing (Commonmaps.mas not dumped, see
# --shared): LMU's own _DEFTEX light grey makes parked trucks and RVs glare white.
MISSING_ALBEDO = (0.18, 0.18, 0.19)

KIND_CAST_SHADOWS = {"surface": False, "scenery": True, "ground": False}


# --------------------------------------------------------------------------- .scn

class Instance:
    def __init__(self, name):
        self.name = name
        self.lines = []

    def flag(self, key):
        return any(re.search(key + r"\s*=\s*True", line, re.I) for line in self.lines)

    @property
    def rendered(self):
        return not any(re.search(r"Render\s*=\s*False", line, re.I) for line in self.lines)

    def meshes(self):
        """[(mesh file, CollTarget, ShadowCaster, ShadowObject)] of the instance's MeshFile lines."""
        out = []
        for line in self.lines:
            m = re.search(r"MeshFile\s*=\s*(\S+\.gmt)", line, re.I)
            if m:
                out.append((m.group(1), bool(re.search(r"CollTarget\s*=\s*True", line, re.I)),
                            "shadowcaster" in line.lower(), "shadowobject" in line.lower()))
        return out


MAX_SHADOW_RANGE = re.compile(r"^MaxShadowRange\s*=\s*\(\s*([0-9.]+)\s*\)", re.I)


def read_scn(path):
    """(instances, level shadow distance) of the scene; the distance is None when
    the file's top-level MaxShadowRange line is missing."""
    instances, current, pending, shadow_range = [], None, None, None
    for raw in path.read_text(encoding="latin-1").splitlines():
        line = strip_comment(raw)
        m = re.match(r"Instance\s*=\s*(\S+)", line, re.I)
        if m:
            pending = m.group(1)
        elif line.startswith("{") and pending:
            current = Instance(pending)
            instances.append(current)
            pending = None
        elif line.startswith("}"):
            current = None
        elif current is not None and line:
            current.lines.append(line)
        elif line:
            m = MAX_SHADOW_RANGE.match(line)
            if m:
                shadow_range = float(m.group(1))
    return instances, shadow_range


def classify(instance, coll_target, shadow_caster, shadow_object):
    """'surface' | 'scenery' | 'ground' | 'collision', or None to skip."""
    if not instance.rendered:
        return "collision" if coll_target and not TRIGGER_INSTANCE.match(instance.name) else None
    if shadow_object or any(re.search(r"AnimFile\s*=", line, re.I) for line in instance.lines):
        return None
    if coll_target and not instance.flag("Moveable"):
        return "surface"
    return "scenery" if shadow_caster else "ground"


# --------------------------------------------------------------------------- materials

def cutout_alpha(path):
    """True when an albedo's alpha is a cutout mask rather than roughness
    (_ALBR) or clearcoat data: some fully clear texels and few mid-range ones."""
    if path.stem.upper().endswith(("_ALBR", "_ALBC")):
        return False
    alpha = load_rgba(path)[..., 3]
    return float(alpha.min()) < 0.05 and float(((alpha > 0.1) & (alpha < 0.9)).mean()) < 0.45


class PaletteTint:
    """A material LMU colours per vertex from a palette: each vertex's UV_B picks
    a swatch, and the albedo's alpha masks where it multiplies in (parked cars,
    RVs, trucks, fuel rigs, awnings). Two shader forms carry it:
        L2IBLSTANDARDBLEND  albedoAltMap on ALBEDOALTUV=UV_B, BLENDMASK=AlbedoA_A,
                            ALBEDOFUNCTION Multiply or MultiplySquare
        L2IBLCLEARCOAT      detailMap on DETAILMAP=UV_B, DETAILALBEDO=Multiply,
                            DETAILMASKING=AlbedoAlpha
    Both become the engine's albedoAlphaMask vertex tint: the swatch travels in
    COLOR_0, the material keeps its one albedo whose alpha is the mask."""

    MODE = "albedoAlphaMask"

    def __init__(self, palette, albedo, square):
        self.palette = palette   # float RGBA image
        self.albedo = albedo     # path of the albedo whose alpha is the mask
        self.square = square

    @staticmethod
    def of(lmu, raw):
        flags, stages = lmu.uv_flags, lmu.stages
        albedo = raw.texture(stages["albedoMap"]) if stages.get("albedoMap") else None
        if albedo is None:
            return None
        if (stages.get("albedoAltMap") and flags.get("ALBEDOALTUV") == "UV_B"
                and flags.get("BLENDMASK", "").startswith("Albedo")
                and flags.get("ALBEDOFUNCTION") in ("Multiply", "MultiplySquare")):
            palette, square = stages["albedoAltMap"], flags["ALBEDOFUNCTION"] == "MultiplySquare"
        elif (stages.get("detailMap") and flags.get("DETAILMAP") == "UV_B"
                and flags.get("DETAILALBEDO") == "Multiply" and flags.get("DETAILMASKING") == "AlbedoAlpha"):
            palette, square = stages["detailMap"], False
        else:
            return None
        path = raw.texture(palette)
        return PaletteTint(load_rgba(path), albedo, square) if path else None

    def swatches(self, uv_b):
        """Per-vertex swatch (sRGB, uint8 RGB) at UV_B, nearest texel."""
        h, w = self.palette.shape[:2]
        x = (np.mod(uv_b[:, 0], 1.0) * w).astype(np.int64).clip(0, w - 1)
        y = (np.mod(uv_b[:, 1], 1.0) * h).astype(np.int64).clip(0, h - 1)
        return (self.palette[y, x, :3] * 255 + 0.5).astype(np.uint8)

    def colors(self, uv_b):
        """Per-vertex linear RGB of the swatch at UV_B for COLOR_0, squared for
        MultiplySquare (the shader form multiplies the square of the swatch)."""
        colour = srgb_to_linear(self.swatches(uv_b).astype(np.float64) / 255.0)
        return (colour * colour if self.square else colour).astype(np.float32)


class TrackMaterialBuilder(MaterialBuilder):
    def __init__(self, raw, textures):
        super().__init__(raw, textures, CONSTANT_MATERIALS, bake_albedo_mapping=True)
        self.tints = {}

    def palette_tint(self, name):
        """The material's PaletteTint, or None."""
        if name not in self.tints:
            definition = self.raw.json(name)
            self.tints[name] = PaletteTint.of(LmuMaterialDefinition(definition, self.raw), self.raw) \
                if definition else None
        return self.tints[name]

    def albedo_multiply(self, name):
        """True when the material multiplies mesh vertex colour into its albedo
        (VERTEXCOLOR=AlbedoMultiply) and its true vertex colours are unknown, so
        the mesh carries white: the tint mode is emitted for the engine, the
        colour is neutral."""
        definition = self.raw.json(name)
        return bool(definition) and \
            (definition.get("preprocessorFlags") or {}).get("VERTEXCOLOR") == "AlbedoMultiply"

    def _mean_color(self, stem):
        if stem and self.raw.texture(stem):
            return super()._mean_color(stem)
        return MISSING_ALBEDO

    def skipped(self, name):
        if name in SKIP_MATERIALS:
            return True
        definition = self.raw.json(name)
        return bool(definition and SKIP_SHADERS.search(definition.get("shader", "")))

    def _alpha_mode(self, lmu, albedo):
        mode = super()._alpha_mode(lmu, albedo)
        if mode == "OPAQUE" and albedo and not self._alpha_is_data(lmu) and cutout_alpha(albedo):
            return "MASK"
        return mode

    @staticmethod
    def _alpha_is_data(lmu):
        """Albedo alpha that is not opacity: a clearcoat mask, or the blend mask
        between albedo and palette of L2IBLSTANDARDBLEND (BLENDMASK=AlbedoA_A,
        e.g. parked trucks, half of whose texels are 'clear')."""
        return "CLEARCOAT" in lmu.shader or lmu.uv_flags.get("BLENDMASK", "").startswith("Albedo")

    def _json_detail_albedo(self, mat, lmu, name):
        """A palette-tinted material carries its swatches in COLOR_0, so the map
        its detail stage names is that palette, not a detail layer: emitting it
        would tint twice and on the stage's own UV set, not UV_B."""
        if lmu.stages.get("detailMap") and self.palette_tint(name) is not None:
            self.report.append(f"{name}: dropped detail albedo (palette tint in COLOR_0)")
            return
        super()._json_detail_albedo(mat, lmu, name)

    def _json_metal_rough(self, mat, lmu):
        _, mr = lmu.texture("metal_rough")
        _, albedo = lmu.albedo()
        if mr is None and not lmu.is_glass and albedo and albedo.stem.upper().endswith("_ALBR"):
            self._roughness_from_alpha(mat, albedo, lmu.adjust_amount("Roughness"),
                                       lmu.adjust_amount("Metallic"))
            return albedo
        return super()._json_metal_rough(mat, lmu)

    def _roughness_from_alpha(self, mat, albedo, rough_adjust=0.5, metal_adjust=0.5):
        rough = adjust(load_rgba(albedo)[..., 3], rough_adjust)
        metal = np.full_like(rough, adjust(0.0, metal_adjust))
        packed = np.stack([np.ones_like(rough), rough, metal, np.ones_like(rough)], axis=-1)
        mat.metal_rough = self.textures.encode(f"{mat.name}_mr.dds", packed)

    def _build_from_gmt(self, mat, name, gmt_textures):
        """Slots of a GMT material block without JSON, picked by LMU suffix."""
        def first(pattern):
            for stem in gmt_textures:
                path = self.raw.texture(stem)
                if path and not path.stem.upper().startswith("_DEFTEX") and re.search(pattern, path.stem, re.I):
                    return path
            return None

        albedo, mr, normal = first(r"_ALB[A-Z]?$"), first(r"_MR$"), first(r"_NM$")
        if albedo:
            mat.base_color = self.textures.copy(albedo)
            if cutout_alpha(albedo):
                mat.alpha_mode = "MASK"
        else:
            self._solid_color(mat, self._mean_color(None))
        if normal:
            mat.normal = self.textures.copy(normal)
        if mr:
            self._repack_mr(mat, mr, 0.5, 0.5)
        elif albedo and albedo.stem.upper().endswith("_ALBR"):
            self._roughness_from_alpha(mat, albedo)
        else:
            self._solid_mr(mat, 0.5, 0.0)
        self.report.append(f"{name}: no material JSON, base={albedo.name if albedo else 'const'} "
                           f"normal={'tex' if normal else '-'} mr={mr.name if mr else 'const'} "
                           f"{mat.alpha_mode}")


# --------------------------------------------------------------------------- terrain

def sample_bilinear(image, u, v):
    """Wrapped bilinear lookup of an (H, W, C) image at texture coordinates
    (D3D convention: v = 0 is the top row)."""
    h, w = image.shape[:2]
    x = np.mod(u, 1.0) * w - 0.5
    y = np.mod(v, 1.0) * h - 0.5
    x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    x0, y0, x1, y1 = x0 % w, y0 % h, (x0 + 1) % w, (y0 + 1) % h
    top = image[y0, x0] * (1 - fx) + image[y0, x1] * fx
    bottom = image[y1, x0] * (1 - fx) + image[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


def rasterize(points, values, tris, lo, size, w, h):
    """Per-texel linear interpolation of per-vertex `values` over triangles
    projected to the XZ grid (lo, size, w x h); texels no triangle covers take
    the mean value."""
    grid = np.full((h, w, values.shape[1]), np.nan)
    px = (points - lo) / size * [w, h] - 0.5
    for tri in tris:
        p = px[tri]
        x0, y0 = np.maximum(np.ceil(p.min(axis=0)).astype(int), 0)
        x1, y1 = np.minimum(np.floor(p.max(axis=0)).astype(int), [w - 1, h - 1])
        if x1 < x0 or y1 < y0:
            continue
        ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
        e0, e1 = p[1] - p[0], p[2] - p[0]
        den = e0[0] * e1[1] - e1[0] * e0[1]
        if abs(den) < 1e-12:
            continue
        dx, dy = xs - p[0, 0], ys - p[0, 1]
        b1 = (dx * e1[1] - e1[0] * dy) / den
        b2 = (e0[0] * dy - dx * e0[1]) / den
        inside = (b1 >= -1e-6) & (b2 >= -1e-6) & (b1 + b2 <= 1 + 1e-6)
        bary = np.stack([1 - b1 - b2, b1, b2], axis=-1)[inside]
        grid[ys[inside], xs[inside]] = bary @ values[tri]
    missing = np.isnan(grid[..., 0])
    grid[missing] = np.nanmean(grid[~missing], axis=0) if (~missing).any() else 0.0
    return grid


def resize_field(grid, w, h):
    """Bilinear resize of a float (H, W, C) field."""
    channels = [np.asarray(Image.fromarray(grid[..., c].astype(np.float32), "F")
                           .resize((w, h), Image.BILINEAR)) for c in range(grid.shape[2])]
    return np.stack(channels, axis=-1)


class TerrainBaker:
    """L2IBLTERRAIN materials -> one baked albedo per (material, output file).

    LMU blends four WorldPlanar-tiled layers (albedoRoughA..D) per pixel by a
    splat map on UV_B, which is itself a planar world mapping covering the
    circuit, then tints by a colour map (splatterOverlayMap, on UV_B or the
    UV_C ribbon along the track). A FoxEngine material has one UV set and one
    albedo, so the blend is evaluated here, at --terrain-texel metres per texel
    over the XZ bounds of the material's triangles in each file, and the
    primitive gets UVs into that unique texture. Blend (inferred, not decoded
    from the shader): start from layer A (dry straw), lerp in B, C, D by the
    splat's R, G, B, then multiply by the overlay map, weighted by the
    material's Terrain.SplatterOverlay times the map's alpha (multiply, not an
    overlay blend, matches the in-game grass/asphalt brightness ratio); UV_C is
    rasterized at 1 m.
    Roughness is the splat-weighted mean of the layers' _ALBR alpha; pattern
    and mix maps, vertex colour and normals are not reproduced.

    Close-up the bake is softer than the game (12 cm texels at the 2048 cap on
    a 250 m cell; off-track "ground" terrain, seen from beyond the barriers, is
    OFF_TRACK_TEXEL_SCALE times coarser). Later step: an engine terrain
    material sampling the splat map, overlay and the four layers per pixel
    (layers at world X/Z, splat at the affine UV_B fit, UV_C as a second vertex
    UV set) would remove that limit and the per-tile textures."""

    SHADER = "L2IBLTERRAIN"
    LAYERS = "ABCD"
    OVERLAY_GRID = 1.0  # metres per UV_C sample
    OFF_TRACK_TEXEL_SCALE = 4.0

    def __init__(self, raw, textures, texel, max_size):
        self.raw = raw
        self.textures = textures
        self.texel = texel
        self.max_size = max_size
        self.samples = defaultdict(list)  # material -> [(world xz, UV_B)]
        self.placeholders = {}
        self.images = {}
        self.report = []

    def is_terrain(self, name):
        definition = self.raw.json(name)
        return bool(definition) and definition.get("shader") == self.SHADER

    def placeholder(self, name, gmt, faces):
        """(stand-in material until bake(), its per-vertex world X/Z for the
        bake); records the UV_B mapping samples. The LMU UV_B and UV_C sets stay
        in their own vertex streams, so `uv` is the mesh's own UV_A."""
        used = np.unique(gmt.tris[faces])
        self.samples[name].append((gmt.pos[used][:, [0, 2]], gmt.uv_b[used]))
        if name not in self.placeholders:
            mat = Material(material_slug(name))
            mat.lmu_name = name
            self.placeholders[name] = mat
        return self.placeholders[name], gmt.pos[:, [0, 2]]

    def bake(self, files):
        """Replaces every placeholder primitive with its baked material; their
        UVs become UVs into the baked texture."""
        mappings = {name: self._fit_uv_b(name) for name in self.samples}
        for stem, (kind, prims) in files.items():
            texel = self.texel * (1.0 if kind == "surface" else self.OFF_TRACK_TEXEL_SCALE)
            for prim in prims.values():
                name = getattr(prim.material, "lmu_name", None)
                if name is None:
                    continue
                verts = np.concatenate([np.c_[world, uv_c]
                                        for world, uv_c in zip(prim.world, prim.uv_c)])
                tris = np.concatenate(prim.idx)
                lo, size = verts[:, :2].min(axis=0), np.maximum(np.ptp(verts[:, :2], axis=0), 1e-3)
                prim.uv = [(chunk - lo) / size for chunk in prim.world]
                prim.world = []
                prim.material = self._bake_material(name, stem, lo, size, texel, mappings[name], verts, tris)

    def _fit_uv_b(self, name):
        """Least-squares affine map world (x, z) -> UV_B (exact for LMU terrain)."""
        xz = np.concatenate([s[0] for s in self.samples[name]])
        uv_b = np.concatenate([s[1] for s in self.samples[name]])
        coef, *_ = np.linalg.lstsq(np.c_[xz, np.ones(len(xz))], uv_b, rcond=None)
        return coef

    def _image(self, stem, size=None):
        """Float RGBA of a texture, box-filtered to `size` texels when given."""
        key = (stem.lower(), size)
        if key not in self.images:
            image = Image.open(self.raw.texture(stem))
            image.load()
            image = image.convert("RGBA")
            if size and size < image.width:
                image = image.resize((size, size), Image.BOX)
            self.images[key] = np.asarray(image).astype(np.float32) / 255.0
        return self.images[key]

    def _bake_material(self, name, stem, lo, size, texel, uv_b_map, verts, tris):
        lmu = LmuMaterialDefinition(self.raw.json(name), self.raw)
        # Multiples of 4: D3D11 rejects BC textures whose top level is not whole blocks.
        w, h = (int(np.clip(np.ceil(s / texel / 4) * 4, 4, self.max_size)) for s in size)
        x = lo[0] + (np.arange(w) + 0.5) / w * size[0]
        z = lo[1] + (np.arange(h) + 0.5) / h * size[1]
        x, z = np.meshgrid(x, z)
        uv_b = np.stack([x, z, np.ones_like(x)], axis=-1) @ uv_b_map
        splat = sample_bilinear(self._image(lmu.stages["splatterMap"]), uv_b[..., 0], uv_b[..., 1])

        result = None
        for i, layer in enumerate(self.LAYERS):
            texture = lmu.stages[f"albedoRough{layer}Map"]
            scale = lmu.uv_scale.get(f"Albedo{layer}", [1.0, 1.0])
            # Prefilter to twice the bake rate: the bake is the layer's mip.
            tile_texels = int(np.clip(2.0 / (scale[0] * size[0] / w), 4, 2048))
            texels = sample_bilinear(self._image(texture, tile_texels), x * scale[0], z * scale[1])
            result = texels if result is None else result + (texels - result) * splat[..., i - 1:i]
        rgb, roughness = result[..., :3], float(result[..., 3].mean())

        overlay_note = "no overlay"
        overlay_stem = lmu.stages.get("splatterOverlayMap")
        if overlay_stem:
            if lmu.uv_flags.get("SPLATTEROVERLAYUV") == "UV_C":
                cw, ch = (max(2, int(np.ceil(s / self.OVERLAY_GRID))) for s in size)
                uv = resize_field(rasterize(verts[:, :2], verts[:, 2:4], tris, lo, size, cw, ch), w, h)
            else:
                uv = uv_b
            scale = lmu.uv_scale.get("SplatterOverlay", [1.0, 1.0])
            tint = sample_bilinear(self._image(overlay_stem), uv[..., 0] * scale[0], uv[..., 1] * scale[1])
            strength = (lmu.definition["parameters"]["cbParameters"].get("Terrain", {})
                        .get("SplatterOverlay", 1.0))
            weight = strength * tint[..., 3:4]
            rgb = rgb * (1 - weight + weight * tint[..., :3])
            overlay_note = f"overlay {lmu.uv_flags.get('SPLATTEROVERLAYUV')} x{strength:.2f}"

        mat = Material(f"{material_slug(name)}_{stem}".replace("-", "m"))
        rgba = np.concatenate([rgb, np.ones_like(rgb[..., :1])], axis=-1)
        # Texture2D views colour maps as sRGB; UNORM keeps texconv from re-encoding.
        mat.base_color = self.textures.encode(f"{mat.name}_basecolor.dds", rgba)
        rough = adjust(roughness, lmu.adjust_amount("Roughness"))
        mat.metal_rough = self.textures.solid(f"{mat.name}_mr.dds",
                                              [255, int(rough * 255 + 0.5), 0, 255])
        self.report.append(f"{name} @ {stem}: baked {w}x{h} ({size[0] / w:.3f} m/texel), "
                           f"{overlay_note}, roughness {rough:.2f}")
        return mat


# --------------------------------------------------------------------------- geometry

class CellGrid:
    """Output files: {stem: (kind, {material name: Primitive})} in glTF space.

    Triangles go to the grid cell of their centroid (<kind>_<x>_<z>). Meshes
    wider than WIDE_CELLS cells (far terrain, horizon) stay whole in
    <kind>_wide: binning them would scatter a few huge triangles over hundreds
    of cells. Collision proxies form one file."""

    WIDE_CELLS = 4

    def __init__(self, cell_size):
        self.cell_size = cell_size
        self.files = {}

    def add(self, kind, gmt, builder, terrain):
        a, b, c = (gmt.pos[gmt.tris[:, k]] for k in range(3))
        has_area = np.linalg.norm(np.cross(b - a, c - a), axis=1) > 1e-12
        centroid = (a + b + c) / 3.0
        cells = np.floor(centroid[:, [0, 2]] / self.cell_size).astype(np.int64)
        whole = None
        if kind == "collision":
            whole = "collision"
        elif np.ptp(gmt.pos[:, [0, 2]], axis=0).max() > self.WIDE_CELLS * self.cell_size:
            whole = f"{kind}_wide"
        pos = gmt.pos * GMT_TO_GLTF
        nrm = gmt.nrm * GMT_TO_GLTF
        tan = gmt.tangent * GMT_TO_GLTF
        btan = gmt.bitangent * GMT_TO_GLTF
        added = 0
        for mat_index in np.unique(gmt.face_mat):
            mat_name, gmt_textures = gmt.materials[mat_index]
            if kind == "collision":
                mat_name, gmt_textures = COLLISION_MATERIAL, []
            elif builder.skipped(mat_name):
                continue
            select = (gmt.face_mat == mat_index) & has_area
            if not select.any():
                continue
            color = None
            if terrain.is_terrain(mat_name):
                mat, world = terrain.placeholder(mat_name, gmt, select)
                uv = gmt.uv
            else:
                mat = builder.get(mat_name, gmt_textures)
                uv = gmt.pos[:, [0, 2]] if mat.uv_source == "WorldPlanar" else gmt.uv
                world = None
                tint = builder.palette_tint(mat_name)
                if tint is not None:
                    # Per-vertex palette swatch in COLOR_0; the albedo's alpha is
                    # the mask the engine tints through.
                    mat.vertex_tint = tint.MODE
                    color = tint.colors(gmt.uv_b)
                elif builder.albedo_multiply(mat_name):
                    # VERTEXCOLOR=AlbedoMultiply: true vertex colours unknown, so
                    # the stream is white (neutral) and the mode still emitted.
                    mat.vertex_tint = "multiply"
                    color = np.ones((len(gmt.pos), 3), np.float32)
            if whole:
                parts = [(whole, select)]
            else:
                parts = [(f"{kind}_{cx}_{cz}", select & (cells[:, 0] == cx) & (cells[:, 1] == cz))
                         for cx, cz in np.unique(cells[select], axis=0)]
            for stem, faces in parts:
                prims = self.files.setdefault(stem, (kind, {}))[1]
                prims.setdefault(mat.name, Primitive(mat)).add(
                    pos, nrm, uv, gmt.uv_b, gmt.uv_c, tan, btan, gmt.tris[faces], world, color)
            added += int(select.sum())
        return added


def surface_height(surfaces, x, z):
    """Highest drivable-surface Y under (x, z), or None (GMT space)."""
    best = None
    for gmt in surfaces:
        a, b, c = (gmt.pos[gmt.tris[:, k]] for k in range(3))
        # Barycentric coordinates of (x, z) in each triangle's XZ projection.
        v0, v1 = (b - a)[:, [0, 2]], (c - a)[:, [0, 2]]
        v2 = np.array([x, z]) - a[:, [0, 2]]
        den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]
        with np.errstate(divide="ignore", invalid="ignore"):
            u = (v2[:, 0] * v1[:, 1] - v1[:, 0] * v2[:, 1]) / den
            v = (v0[:, 0] * v2[:, 1] - v2[:, 0] * v0[:, 1]) / den
            inside = (np.abs(den) > 1e-12) & (u >= 0) & (v >= 0) & (u + v <= 1)
        if inside.any():
            y = (a[:, 1] + u * (b - a)[:, 1] + v * (c - a)[:, 1])[inside].max()
            best = y if best is None else max(best, y)
    return best


# --------------------------------------------------------------------------- .AIW

def read_pole(aiw_path, rows=10):
    """(pole position, unit XZ race direction) from the AIW [GRID]: the
    direction is the grid's long axis, oriented toward pole. (Waypoints are no
    help here: near the grid the AIW interleaves grid-slot entries with them.)"""
    text = aiw_path.read_text(encoding="latin-1")
    grid = text.split("[GRID]", 1)[1].split("[", 1)[0]
    slots = np.array([[float(v) for v in s.split(",")] for s in re.findall(r"Pos=\(([^)]*)\)", grid)])
    xz = slots[:rows, [0, 2]]
    axis = np.linalg.svd(xz - xz.mean(axis=0))[2][0]
    if np.dot(xz[0] - xz.mean(axis=0), axis) < 0:
        axis = -axis
    return slots[0].copy(), axis


# --------------------------------------------------------------------------- scene

def write_scene(path, name, mesh_entries, pole, yaw, bounds_min, bounds_max, floor_top,
                shadow_distance=None):
    forward = np.array([math.sin(math.radians(yaw)), 0.0, math.cos(math.radians(yaw))])
    eye = pole - forward * 8.0 + [0.0, 2.5, 0.0]
    entities = [
        {"id": "", "name": "Main Camera", "components": {"camera": {
            "eye": rounded(eye, 3), "yaw": round(yaw, 3), "pitch": -8, "nearZ": 0.05, "farZ": 4000}}},
        {"id": "", "name": "Sun", "components": {"directionalLight": {
            "elevation": 42.0, "azimuth": -35.0, "intensity": 2.6, "color": [1.0, 0.93, 0.82]}}},
        {"id": "", "name": "Car Spawn", "actor": "Assets/Scripts/Actors/CarSpawn.lua", "components": {
            "transform": {"position": rounded(pole, 3), "rotationDegrees": [0, round(yaw, 3), 0]}}},
    ]
    for mesh_path, kind in mesh_entries:
        entities.append({"id": "", "name": Path(mesh_path).stem, "components": {"meshRenderer": {
            "mesh": mesh_path,
            "castShadows": KIND_CAST_SHADOWS.get(kind, False),
            "receiveShadows": True,
            "collision": kind in ("surface", "collision"),
            "visible": kind != "collision"}}})
    for i, entity in enumerate(entities, start=1):
        entity["id"] = f"{i:016X}"
    margin = 100.0
    level = {
        "format": "FoxLevel",
        "version": 2,
        "name": name,
        "settings": {
            "environment": {"skybox": "Assets/Textures/citrus_orchard_road_puresky_2k.dds",
                            "iblIntensity": 0.4},
            "physics": {
                # Safety net below the whole track; the surfaces carry the cars.
                "floor": {"min": rounded([bounds_min[0] - margin, floor_top - 1.0, bounds_min[2] - margin], 3),
                          "max": rounded([bounds_max[0] + margin, floor_top, bounds_max[2] + margin], 3),
                          "friction": 0.9, "restitution": 0.05},
                "characterSpawn": rounded(pole + [0.0, 1.0, 0.0], 3),
                "gravity": True,
            },
            "toneMapping": {"operator": "ACES", "exposure": 1.0},
            "bloom": {"enabled": True, "threshold": 1.0, "intensity": 0.12, "scatter": 0.6},
        },
        "entities": entities,
    }
    if shadow_distance is not None:
        level["settings"]["shadows"] = {"distance": shadow_distance}
    path.write_text(json.dumps(level, indent=2) + "\n", encoding="ascii")


# --------------------------------------------------------------------------- main

def project_path(path):
    """Project-relative "Assets/..." path of a file under the Showcase project."""
    return path.resolve().relative_to(PROJECT_ASSETS.parent).as_posix()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", type=Path, help="directory of files extracted from the track's .mas archives")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--scene", type=Path, default=DEFAULT_SCENE)
    ap.add_argument("--name", default="Road Atlanta")
    ap.add_argument("--cell-size", type=float, default=250.0)
    ap.add_argument("--max-texture-size", type=int, default=2048)
    ap.add_argument("--terrain-texel", type=float, default=0.1,
                    help="metres per texel of the baked terrain albedo (capped by --max-texture-size)")
    ap.add_argument("--shared", type=Path, action="append", default=[],
                    help="extra directory searched for materials and textures (repeatable)")
    args = ap.parse_args()

    raw = RawDir(args.raw, args.shared)
    scn = next((p for p in raw.files.values() if p.suffix.lower() == ".scn"), None)
    aiw = next((p for p in raw.files.values() if p.suffix.lower() == ".aiw"), None)
    if scn is None or aiw is None:
        raise SystemExit(f"{args.raw} needs a .scn and an .AIW")

    args.out.mkdir(parents=True, exist_ok=True)
    for stale in [*args.out.glob("*.gltf"), *args.out.glob("*.bin"), *args.out.glob("textures/*.dds")]:
        stale.unlink()
    textures = TextureWriter(args.out, args.max_texture_size)
    builder = TrackMaterialBuilder(raw, textures)
    grid = CellGrid(args.cell_size)
    terrain = TerrainBaker(raw, textures, args.terrain_texel, args.max_texture_size)

    gmts, surfaces, counts = {}, [], defaultdict(int)
    instances, shadow_distance = read_scn(scn)
    for instance in instances:
        for mesh_file, coll_target, caster, shadow_object in instance.meshes():
            kind = classify(instance, coll_target, caster, shadow_object)
            if kind is None:
                counts["skipped"] += 1
                continue
            key = mesh_file.lower()
            if key not in gmts:
                path = raw.find(mesh_file)
                if path is None:
                    print(f"warning: {mesh_file} (instance {instance.name}) not found")
                    continue
                gmt = Gmt(path, with_materials=kind != "collision")
                gmt.pos = gmt.pos + gmt.pivot
                gmts[key] = gmt
            counts[kind] += grid.add(kind, gmts[key], builder, terrain)
            if kind == "surface":
                surfaces.append(gmts[key])

    terrain.bake(grid.files)
    mesh_entries = []
    for stem, (kind, prims) in sorted(grid.files.items()):
        write_gltf(args.out / f"{stem}.gltf", stem, list(prims.values()), GENERATOR)
        mesh_entries.append((project_path(args.out / f"{stem}.gltf"), kind))
    textures.cleanup()

    pole, direction = read_pole(aiw)
    ground = surface_height(surfaces, pole[0], pole[2])
    if ground is not None:
        pole[1] = ground
    yaw = math.degrees(math.atan2(direction[0], direction[1]))
    positions = np.concatenate([g.pos for g in surfaces])
    everything = np.concatenate([g.pos for g in gmts.values()])
    write_scene(args.scene, args.name, mesh_entries, pole, yaw, everything.min(axis=0),
                everything.max(axis=0), float(positions[:, 1].min()) - 40.0,
                shadow_distance)

    print(f"Output: {args.out}  scene: {args.scene}")
    print("Triangles: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"  {len(mesh_entries)} meshes, {len(builder.cache)} materials, {len(textures.written)} textures")
    print(f"  pole {rounded(pole, 3)} yaw {yaw:.2f} (surface {'found' if ground is not None else 'missing'})")
    print("Materials:")
    for line in builder.report + terrain.report:
        print("  " + line)


if __name__ == "__main__":
    main()
