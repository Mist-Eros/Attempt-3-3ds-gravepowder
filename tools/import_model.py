#!/usr/bin/env python3
"""
Gravepowder model importer - writes the GPM v1 binary format.

    python tools/import_model.py <source.glb> --config assets/import/foo.json \
        --out data/models/foo.gpm [--kind hero|zombie|weapon] [--credit "text"] \
        [--preview out.png] [--preview-clip idle] [--preview-time 0.5] [--stats]

The positional source is optional if the config JSON has a "source" field.
The format itself is frozen; see docs/assets.md ("The GPM v1 format"). This
file is the reference *writer* (tools/gpm_selftest.py is the reference reader /
validator).

COORDINATE + SKINNING CONVENTION (the runtime must match this):
  * GPM space is right-handed, +Y up, +Z = the direction the model faces
    (characters) / the muzzle direction (guns). Units are metres.
  * Characters are grounded (bbox min y == 0) and centred on x/z; the origin is
    the point between the feet on the ground. Guns are centred on their bbox.
  * Bones are stored parent-before-child with *local rest* TRS; the root bone
    (parent = -1) carries the whole model transform (metric scale + config yaw
    + grounding/centring). Bone rest world matrices are the usual chain
    world = parent_world * T*R*S.
  * Every vertex stores its rest-pose position in GPM model space (i.e. the
    same space as the bone rest chain), plus one dominant bone index (rigid
    skinning) and a rest-pose normal.
  * Runtime skinning for a vertex with dominant bone b:
        rest_world[b] = chain of bone rest TRS
        anim_world[b] = chain of animated local TRS (missing track -> rest)
        skinned_pos   = anim_world[b] * inverse(rest_world[b]) * position
    and the normal is transformed by the rotation part of
        anim_world[b] * inverse(rest_world[b]).
    In other words inverse_bind = inverse(rest_world) and an animation track is
    just the bone's local TRS sampled over the clip.
  * Animation tracks are local TRS of the bone *relative to its parent bone*.
    The root bone's tracks already have the model transform composed in, so a
    clip animates the completely normalised (metre-scale, yawed, grounded)
    model.
  * Vertex colours are baked to 8-bit sRGB-ish display values (the renderer uses
    them as albedo directly); alpha comes from the material.
  * Static models (guns) have bone_count = 0 and animation_count = 0.

GLTF import details:
  * multiple skins/armatures are merged by joint NAME (the hero body/legs/feet
    have four identical skins sharing one armature);
  * rigid skinning: strongest WEIGHTS_0 influence wins; vertices are transformed
    with world(dominant joint) * inverseBind(dominant joint) into rest pose;
  * vertex colours = baseColorTexture sampled at TEXCOORD_0 (nearest, sRGB) *
    baseColorFactor (linear->sRGB), or factor alone; COLOR_0 is multiplied in
    when present (it is all-white on the shipped zombie); a config
    "colour_overrides" entry (keyed by mesh-node or material name) replaces the
    colour for that part;
  * animations are resampled at config "resample_hz" (default 30) and thinned:
    interior keys that a linear/slerp segment reproduces within a small epsilon
    are dropped;
  * model scaling: "kind" picks a canonical target (hero 1.80 m tall, zombie
    1.70 m tall, weapon 1.00 m long); config "scale" multiplies that; optional
    config "target_height"/"target_length" overrides the canonical target.

Python deps: numpy, pygltflib, Pillow.
"""

import argparse
import json
import math
import os
import struct
import sys
from bisect import bisect_right
from dataclasses import dataclass, field

import numpy as np

try:
    from pygltflib import GLTF2
except ImportError:  # pragma: no cover - friendly message for end users
    sys.stderr.write(
        "error: pygltflib is not installed (pip install pygltflib numpy Pillow)\n")
    raise

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "error: Pillow is not installed (pip install pygltflib numpy Pillow)\n")
    raise


# ---------------------------------------------------------------------------
# GPM v1 constants (docs/assets.md)
# ---------------------------------------------------------------------------

GPM_MAGIC = 0x31504D47          # "GPM1" little-endian
GPM_VERSION = 1

FLAG_SKINNED = 1 << 0
FLAG_HAS_ANIMATIONS = 1 << 1

HEADER_SIZE = 64
SECTION_TABLE_SIZE = 24         # 6 absolute u32 offsets
VERTEX_SIZE = 32
BONE_SIZE = 48
ANIMATION_SIZE = 16
TRACK_SIZE = 8
KEY_SIZE = 20

COMPONENT_TRANSLATION = 0
COMPONENT_ROTATION = 1
COMPONENT_SCALE = 2

ROLE_NAMES = {"idle", "walk", "run", "shoot", "reload", "attack", "hit", "death"}

# kind -> (measured axis, target metres); can be overridden in the config with
# "target_height" / "target_length".
KIND_TARGETS = {
    "hero": ("height", 1.80),
    "zombie": ("height", 1.70),
    "weapon": ("length", 1.00),
}

# Keyframe thinning tolerances (interior keys closer than this to the linear /
# slerp interpolation of their neighbours are dropped).
THIN_TRANSLATION_EPS = 5.0e-4    # metres
THIN_SCALE_EPS = 5.0e-4
THIN_ROTATION_EPS = math.radians(0.75)

# How the preview camera sits relative to the model (degrees, around +Y/+X).
PREVIEW_YAW = -32.0
PREVIEW_PITCH = 14.0

# numpy dtypes that map 1:1 onto the little-endian file records.
VERTEX_DTYPE = np.dtype([
    ("px", "<f4"), ("py", "<f4"), ("pz", "<f4"),
    ("nx", "<f4"), ("ny", "<f4"), ("nz", "<f4"),
    ("r", "u1"), ("g", "u1"), ("b", "u1"), ("a", "u1"),
    ("bone", "<u2"), ("pad", "<u2"),
])
BONE_DTYPE = np.dtype([
    ("parent", "<i2"), ("pad", "<u2"),
    ("tx", "<f4"), ("ty", "<f4"), ("tz", "<f4"),
    ("rx", "<f4"), ("ry", "<f4"), ("rz", "<f4"), ("rw", "<f4"),
    ("sx", "<f4"), ("sy", "<f4"), ("sz", "<f4"),
    ("name_off", "<u4"),
])
ANIM_DTYPE = np.dtype([
    ("name_off", "<u4"), ("duration", "<f4"),
    ("track_first", "<u4"), ("track_count", "<u4"),
])
TRACK_DTYPE = np.dtype([
    ("bone", "<u2"), ("component", "u1"), ("pad0", "u1"),
    ("key_count", "<u2"), ("pad1", "<u2"),
])
KEY_DTYPE = np.dtype([
    ("time", "<f4"),
    ("vx", "<f4"), ("vy", "<f4"), ("vz", "<f4"), ("vw", "<f4"),
])
assert VERTEX_DTYPE.itemsize == VERTEX_SIZE
assert BONE_DTYPE.itemsize == BONE_SIZE
assert ANIM_DTYPE.itemsize == ANIMATION_SIZE
assert TRACK_DTYPE.itemsize == TRACK_SIZE
assert KEY_DTYPE.itemsize == KEY_SIZE


# ---------------------------------------------------------------------------
# small math helpers (numpy, float64)
# ---------------------------------------------------------------------------

def mat4_identity():
    return np.eye(4, dtype=np.float64)


def mat4_translate(x, y, z):
    m = np.eye(4, dtype=np.float64)
    m[0, 3] = x
    m[1, 3] = y
    m[2, 3] = z
    return m


def mat4_scale(x, y, z):
    m = np.eye(4, dtype=np.float64)
    m[0, 0] = x
    m[1, 1] = y
    m[2, 2] = z
    return m


def mat4_rot_x(rad):
    c, s = math.cos(rad), math.sin(rad)
    m = np.eye(4, dtype=np.float64)
    m[1, 1] = c
    m[1, 2] = -s
    m[2, 1] = s
    m[2, 2] = c
    return m


def mat4_rot_y(rad):
    c, s = math.cos(rad), math.sin(rad)
    m = np.eye(4, dtype=np.float64)
    m[0, 0] = c
    m[0, 2] = s
    m[2, 0] = -s
    m[2, 2] = c
    return m


def mat4_rot_z(rad):
    c, s = math.cos(rad), math.sin(rad)
    m = np.eye(4, dtype=np.float64)
    m[0, 0] = c
    m[0, 1] = -s
    m[1, 0] = s
    m[1, 1] = c
    return m


def quat_to_mat(q):
    """(x,y,z,w) -> 4x4 rotation matrix."""
    x, y, z, w = [float(v) for v in q]
    n = x * x + y * y + z * z + w * w
    if n < 1e-20:
        return mat4_identity()
    s = 2.0 / n
    m = np.eye(4, dtype=np.float64)
    m[0, 0] = 1.0 - s * (y * y + z * z)
    m[0, 1] = s * (x * y - z * w)
    m[0, 2] = s * (x * z + y * w)
    m[1, 0] = s * (x * y + z * w)
    m[1, 1] = 1.0 - s * (x * x + z * z)
    m[1, 2] = s * (y * z - x * w)
    m[2, 0] = s * (x * z - y * w)
    m[2, 1] = s * (y * z + x * w)
    m[2, 2] = 1.0 - s * (x * x + y * y)
    return m


def mat3_to_quat(r):
    """Rotation matrix 3x3 (near-orthonormal) -> normalised (x,y,z,w)."""
    m = np.asarray(r, dtype=np.float64)
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        w = 0.25 * s
        x = (m[2, 1] - m[1, 2]) / s
        y = (m[0, 2] - m[2, 0]) / s
        z = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2.0
        w = (m[2, 1] - m[1, 2]) / s
        x = 0.25 * s
        y = (m[0, 1] + m[1, 0]) / s
        z = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2.0
        w = (m[0, 2] - m[2, 0]) / s
        x = (m[0, 1] + m[1, 0]) / s
        y = 0.25 * s
        z = (m[1, 2] + m[2, 1]) / s
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2.0
        w = (m[1, 0] - m[0, 1]) / s
        x = (m[0, 2] + m[2, 0]) / s
        y = (m[1, 2] + m[2, 1]) / s
        z = 0.25 * s
    q = np.array([x, y, z, w], dtype=np.float64)
    n = np.linalg.norm(q)
    return q / n if n > 1e-20 else np.array([0.0, 0.0, 0.0, 1.0])


def quat_normalize(q):
    q = np.asarray(q, dtype=np.float64)
    n = np.linalg.norm(q)
    return q / n if n > 1e-20 else np.array([0.0, 0.0, 0.0, 1.0])


def quat_slerp(a, b, alpha):
    a = quat_normalize(a)
    b = quat_normalize(b)
    d = float(np.dot(a, b))
    if d < 0.0:
        b = -b
        d = -d
    if d > 0.9995:
        return quat_normalize(a + (b - a) * alpha)
    theta = math.acos(min(1.0, max(-1.0, d)))
    sin_theta = math.sin(theta)
    wa = math.sin((1.0 - alpha) * theta) / sin_theta
    wb = math.sin(alpha * theta) / sin_theta
    return quat_normalize(a * wa + b * wb)


def quat_angle_between(a, b):
    a = quat_normalize(a)
    b = quat_normalize(b)
    d = abs(float(np.dot(a, b)))
    return 2.0 * math.acos(min(1.0, d))


def decompose_trs(m):
    """4x4 affine (uniform scale, no shear) -> (translation, quat xyzw, scale)."""
    t = m[:3, 3].astype(np.float64)
    sx = float(np.linalg.norm(m[:3, 0]))
    sy = float(np.linalg.norm(m[:3, 1]))
    sz = float(np.linalg.norm(m[:3, 2]))
    if np.linalg.det(m[:3, :3]) < 0.0:
        sx = -sx
    cols = np.array([
        m[:3, 0] / (sx if sx != 0 else 1.0),
        m[:3, 1] / (sy if sy != 0 else 1.0),
        m[:3, 2] / (sz if sz != 0 else 1.0),
    ]).T
    q = mat3_to_quat(cols)
    return t, q, np.array([sx, sy, sz], dtype=np.float64)


def trs_matrix(t, q, s):
    m = mat4_translate(float(t[0]), float(t[1]), float(t[2]))
    m = m @ quat_to_mat(q)
    m = m @ mat4_scale(float(s[0]), float(s[1]), float(s[2]))
    return m


def transform_points(m, points):
    """m (4x4) applied to Nx3 points; returns Nx3 float64."""
    pts = np.asarray(points, dtype=np.float64)
    out = np.empty_like(pts)
    out[:] = pts @ m[:3, :3].T + m[:3, 3]
    return out


def transform_normals(m, normals):
    """Normals transformed by inverse-transpose of the 3x3 part, normalised."""
    n3 = np.asarray(m, dtype=np.float64)[:3, :3]
    try:
        nm = np.linalg.inv(n3).T
    except np.linalg.LinAlgError:
        nm = n3
    out = np.asarray(normals, dtype=np.float64) @ nm.T
    lens = np.linalg.norm(out, axis=1)
    lens[lens < 1e-20] = 1.0
    return out / lens[:, None]


# ---------------------------------------------------------------------------
# sRGB helpers (vertex colours are stored as display values, i.e. sRGB)
# ---------------------------------------------------------------------------

def srgb_to_linear(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * (c ** (1.0 / 2.4)) - 0.055)


# ---------------------------------------------------------------------------
# glTF reading
# ---------------------------------------------------------------------------

_COMPONENT_DTYPES = {
    5120: np.int8,
    5121: np.uint8,
    5122: np.int16,
    5123: np.uint16,
    5125: np.uint32,
    5126: np.float32,
}
_TYPE_COMPONENTS = {
    "SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
    "MAT2": 4, "MAT3": 9, "MAT4": 16,
}


class GltfSource:
    """Thin, strict-enough GLB reader on top of pygltflib."""

    def __init__(self, path):
        self.path = path
        self.gltf = GLTF2().load(path)
        self.raw = self.gltf.binary_blob() or b""
        self.parents = {}
        for index, node in enumerate(self.gltf.nodes):
            for child in node.children or []:
                self.parents[child] = index
        self._world = [None] * len(self.gltf.nodes)
        self._images = {}
        self.warnings = []

    # -- nodes ---------------------------------------------------------
    def node(self, index):
        return self.gltf.nodes[index]

    def node_name(self, index):
        name = self.node(index).name
        return name if name else f"node{index}"

    def node_local(self, index):
        node = self.node(index)
        if node.matrix:
            raw = np.asarray(node.matrix, dtype=np.float64).reshape(4, 4).T
            return raw
        t = node.translation or [0.0, 0.0, 0.0]
        r = node.rotation or [0.0, 0.0, 0.0, 1.0]
        s = node.scale or [1.0, 1.0, 1.0]
        return trs_matrix(t, r, s)

    def world_matrix(self, index):
        if self._world[index] is None:
            local = self.node_local(index)
            parent = self.parents.get(index)
            self._world[index] = (
                self.world_matrix(parent) @ local if parent is not None else local)
        return self._world[index]

    def scene_roots(self):
        if self.gltf.scenes and self.gltf.scene is not None:
            return list(self.gltf.scenes[self.gltf.scene].nodes or [])
        return [i for i in range(len(self.gltf.nodes)) if i not in self.parents]

    # -- accessors -----------------------------------------------------
    def accessor(self, index):
        """Return an (count, ncomp) float64 array with normalized ints scaled."""
        gltf = self.gltf
        acc = gltf.accessors[index]
        if acc.bufferView is None:
            raise ValueError(f"accessor {index} has no bufferView (sparse data "
                             "is not supported)")
        view = gltf.bufferViews[acc.bufferView]
        dtype = _COMPONENT_DTYPES[acc.componentType]
        ncomp = _TYPE_COMPONENTS[acc.type]
        itemsize = np.dtype(dtype).itemsize * ncomp
        stride = view.byteStride or itemsize
        offset = (view.byteOffset or 0) + (acc.byteOffset or 0)
        buffer_index = view.buffer or 0
        if buffer_index != 0:
            raise ValueError("only GLB buffer 0 is supported")
        out = np.empty((acc.count, ncomp), dtype=np.float64)
        if stride == itemsize:
            flat = np.frombuffer(self.raw, dtype=dtype,
                                 count=acc.count * ncomp, offset=offset)
            out = flat.reshape(acc.count, ncomp).astype(np.float64)
        else:
            raw = np.frombuffer(self.raw, dtype=np.uint8,
                                count=stride * acc.count + itemsize,
                                offset=offset)
            for row in range(acc.count):
                chunk = raw[row * stride:row * stride + itemsize]
                out[row] = np.frombuffer(chunk.tobytes(), dtype=dtype,
                                         count=ncomp)
        if acc.normalized:
            info = np.iinfo(dtype)
            out = np.clip(out / float(info.max), -1.0, 1.0)
        return out

    def accessor_raw(self, index):
        """Same as accessor() but keeps the sample dtype for indices."""
        return self.accessor(index)

    # -- skins ---------------------------------------------------------
    def inverse_bind_matrices(self, skin_index):
        skin = self.gltf.skins[skin_index]
        if skin.inverseBindMatrices is None:
            return [np.eye(4) for _ in skin.joints]
        raw = self.accessor(skin.inverseBindMatrices)
        return [np.asarray(raw[i], dtype=np.float64).reshape(4, 4).T
                for i in range(raw.shape[0])]

    # -- materials / textures -----------------------------------------
    def image_rgb(self, image_index):
        """Decode a glTF image into an HxWx3 float array in sRGB [0,1]."""
        if image_index in self._images:
            return self._images[image_index]
        gltf = self.gltf
        img = gltf.images[image_index]
        if img.bufferView is not None:
            view = gltf.bufferViews[img.bufferView]
            data = self.raw[view.byteOffset:view.byteOffset + view.byteLength]
            pil = Image.open(_BytesIO(data))
        elif img.uri:
            base = os.path.dirname(os.path.abspath(self.path))
            with open(os.path.join(base, img.uri), "rb") as fh:
                pil = Image.open(_BytesIO(fh.read()))
        else:
            raise ValueError(f"image {image_index} has no data")
        arr = np.asarray(pil.convert("RGB"), dtype=np.float64) / 255.0
        self._images[image_index] = arr
        return arr

    def material_base_colour(self, material_index):
        """Return (factor_rgba, sampler_wrap) for a material."""
        if material_index is None or not self.gltf.materials:
            return np.array([1.0, 1.0, 1.0, 1.0]), ("REPEAT", "REPEAT"), None
        mat = self.gltf.materials[material_index]
        pbr = mat.pbrMetallicRoughness
        factor = np.array([1.0, 1.0, 1.0, 1.0])
        texture_index = None
        if pbr is not None:
            if pbr.baseColorFactor is not None:
                factor = np.asarray(pbr.baseColorFactor, dtype=np.float64)
            if pbr.baseColorTexture is not None:
                texture_index = pbr.baseColorTexture.index
        wrap = ("REPEAT", "REPEAT")
        if texture_index is not None:
            tex = self.gltf.textures[texture_index]
            sampler = (self.gltf.samplers or [])[tex.sampler] if \
                tex.sampler is not None and self.gltf.samplers else None
            if sampler is not None:
                wrap = (_wrap_name(sampler.wrapS), _wrap_name(sampler.wrapT))
            return factor, wrap, (texture_index, tex.source)
        return factor, wrap, None

    def material_name(self, material_index):
        if material_index is None or not self.gltf.materials:
            return None
        return self.gltf.materials[material_index].name or None


def _wrap_name(value):
    return {33071: "CLAMP", 33648: "MIRROR", 10497: "REPEAT"}.get(value, "REPEAT")


class _BytesIO:
    """Tiny lazy import shim so PIL is only imported when a texture exists."""

    def __new__(cls, data):
        import io
        return io.BytesIO(data)


def sample_texture(image, uvs, wrap, filter_mode="NEAREST"):
    """Sample an HxWx3 sRGB image at Nx2 glTF UVs -> Nx3 sRGB float array."""
    h, w, _ = image.shape
    u = np.asarray(uvs[:, 0], dtype=np.float64)
    v = np.asarray(uvs[:, 1], dtype=np.float64)
    for axis, wrap_mode in enumerate(wrap):
        if wrap_mode == "REPEAT":
            pass
        elif wrap_mode == "CLAMP":
            pass
        elif wrap_mode == "MIRROR":
            pass
        # (REPEAT handled by fract below; MIRROR folded onto REPEAT+flip)
    u_rep = u - np.floor(u)
    v_rep = v - np.floor(v)
    if wrap[0] == "MIRROR":
        fold = np.floor(u) % 2.0
        u_rep = np.where(fold > 0.5, 1.0 - u_rep, u_rep)
    elif wrap[0] == "CLAMP":
        u_rep = np.clip(u, 0.0, 1.0)
    if wrap[1] == "MIRROR":
        fold = np.floor(v) % 2.0
        v_rep = np.where(fold > 0.5, 1.0 - v_rep, v_rep)
    elif wrap[1] == "CLAMP":
        v_rep = np.clip(v, 0.0, 1.0)
    # glTF UV origin is top-left, like PIL pixel rows.
    if filter_mode == "LINEAR":
        x = u_rep * (w - 1)
        y = v_rep * (h - 1)
        x0 = np.floor(x).astype(int)
        y0 = np.floor(y).astype(int)
        x1 = np.clip(x0 + 1, 0, w - 1)
        y1 = np.clip(y0 + 1, 0, h - 1)
        fx = (x - x0)[:, None]
        fy = (y - y0)[:, None]
        c00 = image[y0, x0]
        c10 = image[y0, x1]
        c01 = image[y1, x0]
        c11 = image[y1, x1]
        return (c00 * (1 - fx) * (1 - fy) + c10 * fx * (1 - fy) +
                c01 * (1 - fx) * fy + c11 * fx * fy)
    x = np.clip((u_rep * w).astype(int), 0, w - 1)
    y = np.clip((v_rep * h).astype(int), 0, h - 1)
    return image[y, x]


# ---------------------------------------------------------------------------
# built model representation
# ---------------------------------------------------------------------------

@dataclass
class Bone:
    name: str
    parent: int
    translation: np.ndarray
    rotation: np.ndarray        # xyzw, unit
    scale: np.ndarray


@dataclass
class Key:
    time: float
    value: np.ndarray           # xyzw for rotation; xyz + 0 for T/S


@dataclass
class Track:
    bone: int
    component: int
    keys: list = field(default_factory=list)


@dataclass
class Clip:
    role: str
    duration: float
    tracks: list = field(default_factory=list)


@dataclass
class GpmModel:
    positions: np.ndarray       # Nx3 float32
    normals: np.ndarray         # Nx3 float32
    colours: np.ndarray         # Nx4 uint8
    vertex_bones: np.ndarray    # N uint16
    indices: np.ndarray         # M uint16
    bones: list
    clips: list
    credit: str = ""
    bbox_min: np.ndarray = None
    bbox_max: np.ndarray = None

    @property
    def vertex_count(self):
        return int(self.positions.shape[0])

    @property
    def index_count(self):
        return int(self.indices.shape[0])

    @property
    def triangle_count(self):
        return self.index_count // 3

    @property
    def bone_count(self):
        return len(self.bones)

    @property
    def anim_count(self):
        return len(self.clips)


# ---------------------------------------------------------------------------
# building the model from glTF
# ---------------------------------------------------------------------------

class Importer:
    def __init__(self, glb_path, config, args):
        self.src = GltfSource(glb_path)
        self.config = config
        self.args = args
        self.errors = []
        self.warnings = []
        self.kind = (args.kind or config.get("kind") or "").lower()
        if self.kind not in KIND_TARGETS:
            self.fail(f"config/--kind must be one of {sorted(KIND_TARGETS)}, "
                      f"got {self.kind!r}")
        self.parts = list(config.get("parts") or [])
        self.overrides = {}
        for key, value in (config.get("colour_overrides") or {}).items():
            rgba = np.asarray(value, dtype=np.float64)
            if rgba.shape[0] == 3:
                rgba = np.concatenate([rgba, [1.0]])
            if rgba.shape[0] != 4:
                self.warn(f"colour_overrides[{key!r}] must be [r,g,b] or "
                          "[r,g,b,a]; ignoring")
                continue
            self.overrides[key] = np.clip(rgba, 0.0, 1.0)
        self.texture_filter = str(
            config.get("texture_filter", "nearest")).upper()
        if self.texture_filter not in ("NEAREST", "LINEAR"):
            self.warn(f"texture_filter {self.texture_filter!r} is not "
                      "NEAREST/LINEAR; using NEAREST")
            self.texture_filter = "NEAREST"

    # -- diagnostics ---------------------------------------------------
    def warn(self, message):
        self.warnings.append(message)

    def fail(self, message):
        self.errors.append(message)
        raise SystemExit(f"error: {message}")

    # -- selection -----------------------------------------------------
    def selected_mesh_nodes(self):
        nodes = []
        for index, node in enumerate(self.src.gltf.nodes):
            if node.mesh is None:
                continue
            name = self.src.node_name(index)
            if self.parts and name not in self.parts:
                continue
            nodes.append(index)
        if self.parts:
            found = {self.src.node_name(i) for i in nodes}
            for requested in self.parts:
                if requested not in found:
                    self.warn(f"config part {requested!r} does not name a mesh "
                              "node in this GLB")
        return nodes

    # -- skins / bones -------------------------------------------------
    def collect_skins(self, mesh_nodes):
        skin_ids = []
        for node_index in mesh_nodes:
            node = self.src.node(node_index)
            if node.skin is not None and node.skin not in skin_ids:
                skin_ids.append(node.skin)
        if not skin_ids:
            # static model: also consider the whole file's skins for bones?
            return []
        return skin_ids

    def build_bones(self, skin_ids, model_matrix):
        """Merge skins by joint name; return (bones, name->index, root_bases)."""
        gltf = self.src.gltf
        name_to_index = {}
        name_to_node = {}
        joint_nodes = []
        for skin_id in skin_ids:
            skin = gltf.skins[skin_id]
            for joint_node in skin.joints:
                name = self.src.node_name(joint_node)
                if name in name_to_index:
                    if name_to_node[name] != joint_node:
                        self.warn(f"skins disagree about node for joint "
                                  f"{name!r}; using the first one")
                    continue
                name_to_index[name] = len(joint_nodes)
                name_to_node[name] = joint_node
                joint_nodes.append(joint_node)

        # nearest-joint-parent + roots (parent-before-child order).
        joint_set = set(joint_nodes)
        parent_of = {}
        children = {i: [] for i in range(len(joint_nodes))}
        roots = []
        for merged_index, joint_node in enumerate(joint_nodes):
            parent = self.src.parents.get(joint_node)
            while parent is not None and parent not in joint_set:
                parent = self.src.parents.get(parent)
            if parent is None:
                parent_of[merged_index] = -1
                roots.append(merged_index)
            else:
                parent_name = self.src.node_name(parent)
                parent_of[merged_index] = name_to_index[parent_name]
                children[name_to_index[parent_name]].append(merged_index)

        order = []

        def visit(index):
            order.append(index)
            for child in sorted(children[index]):
                visit(child)

        for root in roots:
            visit(root)
        if len(order) != len(joint_nodes):
            # cycles - should never happen; fall back to file order
            self.warn("joint hierarchy has a cycle; falling back to node order")
            order = list(range(len(joint_nodes)))

        bones = []
        root_bases = {}
        for merged_index in order:
            joint_node = joint_nodes[merged_index]
            world = self.src.world_matrix(joint_node)
            parent = parent_of[merged_index]
            if parent < 0:
                # root local = model matrix * (full native world transform)
                local = model_matrix @ world
                root_bases[merged_index] = model_matrix @ self._ancestor_matrix(
                    joint_node)
            else:
                parent_world = self.src.world_matrix(
                    joint_nodes[parent])
                try:
                    local = np.linalg.inv(parent_world) @ world
                except np.linalg.LinAlgError:
                    self.fail(f"singular rest matrix for joint "
                              f"{self.src.node_name(joint_node)!r}")
            translation, rotation, scale = decompose_trs(local)
            bones.append(Bone(self.src.node_name(joint_node), -1,
                              translation, rotation, scale))
        # renumber parents according to the final order
        remap = {old: new for new, old in enumerate(order)}
        for new_index, old_index in enumerate(order):
            parent = parent_of[old_index]
            bones[new_index].parent = -1 if parent < 0 else remap[parent]
        new_index_by_old = remap
        name_to_final = {bones[new_index_by_old[old]].name: new_index_by_old[old]
                         for old in range(len(order))}
        # root bases keyed by final index
        final_root_bases = {new_index_by_old[old]: base
                            for old, base in root_bases.items()}
        return bones, name_to_final, final_root_bases

    def _ancestor_matrix(self, node_index):
        """Product of transforms of the scene ancestors of `node_index`."""
        chain = []
        parent = self.src.parents.get(node_index)
        while parent is not None:
            chain.append(parent)
            parent = self.src.parents.get(parent)
        m = mat4_identity()
        for index in reversed(chain):
            m = m @ self.src.node_local(index)
        return m

    # -- raw vertices --------------------------------------------------
    def collect_vertices(self, mesh_nodes, name_to_bone, bones):
        """Return per-part lists of (positions, normals, uvs, colours, indices)."""
        gltf = self.src.gltf
        parts = []
        total_vertices = 0
        for node_index in mesh_nodes:
            node = self.src.node(node_index)
            mesh = gltf.meshes[node.mesh]
            mesh_name = self.src.node_name(node_index)
            world = self.src.world_matrix(node_index)
            skin_id = node.skin
            skin_joints = gltf.skins[skin_id].joints if skin_id is not None else []
            ibms = self.src.inverse_bind_matrices(skin_id) if skin_id is not None else []
            for prim_index, prim in enumerate(mesh.primitives):
                attrs = prim.attributes
                position_index = getattr(attrs, "POSITION", None)
                if position_index is None:
                    self.warn(f"{mesh_name} prim{prim_index}: no POSITION; "
                              "skipping")
                    continue
                positions = self.src.accessor(position_index)
                normal_index = getattr(attrs, "NORMAL", None)
                if normal_index is not None:
                    normals = self.src.accessor(normal_index)
                else:
                    normals = None
                uv_index = getattr(attrs, "TEXCOORD_0", None)
                uvs = self.src.accessor(uv_index) if uv_index is not None else None
                colour_index = getattr(attrs, "COLOR_0", None)
                colours = (self.src.accessor(colour_index)
                           if colour_index is not None else None)
                count = positions.shape[0]
                if count == 0:
                    continue
                if prim.indices is not None:
                    indices = self.src.accessor(prim.indices).astype(np.int64)
                    indices = indices.reshape(-1)
                else:
                    indices = np.arange(count, dtype=np.int64)

                # dominant bone + rest transform per vertex
                bones_idx = np.zeros(count, dtype=np.int64)
                rest_positions = positions
                rest_normals = normals
                if skin_id is not None:
                    joints_acc = getattr(attrs, "JOINTS_0", None)
                    weights_acc = getattr(attrs, "WEIGHTS_0", None)
                    if joints_acc is None or weights_acc is None:
                        self.fail(f"{mesh_name} prim{prim_index} is skinned but "
                                  "has no JOINTS_0/WEIGHTS_0")
                    joints = self.src.accessor(joints_acc).astype(np.int64)
                    weights = self.src.accessor(weights_acc)
                    dominant = weights.argmax(axis=1)
                    local_joint = joints[np.arange(count), dominant]
                    rest_positions = np.empty_like(positions)
                    rest_normals = (np.empty_like(normals) if normals is not None
                                    else None)
                    cache = {}
                    for vertex in range(count):
                        local = int(local_joint[vertex])
                        if local < 0 or local >= len(skin_joints):
                            self.fail(f"{mesh_name} prim{prim_index}: joint "
                                      f"index {local} out of range")
                        joint_node = skin_joints[local]
                        joint_name = self.src.node_name(joint_node)
                        bone_index = name_to_bone.get(joint_name)
                        if bone_index is None:
                            self.fail(f"vertex references joint {joint_name!r} "
                                      "that is not in the merged bone table")
                        bones_idx[vertex] = bone_index
                        key = (local, bone_index)
                        skin_matrix = cache.get(key)
                        if skin_matrix is None:
                            skin_matrix = (self.src.world_matrix(joint_node) @
                                           ibms[local])
                            cache[key] = skin_matrix
                        p = skin_matrix @ np.append(positions[vertex], 1.0)
                        rest_positions[vertex] = p[:3]
                        if normals is not None:
                            rn = skin_matrix[:3, :3] @ normals[vertex]
                            ln = np.linalg.norm(rn)
                            rest_normals[vertex] = rn / ln if ln > 1e-12 else rn
                else:
                    rest_positions = transform_points(world, positions)
                    if normals is not None:
                        rest_normals = transform_normals(world, normals)

                # material colour
                factor, wrap, texture = self.src.material_base_colour(
                    prim.material)
                material_name = self.src.material_name(prim.material)
                override = self._override_colour(mesh_name, material_name)
                if override is not None:
                    display = np.broadcast_to(override, (count, 4)).copy()
                else:
                    linear = np.broadcast_to(
                        srgb_to_linear(factor[:3]), (count, 3)).copy()
                    if texture is not None:
                        tex_index, image_index = texture
                        if uvs is None:
                            self.warn(f"{mesh_name} prim{prim_index}: textured "
                                      "material but no TEXCOORD_0; using factor")
                        else:
                            image = self.src.image_rgb(image_index)
                            tex_srgb = sample_texture(image, uvs, wrap,
                                                      self.texture_filter)
                            linear = linear * srgb_to_linear(tex_srgb)
                    if colours is not None:
                        linear = linear * np.clip(colours[:, :3], 0.0, 1.0)
                    display = np.empty((count, 4))
                    display[:, :3] = linear_to_srgb(linear)
                    display[:, 3] = factor[3]
                if colours is not None and override is None:
                    display[:, 3] *= np.clip(colours[:, 3], 0.0, 1.0)
                display[:, 3] = np.clip(display[:, 3], 0.0, 1.0)
                colour_u8 = np.rint(display * 255.0).astype(np.uint8)

                parts.append({
                    "node": node_index,
                    "name": mesh_name,
                    "prim": prim_index,
                    "positions": rest_positions,
                    "normals": rest_normals,
                    "uvs": uvs,
                    "colours": colour_u8,
                    "indices": indices,
                    "bones": bones_idx,
                })
                total_vertices += count
        if total_vertices == 0:
            self.fail("no vertices selected - check the config 'parts' list")
        return parts

    def _override_colour(self, mesh_name, material_name):
        if mesh_name in self.overrides:
            return self.overrides[mesh_name]
        if material_name and material_name in self.overrides:
            return self.overrides[material_name]
        if "*" in self.overrides:
            return self.overrides["*"]
        return None

    # -- model transform ----------------------------------------------
    def compute_model_matrix(self, parts):
        """Normalise to metres, apply yaw, ground characters / centre guns."""
        positions = []
        for part in parts:
            positions.append(part["positions"])
        all_positions = np.concatenate(positions, axis=0)

        kind = self.kind
        target_axis, target_value = KIND_TARGETS[kind]
        if kind == "hero" and "target_height" in self.config:
            target_value = float(self.config["target_height"])
        if kind == "zombie" and "target_height" in self.config:
            target_value = float(self.config["target_height"])
        if kind == "weapon" and "target_length" in self.config:
            target_value = float(self.config["target_length"])
        config_scale = float(self.config.get("scale", 1.0))
        yaw = math.radians(float(self.config.get("yaw", 0.0)))
        offset = np.asarray(self.config.get("offset", [0.0, 0.0, 0.0]),
                            dtype=np.float64)

        # Weapons: rotate the long axis onto +Z and point the muzzle at +Z,
        # before measuring/centring.
        orientation = mat4_identity()
        if kind == "weapon":
            orientation = self._weapon_orientation(all_positions)
        oriented = transform_points(orientation, all_positions) if kind == "weapon" \
            else all_positions

        native_min = oriented.min(axis=0)
        native_max = oriented.max(axis=0)
        native_size = np.maximum(native_max - native_min, 1e-6)
        if target_axis == "height":
            extent = float(native_size[1])
        else:
            extent = float(native_size.max())
        base_scale = target_value / extent
        final_scale = base_scale * config_scale
        transform = (mat4_rot_y(yaw) @ mat4_scale(final_scale, final_scale,
                                                  final_scale) @ orientation)
        transformed = transform_points(transform, all_positions)
        bmin = transformed.min(axis=0)
        bmax = transformed.max(axis=0)
        if kind == "weapon":
            centre = (bmin + bmax) * 0.5
            shift = np.array([-centre[0], -centre[1], -centre[2]]) + offset
        else:
            # ground the feet and centre the footprint on the origin
            shift = np.array([
                -(bmin[0] + bmax[0]) * 0.5,
                -bmin[1],
                -(bmin[2] + bmax[2]) * 0.5,
            ]) + offset
        model_matrix = mat4_translate(*shift) @ transform
        stats = {
            "native_size": native_size,
            "base_scale": base_scale,
            "config_scale": config_scale,
            "scale": final_scale,
            "yaw": float(self.config.get("yaw", 0.0)),
            "target": (target_axis, target_value),
            "weapon_orientation": orientation,
        }
        return model_matrix, stats

    def _weapon_orientation(self, positions):
        bmin = positions.min(axis=0)
        bmax = positions.max(axis=0)
        size = bmax - bmin
        long_axis = int(size.argmax())
        rotation = mat4_identity()
        if long_axis == 0:      # X -> +Z
            rotation = mat4_rot_y(math.radians(-90.0))
        elif long_axis == 1:    # Y -> +Z
            rotation = mat4_rot_x(math.radians(90.0))
        # long axis already Z: nothing to do
        oriented = transform_points(rotation, positions)
        obmin = oriented.min(axis=0)
        obmax = oriented.max(axis=0)
        # Muzzle = the thinner end of the long (now Z) axis.
        zmin, zmax = obmin[2], obmax[2]
        span = max(zmax - zmin, 1e-9)
        slab = span * 0.18
        lo = oriented[oriented[:, 2] <= zmin + slab]
        hi = oriented[oriented[:, 2] >= zmax - slab]
        lo_width = (np.ptp(lo[:, 0]) + np.ptp(lo[:, 1])) if len(lo) else 0.0
        hi_width = (np.ptp(hi[:, 0]) + np.ptp(hi[:, 1])) if len(hi) else 0.0
        muzzle_at_negative = lo_width < hi_width
        if muzzle_at_negative:
            rotation = mat4_rot_y(math.pi) @ rotation
        self.weapon_muzzle_sign = -1.0 if muzzle_at_negative else 1.0
        self.weapon_long_axis = long_axis
        return rotation

    # -- animations ----------------------------------------------------
    def build_animations(self, bones, name_to_bone, root_bases, resample_hz,
                         no_thin=False):
        gltf = self.src.gltf
        role_map = self.config.get("animations") or {}
        if not role_map:
            return []
        animations_by_name = {}
        for anim in gltf.animations or []:
            animations_by_name[anim.name] = anim
        clips = []
        for role, source_name in role_map.items():
            if role not in ROLE_NAMES:
                self.fail(f"unknown animation role {role!r} in config "
                          f"(known: {sorted(ROLE_NAMES)})")
            anim = animations_by_name.get(source_name)
            if anim is None:
                available = ", ".join(sorted(animations_by_name)) or "(none)"
                self.fail(f"config maps {role!r} -> {source_name!r}, but the GLB "
                          f"has no such animation. Available: {available}")
            if resample_hz <= 0:
                self.fail(f"animation role {role!r} requested but "
                          "resample_hz is 0")
            clips.append(self._sample_clip(anim, role, bones, name_to_bone,
                                           root_bases, resample_hz, no_thin))
        return clips

    def _channels_by_bone(self, anim, name_to_bone):
        by_bone = {}
        for channel in anim.channels:
            target = channel.target
            if target.node is None or target.path is None:
                continue
            node_name = self.src.node_name(target.node)
            bone_index = name_to_bone.get(node_name)
            if bone_index is None:
                self.warn(f"animation {anim.name!r} targets {node_name!r} "
                          "which is not an exported bone; skipping channel")
                continue
            if target.path not in ("translation", "rotation", "scale"):
                continue
            sampler = anim.samplers[channel.sampler]
            by_bone.setdefault(bone_index, {})[target.path] = sampler
        return by_bone

    def _sample_clip(self, anim, role, bones, name_to_bone, root_bases,
                     resample_hz, no_thin):
        gltf = self.src.gltf
        duration = 0.0
        for sampler in anim.samplers:
            acc = gltf.accessors[sampler.input]
            if acc.max:
                duration = max(duration, float(acc.max[0]))
        if duration < 0.0:
            duration = 0.0
        sample_count = int(math.floor(duration * resample_hz + 1e-6)) + 1
        times = [min(k / float(resample_hz), duration)
                 for k in range(sample_count)]
        if times and times[-1] < duration - 1e-6:
            times.append(duration)

        channels = self._channels_by_bone(anim, name_to_bone)
        clip = Clip(role=role, duration=float(times[-1]) if times else 0.0)
        for bone_index in sorted(channels):
            components = channels[bone_index]
            rest = bones[bone_index]
            for path, component in (("translation", COMPONENT_TRANSLATION),
                                    ("rotation", COMPONENT_ROTATION),
                                    ("scale", COMPONENT_SCALE)):
                sampler = components.get(path)
                if sampler is None:
                    continue
                values = self._sample_channel(sampler, path, times)
                if bone_index in root_bases:
                    values = self._compose_root(root_bases[bone_index], path,
                                                values, rest)
                keys = [Key(time=t, value=v) for t, v in zip(times, values)]
                if not no_thin:
                    keys = thin_keys(keys, component)
                if len(keys) > 0:
                    clip.tracks.append(Track(bone=bone_index,
                                             component=component, keys=keys))
        return clip

    def _sample_channel(self, sampler, path, times):
        gltf = self.src.gltf
        input_acc = gltf.accessors[sampler.input]
        output_acc = gltf.accessors[sampler.output]
        src_times = self.src.accessor(sampler.input).reshape(-1)
        values = self.src.accessor(sampler.output)
        interpolation = (sampler.interpolation or "LINEAR").upper()
        if interpolation == "CUBICSPLINE":
            # value lives in the middle of each in-tangent/value/out-tangent
            # triple; tangents are ignored (linear between key values).
            values = values.reshape(-1, 3, values.shape[1])[:, 1, :]
        if values.shape[0] != src_times.shape[0]:
            if interpolation == "CUBICSPLINE":
                pass
            else:
                self.fail("animation sampler input/output key counts differ")
        n = src_times.shape[0]
        out = []
        for t in times:
            if n == 0:
                out.append(np.zeros(4))
                continue
            if t <= src_times[0]:
                output = np.zeros(values.shape[0])
                out.append(_normalise_sample(values[0], path))
                continue
            if t >= src_times[-1]:
                out.append(_normalise_sample(values[-1], path))
                continue
            index = int(np.searchsorted(src_times, t, side="right") - 1)
            index = max(0, min(index, n - 2))
            t0 = float(src_times[index])
            t1 = float(src_times[index + 1])
            alpha = 0.0 if t1 - t0 <= 1e-9 else (t - t0) / (t1 - t0)
            v0 = values[index]
            v1 = values[index + 1]
            if interpolation == "STEP":
                value = v0
            elif path == "rotation":
                value = quat_slerp(v0, v1, alpha)
            else:
                value = v0 + (v1 - v0) * alpha
            out.append(_normalise_sample(value, path))
        # quaternion sign continuity across the resampled keys
        if path == "rotation":
            for k in range(1, len(out)):
                if float(np.dot(out[k], out[k - 1])) < 0.0:
                    out[k] = -out[k]
        return out

    def _compose_root(self, root_base, path, values, rest):
        """Root tracks carry the whole model transform: GPM = Q * (T R S)."""
        composed = []
        rest_t = rest.translation
        rest_q = rest.rotation
        rest_s = rest.scale
        for value in values:
            if path == "translation":
                t, q, s = value[:3], rest_q, rest_s
            elif path == "rotation":
                t, q, s = rest_t, value, rest_s
            else:
                t, q, s = rest_t, rest_q, value[:3]
            local = trs_matrix(t, q, s)
            final = root_base @ local
            ft, fq, fs = decompose_trs(final)
            if path == "translation":
                composed.append(np.array([ft[0], ft[1], ft[2], 0.0]))
            elif path == "rotation":
                composed.append(fq)
            else:
                composed.append(np.array([fs[0], fs[1], fs[2], 0.0]))
        return composed


def _normalise_sample(value, path):
    value = np.asarray(value, dtype=np.float64)
    if path == "rotation":
        return quat_normalize(value[:4])
    out = np.zeros(4)
    out[:3] = value[:3]
    return out


def thin_keys(keys, component):
    """Drop interior keys that a linear/slerp segment reproduces within eps."""
    if len(keys) <= 2:
        return keys
    out = [keys[0]]
    for index in range(1, len(keys) - 1):
        prev = out[-1]
        nxt = keys[index + 1]
        span = nxt.time - prev.time
        alpha = 0.0 if span <= 1e-9 else (keys[index].time - prev.time) / span
        if component == COMPONENT_ROTATION:
            predicted = quat_slerp(prev.value, nxt.value, alpha)
            error = quat_angle_between(predicted, keys[index].value)
            keep = error > THIN_ROTATION_EPS
        elif component == COMPONENT_TRANSLATION:
            predicted = prev.value + (nxt.value - prev.value) * alpha
            keep = np.linalg.norm(predicted - keys[index].value) > \
                THIN_TRANSLATION_EPS
        else:
            predicted = prev.value + (nxt.value - prev.value) * alpha
            keep = np.linalg.norm(predicted - keys[index].value) > THIN_SCALE_EPS
        if keep:
            out.append(keys[index])
    out.append(keys[-1])
    return out


# ---------------------------------------------------------------------------
# GPM writing
# ---------------------------------------------------------------------------

def _align4(value):
    return (value + 3) & ~3


def write_gpm(model, out_path):
    bone_names = [bone.name for bone in model.bones]
    clip_names = [clip.role for clip in model.clips]
    strings = bytearray()
    bone_name_offsets = []
    for name in bone_names:
        bone_name_offsets.append(len(strings))
        strings += name.encode("utf-8") + b"\x00"
    clip_name_offsets = []
    for name in clip_names:
        clip_name_offsets.append(len(strings))
        strings += name.encode("utf-8") + b"\x00"
    credit = model.credit or ""
    if credit:
        strings += credit.encode("utf-8") + b"\x00"

    track_records = []
    key_records = []
    animation_records = []
    track_first = 0
    for clip in model.clips:
        animation_records.append((clip.duration, track_first, len(clip.tracks)))
        for track in clip.tracks:
            track_records.append((track.bone, track.component, len(track.keys)))
            for key in track.keys:
                key_records.append((key.time, key.value))
        track_first += len(clip.tracks)

    off_strings = HEADER_SIZE
    pos = _align4(off_strings + len(strings))
    off_sections = pos
    pos += SECTION_TABLE_SIZE
    off_vertices = pos
    pos += model.vertex_count * VERTEX_SIZE
    off_indices = pos
    pos += model.index_count * 2
    pos = _align4(pos)
    off_bones = pos
    pos += model.bone_count * BONE_SIZE
    off_anims = pos
    pos += model.anim_count * ANIMATION_SIZE
    off_tracks = pos
    pos += len(track_records) * TRACK_SIZE
    off_keys = pos
    pos += len(key_records) * KEY_SIZE
    file_size = pos

    flags = 0
    if model.bone_count > 0:
        flags |= FLAG_SKINNED
    if model.anim_count > 0:
        flags |= FLAG_HAS_ANIMATIONS

    header = struct.pack(
        "<7I6f3I", GPM_MAGIC, GPM_VERSION, flags, model.vertex_count,
        model.index_count, model.bone_count, model.anim_count,
        float(model.bbox_min[0]), float(model.bbox_min[1]),
        float(model.bbox_min[2]), float(model.bbox_max[0]),
        float(model.bbox_max[1]), float(model.bbox_max[2]),
        off_strings, off_sections, file_size)
    assert len(header) == HEADER_SIZE, len(header)

    vertices = np.zeros(model.vertex_count, dtype=VERTEX_DTYPE)
    vertices["px"] = model.positions[:, 0]
    vertices["py"] = model.positions[:, 1]
    vertices["pz"] = model.positions[:, 2]
    vertices["nx"] = model.normals[:, 0]
    vertices["ny"] = model.normals[:, 1]
    vertices["nz"] = model.normals[:, 2]
    vertices["r"] = model.colours[:, 0]
    vertices["g"] = model.colours[:, 1]
    vertices["b"] = model.colours[:, 2]
    vertices["a"] = model.colours[:, 3]
    vertices["bone"] = model.vertex_bones
    vertices["pad"] = 0

    indices = np.asarray(model.indices, dtype="<u2")

    bones = np.zeros(model.bone_count, dtype=BONE_DTYPE)
    for index, bone in enumerate(model.bones):
        bones["parent"][index] = bone.parent
        bones["tx"][index] = bone.translation[0]
        bones["ty"][index] = bone.translation[1]
        bones["tz"][index] = bone.translation[2]
        bones["rx"][index] = bone.rotation[0]
        bones["ry"][index] = bone.rotation[1]
        bones["rz"][index] = bone.rotation[2]
        bones["rw"][index] = bone.rotation[3]
        bones["sx"][index] = bone.scale[0]
        bones["sy"][index] = bone.scale[1]
        bones["sz"][index] = bone.scale[2]
        bones["name_off"][index] = bone_name_offsets[index]

    anims = np.zeros(model.anim_count, dtype=ANIM_DTYPE)
    for index, (duration, first, count) in enumerate(animation_records):
        anims["name_off"][index] = clip_name_offsets[index]
        anims["duration"][index] = duration
        anims["track_first"][index] = first
        anims["track_count"][index] = count

    tracks = np.zeros(len(track_records), dtype=TRACK_DTYPE)
    for index, (bone, component, count) in enumerate(track_records):
        tracks["bone"][index] = bone
        tracks["component"][index] = component
        tracks["key_count"][index] = count

    keys = np.zeros(len(key_records), dtype=KEY_DTYPE)
    for index, (time, value) in enumerate(key_records):
        keys["time"][index] = time
        keys["vx"][index] = value[0]
        keys["vy"][index] = value[1]
        keys["vz"][index] = value[2]
        keys["vw"][index] = value[3] if len(value) > 3 else 0.0

    sections = struct.pack("<6I", off_vertices, off_indices, off_bones,
                           off_anims, off_tracks, off_keys)
    assert len(sections) == SECTION_TABLE_SIZE

    out = bytearray(file_size)
    out[0:HEADER_SIZE] = header
    out[off_strings:off_strings + len(strings)] = strings
    out[off_sections:off_sections + SECTION_TABLE_SIZE] = sections
    out[off_vertices:off_vertices + vertices.nbytes] = vertices.tobytes()
    out[off_indices:off_indices + indices.nbytes] = indices.tobytes()
    out[off_bones:off_bones + bones.nbytes] = bones.tobytes()
    out[off_anims:off_anims + anims.nbytes] = anims.tobytes()
    out[off_tracks:off_tracks + tracks.nbytes] = tracks.tobytes()
    out[off_keys:off_keys + keys.nbytes] = keys.tobytes()

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(out)
    return file_size


# ---------------------------------------------------------------------------
# GPM reading / validation (also used by tools/gpm_selftest.py)
# ---------------------------------------------------------------------------

@dataclass
class GpmBone:
    parent: int
    translation: np.ndarray
    rotation: np.ndarray
    scale: np.ndarray
    name: str


@dataclass
class GpmTrack:
    bone: int
    component: int
    times: np.ndarray
    values: np.ndarray        # Nx4


@dataclass
class GpmClip:
    name: str
    duration: float
    tracks: list


@dataclass
class GpmFile:
    path: str
    flags: int
    positions: np.ndarray
    normals: np.ndarray
    colours: np.ndarray
    bones_idx: np.ndarray
    indices: np.ndarray
    bones: list
    clips: list
    credit: str
    bbox_min: np.ndarray
    bbox_max: np.ndarray
    file_size: int
    section_offsets: tuple

    @property
    def vertex_count(self):
        return int(self.positions.shape[0])

    @property
    def index_count(self):
        return int(self.indices.shape[0])

    @property
    def triangle_count(self):
        return self.index_count // 3

    @property
    def bone_count(self):
        return len(self.bones)

    @property
    def anim_count(self):
        return len(self.clips)

    @property
    def skinned(self):
        return bool(self.flags & FLAG_SKINNED)


def read_gpm(path):
    """Parse a GPM v1 file. Raises ValueError on structurally broken input."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{path}: too small for a GPM header ({len(data)} bytes)")
    (magic, version, flags, vertex_count, index_count, bone_count, anim_count,
     bmin0, bmin1, bmin2, bmax0, bmax1, bmax2, off_strings,
     off_sections, file_size) = struct.unpack_from("<7I6f3I", data, 0)
    if magic != GPM_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08X} (expected 0x"
                         f"{GPM_MAGIC:08X})")
    if version != GPM_VERSION:
        raise ValueError(f"{path}: unsupported version {version}")
    if file_size != len(data):
        raise ValueError(f"{path}: header file_size {file_size} != actual "
                         f"{len(data)}")
    if off_sections + SECTION_TABLE_SIZE > len(data):
        raise ValueError(f"{path}: section table out of bounds")
    (off_vertices, off_indices, off_bones, off_anims, off_tracks,
     off_keys) = struct.unpack_from("<6I", data, off_sections)

    def check_range(offset, size, what):
        if offset < HEADER_SIZE or offset + size > len(data):
            raise ValueError(f"{path}: {what} range [{offset},{offset+size}) "
                             f"outside file")

    check_range(off_vertices, vertex_count * VERTEX_SIZE, "vertices")
    check_range(off_indices, index_count * 2, "indices")
    check_range(off_bones, bone_count * BONE_SIZE, "bones")
    check_range(off_anims, anim_count * ANIMATION_SIZE, "animations")
    check_range(off_tracks, 0, "tracks")

    positions = np.zeros((vertex_count, 3), dtype=np.float32)
    normals = np.zeros((vertex_count, 3), dtype=np.float32)
    colours = np.zeros((vertex_count, 4), dtype=np.uint8)
    bones_idx = np.zeros(vertex_count, dtype=np.uint16)
    if vertex_count:
        verts = np.frombuffer(data, dtype=VERTEX_DTYPE, count=vertex_count,
                              offset=off_vertices)
        positions[:, 0] = verts["px"]
        positions[:, 1] = verts["py"]
        positions[:, 2] = verts["pz"]
        normals[:, 0] = verts["nx"]
        normals[:, 1] = verts["ny"]
        normals[:, 2] = verts["nz"]
        for channel, name in enumerate("rgba"):
            colours[:, channel] = verts[name]
        bones_idx[:] = verts["bone"]

    indices = (np.frombuffer(data, dtype="<u2", count=index_count,
                             offset=off_indices).copy()
               if index_count else np.zeros(0, dtype=np.uint16))

    # strings: bone names, animation names, then optional credit
    def read_cstring(offset):
        end = data.find(b"\x00", offset)
        if end < 0:
            raise ValueError(f"{path}: unterminated string at {offset}")
        return data[offset:end].decode("utf-8", errors="replace")

    bone_records = []
    if bone_count:
        raw = np.frombuffer(data, dtype=BONE_DTYPE, count=bone_count,
                            offset=off_bones)
        for i in range(bone_count):
            bone_records.append((
                int(raw["parent"][i]),
                np.array([raw["tx"][i], raw["ty"][i], raw["tz"][i]],
                         dtype=np.float64),
                np.array([raw["rx"][i], raw["ry"][i], raw["rz"][i],
                          raw["rw"][i]], dtype=np.float64),
                np.array([raw["sx"][i], raw["sy"][i], raw["sz"][i]],
                         dtype=np.float64),
                int(raw["name_off"][i])))
    bone_names = []
    for parent, _t, _r, _s, name_off in bone_records:
        absolute = off_strings + name_off
        if not (off_strings <= absolute < len(data)):
            raise ValueError(f"{path}: bone name offset out of range "
                             f"({name_off} + off_strings {off_strings})")
        bone_names.append(read_cstring(absolute))
    bones = [GpmBone(parent=record[0], translation=record[1],
                     rotation=record[2], scale=record[3], name=names)
             for record, names in zip(bone_records, bone_names)]

    anim_records = []
    if anim_count:
        raw = np.frombuffer(data, dtype=ANIM_DTYPE, count=anim_count,
                            offset=off_anims)
        for i in range(anim_count):
            anim_records.append((
                int(raw["name_off"][i]), float(raw["duration"][i]),
                int(raw["track_first"][i]), int(raw["track_count"][i])))
    clips = []
    if anim_records:
        raw_tracks = np.frombuffer(data, dtype=TRACK_DTYPE,
                                   count=sum(r[3] for r in anim_records),
                                   offset=off_tracks)
        track_list = []
        for i in range(len(raw_tracks)):
            track_list.append({
                "bone": int(raw_tracks["bone"][i]),
                "component": int(raw_tracks["component"][i]),
                "key_count": int(raw_tracks["key_count"][i]),
            })
        key_cursor = off_keys
        for name_off, duration, track_first, track_count in anim_records:
            absolute = off_strings + name_off
            if not (off_strings <= absolute < len(data)):
                raise ValueError(f"{path}: animation name offset out of range")
            tracks = []
            clip_tracks = track_list[track_first:track_first + track_count]
            for track in clip_tracks:
                keys = np.frombuffer(data, dtype=KEY_DTYPE,
                                     count=track["key_count"],
                                     offset=key_cursor)
                key_cursor += track["key_count"] * KEY_SIZE
                times = keys["time"].astype(np.float64)
                values = np.stack([
                    keys["vx"].astype(np.float64),
                    keys["vy"].astype(np.float64),
                    keys["vz"].astype(np.float64),
                    keys["vw"].astype(np.float64),
                ], axis=1) if track["key_count"] else np.zeros((0, 4))
                tracks.append(GpmTrack(bone=track["bone"],
                                       component=track["component"],
                                       times=times, values=values))
            clips.append(GpmClip(name=read_cstring(absolute),
                                 duration=duration, tracks=tracks))

    # credit = whatever NUL-separated string follows the last bone/anim name
    last_name_offset = -1
    if anim_records:
        last_name_offset = max(record[0] for record in anim_records)
    elif bone_records:
        last_name_offset = max(record[4] for record in bone_records)
    credit = ""
    if last_name_offset >= 0:
        last_absolute = off_strings + last_name_offset
        end = data.find(b"\x00", last_absolute)
        next_off = end + 1 if end >= 0 else -1
        # the strings block ends at off_sections (the section table follows it)
        if 0 <= next_off < off_sections:
            text = read_cstring(next_off)
            if text:
                credit = text

    gpm = GpmFile(
        path=path, flags=flags, positions=positions, normals=normals,
        colours=colours, bones_idx=bones_idx, indices=indices, bones=bones,
        clips=clips, credit=credit,
        bbox_min=np.array([bmin0, bmin1, bmin2], dtype=np.float64),
        bbox_max=np.array([bmax0, bmax1, bmax2], dtype=np.float64),
        file_size=file_size,
        section_offsets=(off_vertices, off_indices, off_bones, off_anims,
                         off_tracks, off_keys),
    )
    return gpm


def validate_gpm(gpm, verbose=False):
    """Return a list of human-readable problems (empty == valid)."""
    problems = []
    warnings = []

    def bad(message):
        problems.append(message)

    if gpm.vertex_count <= 0:
        bad("vertex_count == 0")
    if gpm.index_count % 3 != 0:
        bad(f"index_count {gpm.index_count} is not a multiple of 3")
    if gpm.vertex_count >= 65536:
        bad("vertex_count must fit in u16 indices")
    if gpm.index_count and (gpm.indices.max() >= gpm.vertex_count):
        bad(f"index {int(gpm.indices.max())} out of range "
            f"(vertex_count {gpm.vertex_count})")
    if gpm.skinned != (gpm.bone_count > 0):
        bad("FLAG_SKINNED does not match bone_count")
    if bool(gpm.flags & FLAG_HAS_ANIMATIONS) != (gpm.anim_count > 0):
        bad("FLAG_HAS_ANIMATIONS does not match anim_count")
    if gpm.bone_count == 0 and gpm.vertex_count and gpm.bones_idx.max() != 0:
        bad("static model has non-zero vertex bone indices")

    if gpm.vertex_count:
        lens = np.linalg.norm(gpm.normals.astype(np.float64), axis=1)
        if np.abs(lens - 1.0).max() > 2e-2:
            bad(f"normals not unit length (max |len-1| "
                f"{np.abs(lens - 1.0).max():.4f})")
        recomputed_min = gpm.positions.min(axis=0)
        recomputed_max = gpm.positions.max(axis=0)
        if np.abs(recomputed_min - gpm.bbox_min).max() > 1e-3 or \
                np.abs(recomputed_max - gpm.bbox_max).max() > 1e-3:
            bad("header bbox does not match vertex positions")
        if np.any(~np.isfinite(gpm.positions)):
            bad("non-finite vertex positions")
        if np.any(~np.isfinite(gpm.normals)):
            bad("non-finite vertex normals")
    if np.any(gpm.bbox_min > gpm.bbox_max):
        bad("bbox_min > bbox_max")

    # bones
    seen_names = set()
    for index, bone in enumerate(gpm.bones):
        if bone.parent >= index:
            bad(f"bone {index} ({bone.name}): parent {bone.parent} is not "
                "before it (parent-before-child order broken)")
        if bone.parent < -1 or bone.parent >= gpm.bone_count:
            bad(f"bone {index} ({bone.name}): parent index out of range")
        norm = float(np.linalg.norm(bone.rotation))
        if abs(norm - 1.0) > 1e-3:
            bad(f"bone {index} ({bone.name}): rotation not unit length "
                f"({norm:.6f})")
        if not np.all(np.isfinite(bone.translation)) or \
                not np.all(np.isfinite(bone.scale)):
            bad(f"bone {index} ({bone.name}): non-finite TRS")
        if bone.name in seen_names:
            warnings.append(f"duplicate bone name {bone.name!r}")
        seen_names.add(bone.name)
    if gpm.bone_count and not any(bone.parent == -1 for bone in gpm.bones):
        bad("no root bone (no parent == -1)")

    # vertex bone indices
    if gpm.bone_count and gpm.vertex_count:
        if gpm.bones_idx.max() >= gpm.bone_count:
            bad("vertex bone index out of range")
        used = set(int(v) for v in np.unique(gpm.bones_idx))
        if len(used) < min(gpm.bone_count, 2):
            warnings.append("very few bones are used by vertices")

    # animations
    clip_names = set()
    seen_roles = set()
    for clip in gpm.clips:
        if clip.name not in ROLE_NAMES:
            warnings.append(f"animation name {clip.name!r} is not a standard "
                            f"role (allowed: {sorted(ROLE_NAMES)})")
        if clip.name in seen_roles:
            bad(f"duplicate animation role {clip.name!r}")
        seen_roles.add(clip.name)
        if clip.duration < 0:
            bad(f"clip {clip.name!r}: negative duration")
        track_keys = [track for track in clip.tracks]
        if not track_keys:
            warnings.append(f"clip {clip.name!r} has no tracks")
        seen_pairs = set()
        for track in clip.tracks:
            if track.bone >= gpm.bone_count:
                bad(f"clip {clip.name!r}: track bone index {track.bone} out of "
                    "range")
            if track.component not in (0, 1, 2):
                bad(f"clip {clip.name!r}: track component "
                    f"{track.component} invalid")
            pair = (track.bone, track.component)
            if pair in seen_pairs:
                bad(f"clip {clip.name!r}: duplicate track for bone/"
                    f"component {pair}")
            seen_pairs.add(pair)
            if len(track.times) == 0:
                bad(f"clip {clip.name!r}: empty track")
                continue
            if np.any(np.diff(track.times) < -1e-6):
                bad(f"clip {clip.name!r}: track times not sorted")
            if track.times[0] < -1e-6:
                bad(f"clip {clip.name!r}: negative key time")
            if track.times[-1] > clip.duration + 1e-3:
                bad(f"clip {clip.name!r}: key time {track.times[-1]:.4f} "
                    f"beyond duration {clip.duration:.4f}")
            if track.component == 1:
                norms = np.linalg.norm(track.values, axis=1)
                if np.abs(norms - 1.0).max() > 2e-3:
                    bad(f"clip {clip.name!r}: non-unit quaternion "
                        f"(max |len-1| {np.abs(norms - 1.0).max():.5f})")
            else:
                pads = track.values[:, 3]
                if np.abs(pads).max() > 1e-6:
                    bad(f"clip {clip.name!r}: T/S key pad (w) is not zero")
            if not np.all(np.isfinite(track.values)):
                bad(f"clip {clip.name!r}: non-finite key values")

    # section layout sanity (tiny reader guard vs corrupt files)
    offsets = gpm.section_offsets
    if list(offsets) != sorted(offsets):
        warnings.append("section offsets are not monotonically increasing")
    if any(offset % 4 != 0 for offset in offsets):
        warnings.append("a section is not 4-byte aligned")

    if verbose:
        for warning in warnings:
            print(f"warning: {warning}")
    return problems


def gpm_stats(gpm):
    lines = []
    lines.append(f"file            : {os.path.basename(gpm.path)} "
                 f"({gpm.file_size} bytes)")
    flags = []
    if gpm.flags & FLAG_SKINNED:
        flags.append("skinned")
    if gpm.flags & FLAG_HAS_ANIMATIONS:
        flags.append("animations")
    lines.append(f"flags           : {gpm.flags} ({', '.join(flags) or 'static'})")
    lines.append(f"vertices        : {gpm.vertex_count}")
    lines.append(f"triangles       : {gpm.triangle_count}")
    lines.append(f"bones           : {gpm.bone_count}")
    lines.append(f"animations      : {gpm.anim_count}")
    size = gpm.bbox_max - gpm.bbox_min
    lines.append(f"bbox min        : ({gpm.bbox_min[0]:.3f}, "
                 f"{gpm.bbox_min[1]:.3f}, {gpm.bbox_min[2]:.3f}) m")
    lines.append(f"bbox max        : ({gpm.bbox_max[0]:.3f}, "
                 f"{gpm.bbox_max[1]:.3f}, {gpm.bbox_max[2]:.3f}) m")
    lines.append(f"size            : {size[0]:.3f} x {size[1]:.3f} x "
                 f"{size[2]:.3f} m")
    if gpm.clips:
        parts = [f"{clip.name} {clip.duration:.2f}s" for clip in gpm.clips]
        lines.append("clips           : " + ", ".join(parts))
    if gpm.credit:
        lines.append(f"credit          : {gpm.credit}")
    skin_cost = gpm.vertex_count
    lines.append(f"skinning budget : {skin_cost} rigid vertex transforms/frame "
                 f"({skin_cost * 16 * 4} bytes of matrix work)")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# preview rendering (Pillow)
# ---------------------------------------------------------------------------

def compute_pose(gpm, clip_name=None, time_seconds=0.0):
    """Return per-bone 4x4 matrices (anim_world @ inverse(rest_world))."""
    rest_world = [None] * gpm.bone_count

    def rest_matrix(index):
        if rest_world[index] is None:
            bone = gpm.bones[index]
            local = trs_matrix(bone.translation, bone.rotation, bone.scale)
            if bone.parent < 0:
                rest_world[index] = local
            else:
                rest_world[index] = rest_matrix(bone.parent) @ local
        return rest_world[index]

    for index in range(gpm.bone_count):
        rest_matrix(index)

    # Sample every track first and merge the components per bone: a bone may have
    # both a translation and a rotation track (e.g. the hero's Body/Foot bones),
    # and overwriting instead of merging would lose one of them.
    sampled = [dict() for _ in range(gpm.bone_count)]
    if clip_name is not None and gpm.clips:
        clip = None
        for candidate in gpm.clips:
            if candidate.name == clip_name:
                clip = candidate
                break
        if clip is None:
            raise ValueError(f"clip {clip_name!r} not found in {gpm.path}")
        time_seconds = max(0.0, min(time_seconds, clip.duration))
        for track in clip.tracks:
            sampled[track.bone][track.component] = _sample_pose_track(
                track, time_seconds)

    animated_local = [None] * gpm.bone_count
    for index in range(gpm.bone_count):
        components = sampled[index]
        if not components:
            continue
        bone = gpm.bones[index]
        translation = (components[COMPONENT_TRANSLATION][:3]
                       if COMPONENT_TRANSLATION in components
                       else bone.translation)
        rotation = (components[COMPONENT_ROTATION]
                    if COMPONENT_ROTATION in components else bone.rotation)
        scale = (components[COMPONENT_SCALE][:3]
                 if COMPONENT_SCALE in components else bone.scale)
        animated_local[index] = (translation, rotation, scale)

    anim_world = [None] * gpm.bone_count

    def anim_matrix(index):
        if anim_world[index] is None:
            bone = gpm.bones[index]
            if animated_local[index] is not None:
                t, q, s = animated_local[index]
                local = trs_matrix(t, q, s)
            else:
                local = trs_matrix(bone.translation, bone.rotation, bone.scale)
            if bone.parent < 0:
                anim_world[index] = local
            else:
                anim_world[index] = anim_matrix(bone.parent) @ local
        return anim_world[index]

    for index in range(gpm.bone_count):
        anim_matrix(index)

    poses = []
    for index in range(gpm.bone_count):
        poses.append(anim_world[index] @ np.linalg.inv(rest_world[index]))
    return poses


def _sample_pose_track(track, time_seconds):
    times = track.times
    if len(times) == 0:
        return np.array([0.0, 0.0, 0.0, 0.0])
    if time_seconds <= times[0]:
        return track.values[0]
    if time_seconds >= times[-1]:
        return track.values[-1]
    index = int(np.searchsorted(times, time_seconds, side="right") - 1)
    index = max(0, min(index, len(times) - 2))
    t0, t1 = times[index], times[index + 1]
    alpha = 0.0 if t1 - t0 <= 1e-9 else (time_seconds - t0) / (t1 - t0)
    if track.component == COMPONENT_ROTATION:
        return quat_slerp(track.values[index], track.values[index + 1], alpha)
    return track.values[index] + (track.values[index + 1] -
                                  track.values[index]) * alpha


def render_preview(gpm, out_path, clip_name=None, time_seconds=0.0,
                   width=900, height=700, wireframe=True,
                   view_yaw=PREVIEW_YAW, view_pitch=PREVIEW_PITCH,
                   label_suffix=""):
    """Orthographic 3/4-view flat-shaded render of the model (Pillow)."""
    if gpm.bone_count:
        poses = compute_pose(gpm, clip_name, time_seconds)
        positions = gpm.positions.astype(np.float64)
        posed = np.empty_like(positions)
        for vertex in range(gpm.vertex_count):
            bone = int(gpm.bones_idx[vertex])
            m = poses[bone]
            posed[vertex] = m[:3, :3] @ positions[vertex] + m[:3, 3]
    else:
        posed = gpm.positions.astype(np.float64)

    yaw = math.radians(view_yaw)
    pitch = math.radians(view_pitch)
    view = mat4_rot_x(pitch) @ mat4_rot_y(yaw)
    camera = posed @ view[:3, :3].T + view[:3, 3]
    # orthographic fit
    mins = camera.min(axis=0)
    maxs = camera.max(axis=0)
    centre = (mins + maxs) * 0.5
    span = max(maxs[0] - mins[0], maxs[1] - mins[1], 1e-6)
    margin = 0.82
    scale = min(width, height) * margin / span
    cx, cy = width / 2.0, height / 2.0

    image = Image.new("RGB", (width, height), (26, 28, 34))
    draw = ImageDraw.Draw(image)
    # ground line / grid hint
    ground_y = cy + (0.0 - centre[1]) * -scale
    draw.line([(0, ground_y), (width, ground_y)], fill=(48, 52, 60))
    for x in range(-4, 5):
        sx = cx + (x * 0.5 - centre[0]) * scale
        draw.line([(sx, ground_y - 6), (sx, ground_y + 2)],
                  fill=(40, 44, 52))

    light = np.array([-0.35, 0.7, 0.6])
    light = light / np.linalg.norm(light)
    indices = gpm.indices.reshape(-1, 3)
    colours = gpm.colours[:, :3].astype(np.float64) / 255.0
    faces = []
    for face in indices:
        a, b, c = posed[face[0]], posed[face[1]], posed[face[2]]
        normal = np.cross(b - a, c - a)
        norm = np.linalg.norm(normal)
        if norm < 1e-12:
            continue
        normal = normal / norm
        view_normal = view[:3, :3] @ normal
        # two-sided flat shading; the painter's sort deals with occlusion
        shade = 0.55 + 0.45 * max(0.0, float(np.dot(view_normal, light)))
        xy = []
        depth = 0.0
        for vertex in face:
            p = camera[vertex]
            xy.append((cx + (p[0] - centre[0]) * scale,
                       cy - (p[1] - centre[1]) * scale))
            depth += p[2]
        depth /= 3.0
        rgb = np.clip(colours[face].mean(axis=0) * shade, 0.0, 1.0)
        faces.append((depth, xy, tuple(int(value * 255) for value in rgb)))
    faces.sort(key=lambda item: item[0])   # far (small +Z) first
    for depth, xy, rgb in faces:
        draw.polygon(xy, fill=rgb)
        if wireframe:
            draw.line(xy + [xy[0]], fill=(0, 0, 0), width=1)
    label = os.path.basename(gpm.path) + label_suffix
    if clip_name:
        label += f"  clip={clip_name} t={time_seconds:.2f}s"
    draw.text((14, 12), label, fill=(210, 210, 200))
    draw.text((14, 28), f"{gpm.vertex_count} verts / {gpm.triangle_count} tris / "
                        f"{gpm.bone_count} bones", fill=(150, 150, 140))
    bounds = " ".join(f"{gpm.bbox_min[i]:.2f}" for i in range(3))
    bounds += "  ..  " + " ".join(f"{gpm.bbox_max[i]:.2f}" for i in range(3))
    draw.text((14, 44), f"bbox {bounds} m  view yaw={view_yaw:+.0f}",
              fill=(150, 150, 140))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    image.save(out_path)
    return out_path


# ---------------------------------------------------------------------------
# importer main
# ---------------------------------------------------------------------------

def import_model(glb_path, config, args):
    importer = Importer(glb_path, config, args)
    if importer.errors:
        raise SystemExit("error: " + importer.errors[0])
    mesh_nodes = importer.selected_mesh_nodes()
    if not mesh_nodes:
        importer.fail("no mesh nodes selected (check the config 'parts' list)")

    skin_ids = importer.collect_skins(mesh_nodes)
    # Bones must be known before vertices, but root rest needs the model
    # matrix, which needs the native vertex bbox. Do it in two passes:
    # pass 1: cheap skinned positions for the bbox (no colours/textures).
    # The metric scale is measured over the WHOLE rig (including parts we drop,
    # e.g. the hero's imported head), so the final model lands at the canonical
    # target height instead of shrinking because a part was filtered out.
    all_mesh_nodes = [index for index, node in enumerate(importer.src.gltf.nodes)
                      if node.mesh is not None]
    skinned_nodes = [index for index in all_mesh_nodes
                     if importer.src.node(index).skin is not None]
    measure_nodes = skinned_nodes if skinned_nodes else all_mesh_nodes
    pre = _preview_parts_bbox(importer, measure_nodes)
    model_matrix, transform_stats = _compute_matrix_from_bbox(
        importer, pre)
    bones, name_to_bone, root_bases = importer.build_bones(
        skin_ids, model_matrix) if skin_ids else ([], {}, {})
    parts = importer.collect_vertices(mesh_nodes, name_to_bone, bones)
    # apply the model matrix to every cached part
    positions = []
    normals = []
    colours = []
    vertex_bones = []
    indices = []
    base_vertex = 0
    for part in parts:
        positions.append(transform_points(model_matrix, part["positions"]))
        if part["normals"] is not None:
            normals.append(transform_normals(model_matrix, part["normals"]))
        else:
            normals.append(np.zeros_like(positions[-1]))
        colours.append(part["colours"])
        vertex_bones.append(part["bones"])
        indices.append(part["indices"].astype(np.int64) + base_vertex)
        base_vertex += part["positions"].shape[0]
    all_positions = np.concatenate(positions, axis=0).astype(np.float32)
    all_normals = np.concatenate(normals, axis=0).astype(np.float32)
    lengths = np.linalg.norm(all_normals.astype(np.float64), axis=1)
    lengths[lengths < 1e-12] = 1.0
    all_normals = (all_normals / lengths[:, None]).astype(np.float32)
    all_colours = np.concatenate(colours, axis=0).astype(np.uint8)
    all_vertex_bones = (np.concatenate(vertex_bones, axis=0).astype(np.uint16)
                        if bones else np.zeros(len(all_positions), dtype=np.uint16))
    all_indices = np.concatenate(indices, axis=0).astype(np.int64)
    if all_positions.shape[0] >= 65536:
        importer.fail(f"model has {all_positions.shape[0]} vertices; GPM v1 "
                      "indices are u16 (max 65535). Split the model or drop "
                      "parts.")
    if all_indices.max(initial=0) >= all_positions.shape[0]:
        importer.fail("internal error: index out of range after merge")

    bbox_min = all_positions.min(axis=0).astype(np.float64)
    bbox_max = all_positions.max(axis=0).astype(np.float64)
    # snap the header bbox to the grounded/centred model (it should already be)
    model = GpmModel(
        positions=all_positions, normals=all_normals, colours=all_colours,
        vertex_bones=all_vertex_bones, indices=all_indices.astype(np.uint16),
        bones=bones, clips=[], credit=(args.credit or "").strip(),
        bbox_min=bbox_min, bbox_max=bbox_max)

    resample_hz = int(config.get("resample_hz", 30) or 0)
    clips = importer.build_animations(bones, name_to_bone, root_bases,
                                      resample_hz,
                                      no_thin=bool(getattr(args, "no_thin",
                                                            False)))
    model.clips = clips

    # attachment validation: hint names resolved against the exported bones
    attachments = config.get("attachments") or {}
    resolved = {}
    bone_names_lower = {name.lower(): name for name in
                        [bone.name for bone in bones]}
    for role, bone_name in attachments.items():
        target = None
        if bone_name in bone_names_lower.values():
            target = bone_name
        elif bone_name.lower() in bone_names_lower:
            target = bone_names_lower[bone_name.lower()]
        else:
            stripped = bone_name.lower()
            for prefix in ("mixamorig:", "armature|", "characterarmature|"):
                if stripped.startswith(prefix):
                    stripped = stripped[len(prefix):]
            if stripped in bone_names_lower:
                target = bone_names_lower[stripped]
        if target is None:
            importer.warn(f"attachment {role!r} -> {bone_name!r} does not "
                          "match any exported bone name; the runtime will fall "
                          "back")
        else:
            resolved[role] = target
    model.attachments = resolved

    return model, transform_stats, importer


def _preview_parts_bbox(importer, node_indices):
    """Cheap pass: skinned/static rest positions without colours/textures."""
    src = importer.src
    gltf = src.gltf
    points = []
    for node_index in node_indices:
        node = gltf.nodes[node_index]
        mesh = gltf.meshes[node.mesh]
        world = src.world_matrix(node_index)
        skin_id = node.skin
        skin_joints = gltf.skins[skin_id].joints if skin_id is not None else []
        ibms = src.inverse_bind_matrices(skin_id) if skin_id is not None else []
        for prim in mesh.primitives:
            attrs = prim.attributes
            position_index = getattr(attrs, "POSITION", None)
            if position_index is None:
                continue
            positions = src.accessor(position_index)
            if skin_id is not None:
                joints_acc = getattr(attrs, "JOINTS_0", None)
                weights_acc = getattr(attrs, "WEIGHTS_0", None)
                if joints_acc is None or weights_acc is None:
                    continue
                joints = src.accessor(joints_acc).astype(np.int64)
                weights = src.accessor(weights_acc)
                dominant = weights.argmax(axis=1)
                local_joint = joints[np.arange(positions.shape[0]), dominant]
                cache = {}
                for vertex in range(positions.shape[0]):
                    local = int(local_joint[vertex])
                    joint_node = skin_joints[local]
                    matrix = cache.get(local)
                    if matrix is None:
                        matrix = src.world_matrix(joint_node) @ ibms[local]
                        cache[local] = matrix
                    points.append((matrix @ np.append(positions[vertex], 1.0))[:3])
            else:
                points.append(transform_points(world, positions))
    if not points:
        importer.fail("no vertices selected - check the config 'parts' list")
    flat = np.concatenate([np.asarray(p).reshape(-1, 3) for p in points], axis=0)
    return flat


def _compute_matrix_from_bbox(importer, native_points):
    """Normalise/yaw/ground using native rest points (no material pass)."""
    # Reuse Importer.compute_model_matrix but feed it fake 'parts'.
    fake = [{"positions": native_points}]
    return importer.compute_model_matrix(fake)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert a glTF binary model to the GPM v1 format.")
    parser.add_argument("source", nargs="?", help="source .glb file (optional "
                        "if the config JSON has a 'source' field)")
    parser.add_argument("--config", required=True,
                        help="import config JSON (assets/import/*.json)")
    parser.add_argument("--out", required=True, help="output .gpm path")
    parser.add_argument("--kind", choices=sorted(KIND_TARGETS),
                        help="override/confirm the config 'kind'")
    parser.add_argument("--credit", default=None,
                        help="credit/attribution string written into the GPM "
                             "header")
    parser.add_argument("--preview", default=None,
                        help="write a PNG preview of the converted model")
    parser.add_argument("--preview-clip", default=None,
                        help="pose the preview with this animation role")
    parser.add_argument("--preview-time", type=float, default=0.0,
                        help="preview clip time in seconds")
    parser.add_argument("--preview-rest", action="store_true",
                        help="preview the rest pose even if the model has clips")
    parser.add_argument("--preview-yaw", type=float, default=PREVIEW_YAW,
                        help="preview camera yaw in degrees (0 looks at the "
                             "model's +Z side)")
    parser.add_argument("--preview-pitch", type=float, default=PREVIEW_PITCH,
                        help="preview camera pitch in degrees")
    parser.add_argument("--stats", action="store_true",
                        help="print a detailed stats table")
    parser.add_argument("--bones", action="store_true",
                        help="print the exported bone table (name/parent/rest)")
    parser.add_argument("--no-thin", action="store_true",
                        help="do not drop redundant animation keyframes")
    parser.add_argument("--thin-rotation-deg", type=float, default=None,
                        help="keyframe thinning tolerance for rotations in "
                             "degrees (default 0.75)")
    parser.add_argument("--thin-translation-m", type=float, default=None,
                        help="keyframe thinning tolerance for translations in "
                             "metres (default 0.0005)")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    globals()["THIN_ROTATION_EPS"] = math.radians(
        args.thin_rotation_deg if args.thin_rotation_deg is not None else 0.75)
    if args.thin_translation_m is not None:
        globals()["THIN_TRANSLATION_EPS"] = args.thin_translation_m
        globals()["THIN_SCALE_EPS"] = args.thin_translation_m

    with open(args.config, "r", encoding="utf-8") as fh:
        config = json.load(fh)
    source = args.source or config.get("source")
    if not source:
        parser.error("no source .glb: pass it positionally or set 'source' in "
                     "the config")
    if not os.path.isfile(source):
        parser.error(f"source model not found: {source}")
    kind_from_config = config.get("kind")
    if args.kind and kind_from_config and args.kind != kind_from_config:
        sys.stderr.write(f"warning: --kind {args.kind!r} overrides config kind "
                         f"{kind_from_config!r}\n")
    credit = args.credit if args.credit is not None else config.get("credit", "")

    model, transform_stats, importer = import_model(source, config,
                                                    args_with_credit(args,
                                                                     credit))
    written = write_gpm(model, args.out)

    # self-check: re-read and validate what was just written
    gpm = read_gpm(args.out)
    problems = validate_gpm(gpm)
    if problems:
        for problem in problems:
            sys.stderr.write(f"validation error: {problem}\n")
        try:
            os.remove(args.out)
        except OSError:
            pass
        sys.exit(f"error: wrote invalid {args.out}; file removed")

    for warning in importer.warnings:
        sys.stderr.write(f"warning: {warning}\n")

    if not args.quiet:
        stats = transform_stats
        native = stats["native_size"]
        print(f"{os.path.basename(args.out)}: {gpm.vertex_count} verts, "
              f"{gpm.triangle_count} tris, {gpm.bone_count} bones, "
              f"{gpm.anim_count} clips, {written} bytes")
        print(f"  native bbox {native[0]:.3f} x {native[1]:.3f} x "
              f"{native[2]:.3f}  -> scale {stats['scale']:.4f}x "
              f"(config {stats['config_scale']:.3f}x, "
              f"{stats['target'][0]} target {stats['target'][1]:.2f} m), "
              f"yaw {stats['yaw']:.1f} deg")
        print(f"  bbox {gpm.bbox_min[0]:.3f},{gpm.bbox_min[1]:.3f},"
              f"{gpm.bbox_min[2]:.3f} .. {gpm.bbox_max[0]:.3f},"
              f"{gpm.bbox_max[1]:.3f},{gpm.bbox_max[2]:.3f} m")
        if gpm.clips:
            clip_parts = []
            total_keys = 0
            for clip in gpm.clips:
                keys = sum(len(track.times) for track in clip.tracks)
                total_keys += keys
                clip_parts.append(f"{clip.name}({clip.duration:.2f}s, "
                                  f"{len(clip.tracks)}tr/{keys}keys)")
            print("  clips: " + ", ".join(clip_parts) +
                  f"  [total {total_keys} keys]")
        if gpm.bones:
            attachments = getattr(model, "attachments", {})
            if attachments:
                print("  attachments: " + ", ".join(
                    f"{role}->{bone}" for role, bone in attachments.items()))
        if args.stats:
            print(gpm_stats(gpm))
        if args.bones:
            print("  bone table (parent, rest T/R/S):")
            for index, bone in enumerate(gpm.bones):
                rotation = "[" + ", ".join(f"{v:+.4f}" for v in bone.rotation) + "]"
                translation = "[" + ", ".join(f"{v:+.4f}"
                                              for v in bone.translation) + "]"
                scale = "[" + ", ".join(f"{v:.4f}" for v in bone.scale) + "]"
                print(f"    [{index:2d}] parent {bone.parent:3d}  "
                      f"{bone.name:28s} T{translation} R{rotation} S{scale}")
        if stats["yaw"] != 0.0:
            print(f"  (model facing convention: +Z after the config yaw)")

    if args.preview:
        clip_name = args.preview_clip
        if not args.preview_rest and clip_name is None and gpm.clips:
            preferred = ["idle", "walk", "run", "attack", "shoot"]
            for role in preferred:
                if any(clip.name == role for clip in gpm.clips):
                    clip_name = role
                    break
            if clip_name is None:
                clip_name = gpm.clips[0].name
        render_preview(gpm, args.preview, clip_name=clip_name,
                       time_seconds=args.preview_time,
                       view_yaw=args.preview_yaw,
                       view_pitch=args.preview_pitch)
        if not args.quiet:
            print(f"  preview: {args.preview}"
                  + (f" (clip {clip_name} @ {args.preview_time:.2f}s)"
                     if clip_name else " (rest pose)"))
    return 0


def args_with_credit(args, credit):
    """Shallow copy of the argparse namespace with the final credit string."""
    clone = argparse.Namespace(**vars(args))
    clone.credit = credit
    return clone


if __name__ == "__main__":
    sys.exit(main())
