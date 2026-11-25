"""Import a Le Mans Ultimate track (rFactor 2 gMotor .scn + GMT meshes) as a FoxEngine level.

Usage:
    python Tools/External/LMU/import_lmu_track.py <raw_dir> [--shared DIR]... [--out DIR]
        [--scene FILE] [--name NAME] [--cell-size M] [--max-texture-size N]

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
    terrain (L2IBLTERRAIN grass) -> a <material>.foxmat sidecar next to the
             glTFs (four world-planar albedo/rough layers, layer normals, splat
             map, overlay) that the engine's terrain shader blends per pixel;
             the glTF material references it through extras.fox.material.
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

from lmu_assets import (REPO_ROOT, LmuMaterialDefinition, MaterialBuilder, Primitive, RawDir, Gmt,
                        TextureWriter, UV_SET_INDEX, adjust, load_rgba, material_slug, rounded,
                        srgb_to_linear, strip_comment, write_gltf)

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
            if cutout_alpha(albedo):
                mat.alpha_mode = "MASK"
            mat.base_color = self.textures.copy(albedo, self._mask_cutoff(mat))
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

class TerrainMaterials:
    """L2IBLTERRAIN materials -> glTF material + `<material>.foxmat` sidecar.

    LMU blends four world-planar tiled albedo layers (albedoRoughA..DMap) per
    pixel by the splatter map on UV_B, then tints the result by the splatter
    overlay on UV_B or the UV_C ribbon. The sidecar keeps that blend for the
    engine's terrain shader instead of baking one albedo per cell: layer UV =
    world (x, z) * UV_Scale.Albedo<layer>, u from world X and v from world Z
    (GMT world space is engine world space); the splat's R, G and B pick layers
    B, C and D over layer A. Layer normals are normalAMap on the first two
    layers and normalBMap on the last two (inferred from the shader forms, not
    decoded). The overlay's alpha weights a tint of the blended albedo. Roughness
    is the blended albedo alpha (LMU _ALBR); the pattern and mix maps are not
    reproduced. The glTF material keeps the standard slots as the engine's
    fallback when the sidecar cannot be used."""

    SHADER = "L2IBLTERRAIN"
    LAYERS = "ABCD"

    def __init__(self, raw, textures, builder):
        self.raw = raw
        self.textures = textures
        self.builder = builder
        self.materials = {}
        self.documents = {}  # sidecar file name -> JSON document
        self.report = []

    def is_terrain(self, name):
        definition = self.raw.json(name)
        return bool(definition) and definition.get("shader") == self.SHADER

    def get(self, name, gmt_textures):
        """The material of an L2IBLTERRAIN name, carrying its sidecar reference."""
        if name not in self.materials:
            self.materials[name] = self._build(name, gmt_textures)
        return self.materials[name]

    def write(self, out_dir):
        """Writes every referenced sidecar next to the glTFs."""
        for file_name, document in sorted(self.documents.items()):
            (out_dir / file_name).write_text(json.dumps(document, indent=2) + "\n", encoding="ascii")

    def _build(self, name, gmt_textures):
        mat = self.builder.get(name, gmt_textures)
        lmu = LmuMaterialDefinition(self.raw.json(name), self.raw, self.report)
        layers = self._layers(name, lmu)
        splat = self._texture(lmu, "splatterMap")
        if not layers or splat is None:
            self.report.append(f"{name}: standard material "
                               f"(no {'albedo layer' if not layers else 'splatter map'})")
            return mat
        file_name = f"{material_slug(name)}.foxmat"
        document = {"format": "FoxMaterial", "version": 1, "type": "terrain", "layers": layers,
                    "albedoAlphaIsRoughness": True,
                    "splat": {"texture": splat, "uvSet": 1,
                              "uvScale": rounded(lmu.uv_scale.get("Splatter", [1.0, 1.0]), 6)}}
        overlay = self._overlay(lmu)
        if overlay:
            document["overlay"] = overlay
        document["roughnessScale"] = 1.0
        self.documents[file_name] = document
        mat.foxmat = file_name
        self.report.append(f"{name}: {file_name}, {len(layers)} layers"
                           + (f", overlay uvSet {overlay['uvSet']} x{overlay['strength']:.2f}"
                              if overlay else ", no overlay"))
        return mat

    def _texture(self, lmu, stage):
        """Copied texture path (relative to the sidecar) of a material stage, or
        None when the stage is missing or its file is not in the archives."""
        stem = lmu.stages.get(stage)
        path = self.raw.texture(stem) if stem else None
        return self.textures.copy(path) if path else None

    def _layers(self, name, lmu):
        """The sidecar's layers: contiguous albedo layers from A, each with its
        world scale and (when present) its normal map."""
        layers = []
        for index, letter in enumerate(self.LAYERS):
            albedo = self._texture(lmu, f"albedoRough{letter}Map")
            if albedo is None:
                break
            layer = {"albedo": albedo}
            normal = self._texture(lmu, "normalAMap" if index < 2 else "normalBMap")
            if normal is not None:
                layer["normal"] = normal
            scale = lmu.uv_scale.get(f"Albedo{letter}", [1.0, 1.0])
            if abs(float(scale[0]) - float(scale[1])) > 1e-6:
                self.report.append(f"{name}: layer {letter} world scale {list(scale)} is not "
                                   f"uniform, using {scale[0]}")
            layer["worldScale"] = round(float(scale[0]), 6)
            layers.append(layer)
        return layers

    def _overlay(self, lmu):
        """The sidecar's overlay, or None. LMU turns the stage off with
        SPLATTEROVERLAYMAP=@Undef, and then the stage is absent too."""
        if lmu.uv_flags.get("SPLATTEROVERLAYMAP") in (None, "@Undef"):
            return None
        texture = self._texture(lmu, "splatterOverlayMap")
        uv_set = UV_SET_INDEX.get(lmu.uv_flags.get("SPLATTEROVERLAYUV"))
        if texture is None or uv_set not in (1, 2):
            return None
        terrain = ((lmu.definition.get("parameters") or {}).get("cbParameters") or {}).get("Terrain") or {}
        return {"texture": texture, "uvSet": uv_set,
                "uvScale": rounded(lmu.uv_scale.get("SplatterOverlay", [1.0, 1.0]), 6),
                "strength": round(float(terrain.get("SplatterOverlay", 1.0)), 6)}


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
                mat = terrain.get(mat_name, gmt_textures)
                uv = gmt.uv
            else:
                mat = builder.get(mat_name, gmt_textures)
                uv = gmt.pos[:, [0, 2]] if mat.uv_source == "WorldPlanar" else gmt.uv
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
                    pos, nrm, uv, gmt.uv_b, gmt.uv_c, tan, btan, gmt.tris[faces], color)
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
    ap.add_argument("--shared", type=Path, action="append", default=[],
                    help="extra directory searched for materials and textures (repeatable)")
    args = ap.parse_args()

    raw = RawDir(args.raw, args.shared)
    scn = next((p for p in raw.files.values() if p.suffix.lower() == ".scn"), None)
    aiw = next((p for p in raw.files.values() if p.suffix.lower() == ".aiw"), None)
    if scn is None or aiw is None:
        raise SystemExit(f"{args.raw} needs a .scn and an .AIW")

    args.out.mkdir(parents=True, exist_ok=True)
    for stale in [*args.out.glob("*.gltf"), *args.out.glob("*.bin"), *args.out.glob("*.foxmat"),
                  *args.out.glob("textures/*.dds")]:
        stale.unlink()
    textures = TextureWriter(args.out, args.max_texture_size)
    builder = TrackMaterialBuilder(raw, textures)
    grid = CellGrid(args.cell_size)
    terrain = TerrainMaterials(raw, textures, builder)

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

    terrain.write(args.out)
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
