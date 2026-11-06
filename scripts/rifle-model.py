#!/usr/bin/env python3
"""The VR rifle's model for the runtime: third_party/rifle/m700.mesh and
m700_color.jpg from the Stein Games M700 (CC0; third_party/README.md).

Usage (by hand, once; the output is committed):
    python3 -I scripts/rifle-model.py <m700.glb> [out dir: third_party/rifle]
in a venv with numpy and Pillow. The input is the M700 already in glTF in the
rifle's gun space (metres; +y up; the muzzle along -z; +x right; the origin
the pistol grip's centre), one mesh, one primitive, its base colour JPEG and
its occlusion/roughness/metal JPEG (R = ambient occlusion). It is read as
data: nothing in it is run.

m700.mesh, little-endian:
    "RFL1", uint32 vertices, uint32 indices,
    float min[3], float step[3]           position = min + q * step
    vertices x { uint16 q[3]; uint16 uv[2] (/ 65535); int8 n[3] (/ 127), pad }
    indices x uint16                      triangles
m700_color.jpg: the base colour times the ambient occlusion (in linear
light), 1024 x 1024. The normal and roughness/metal maps are not used: the
overlay lights per vertex.

Also prints what the runtime's constants (quest_rifle_geometry.inc) are
measured from: the muzzle, the rail, the heights round it.
"""
import io
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

TEXTURE = 1024


def load_glb(path):
    data = open(path, 'rb').read()
    magic, version, _ = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67 or version != 2:
        sys.exit('not a glTF 2 binary: %s' % path)
    jlen, jtype = struct.unpack_from('<II', data, 12)
    if jtype != 0x4E4F534A:
        sys.exit('no JSON chunk')
    gltf = json.loads(data[20:20 + jlen])
    blen, btype = struct.unpack_from('<II', data, 20 + jlen)
    if btype != 0x004E4942:
        sys.exit('no BIN chunk')
    blob = data[28 + jlen:28 + jlen + blen]
    views = gltf['bufferViews']

    def accessor(i, ncomp):
        a = gltf['accessors'][i]
        dtype = {5126: np.float32, 5123: np.uint16, 5125: np.uint32}[a['componentType']]
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        if n != ncomp:
            sys.exit('accessor %d: %s, want %d components' % (i, a['type'], ncomp))
        v = views[a['bufferView']]
        if v.get('byteStride', 0) not in (0, n * np.dtype(dtype).itemsize):
            sys.exit('accessor %d: interleaved' % i)
        arr = np.frombuffer(blob, dtype, a['count'] * n, v.get('byteOffset', 0) + a.get('byteOffset', 0))
        return arr.reshape(-1, n) if n > 1 else arr

    meshes = gltf['meshes']
    if len(meshes) != 1 or len(meshes[0]['primitives']) != 1:
        sys.exit('want one mesh with one primitive')
    prim = meshes[0]['primitives'][0]
    if prim.get('mode', 4) != 4:
        sys.exit('want triangles')
    at = prim['attributes']
    pos = accessor(at['POSITION'], 3).astype(np.float64)
    nrm = accessor(at['NORMAL'], 3).astype(np.float64)
    uv = accessor(at['TEXCOORD_0'], 2).astype(np.float64)
    idx = accessor(prim['indices'], 1).astype(np.int64).reshape(-1, 3)

    material = gltf['materials'][prim['material']]
    pbr = material['pbrMetallicRoughness']

    def image(texture_index):
        img = gltf['images'][gltf['textures'][texture_index]['source']]
        v = views[img['bufferView']]
        raw = blob[v.get('byteOffset', 0):v.get('byteOffset', 0) + v['byteLength']]
        return Image.open(io.BytesIO(raw))

    base = image(pbr['baseColorTexture']['index']).convert('RGB')
    occlusion = image(material['occlusionTexture']['index']).convert('RGB').split()[0]
    return pos, nrm, uv, idx, base, occlusion, gltf


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def colour_texture(base, occlusion):
    """base x AO in linear light, box-filtered down to TEXTURE, back to sRGB."""
    if base.size != occlusion.size or base.size[0] != base.size[1] or base.size[0] % TEXTURE:
        sys.exit('textures %s / %s: want square, a multiple of %d' % (base.size, occlusion.size, TEXTURE))
    lin = srgb_to_linear(np.asarray(base, dtype=np.float64) / 255.0)
    lin *= (np.asarray(occlusion, dtype=np.float64) / 255.0)[..., None]
    k = base.size[0] // TEXTURE
    lin = lin.reshape(TEXTURE, k, TEXTURE, k, 3).mean(axis=(1, 3))
    out = np.round(linear_to_srgb(lin) * 255.0).astype(np.uint8)
    return Image.fromarray(out, 'RGB')


def pack_mesh(pos, nrm, uv, idx):
    nv, ni = len(pos), idx.size
    if nv > 65535 or idx.max() >= nv or idx.min() < 0:
        sys.exit('indices do not fit uint16 (%d vertices)' % nv)
    if uv.min() < 0 or uv.max() > 1:
        sys.exit('uv outside 0..1')
    lo, hi = pos.min(0), pos.max(0)
    step = (hi - lo) / 65535.0
    q = np.round((pos - lo) / step).astype(np.uint16)
    n = nrm / np.linalg.norm(nrm, axis=1, keepdims=True)
    n8 = np.clip(np.round(n * 127.0), -127, 127).astype(np.int8)
    u16 = np.round(uv * 65535.0).astype(np.uint16)
    rec = np.zeros(nv, dtype=[('q', '<u2', 3), ('uv', '<u2', 2), ('n', 'i1', 3), ('pad', 'i1')])
    rec['q'], rec['uv'], rec['n'] = q, u16, n8
    head = b'RFL1' + struct.pack('<II6f', nv, ni, *lo.astype(np.float32), *step.astype(np.float32))
    body = head + rec.tobytes() + idx.reshape(-1).astype('<u2').tobytes()
    # what the runtime unpacks: the measurements below are of this
    back = lo.astype(np.float32).astype(np.float64) + q.astype(np.float64) * step.astype(np.float32).astype(np.float64)
    return body, back


def clip(poly, axis, value, keep_above):
    out = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        ina = a[axis] >= value if keep_above else a[axis] <= value
        inb = b[axis] >= value if keep_above else b[axis] <= value
        if ina:
            out.append(a)
        if ina != inb:
            out.append(a + (value - a[axis]) / (b[axis] - a[axis]) * (b - a))
    return out


def top(tris, z0, z1, half_x=None):
    """The highest point of the triangles between z0 and z1 (and |x| <= half_x)."""
    best = -1.0
    lo, hi = tris[:, :, 2].min(1), tris[:, :, 2].max(1)
    for t in np.where((hi >= z0) & (lo <= z1))[0]:
        poly = clip(clip(list(tris[t]), 2, z0, True), 2, z1, False)
        if poly and half_x is not None:
            poly = clip(clip(poly, 0, -half_x, True), 0, half_x, False)
        if poly:
            best = max(best, max(p[1] for p in poly))
    return best


def ray(tris, origin, direction):
    """The nearest hit along a ray (Moller-Trumbore), or inf."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = b - a, c - a
    p = np.cross(direction, e2)
    det = (e1 * p).sum(1)
    ok = np.abs(det) > 1e-12
    inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
    s = origin - a
    u = (s * p).sum(1) * inv
    q = np.cross(s, e1)
    v = (q * direction).sum(1) * inv
    t = (e2 * q).sum(1) * inv
    hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0)
    return t[hit].min() if hit.any() else np.inf


def measure(pos, idx):
    tris = pos[idx]
    zmin = pos[:, 2].min()
    tip = pos[pos[:, 2] < zmin + 0.003]
    tip_c = (tip.min(0) + tip.max(0)) / 2
    print('bounds        x %.4f..%.4f  y %.4f..%.4f  z %.4f..%.4f' % (
        pos[:, 0].min(), pos[:, 0].max(), pos[:, 1].min(), pos[:, 1].max(), zmin, pos[:, 2].max()))
    print('muzzle tip    (%.4f, %.4f, %.4f)  [RIFLE_MUZZLE_Y/Z]' % (tip_c[0], tip_c[1], zmin))
    rail = pos[(np.abs(pos[:, 0]) < 0.011) & (pos[:, 1] > pos[:, 1].max() - 0.0005)]
    print('rail top      y %.4f, z %.4f..%.4f  [M700_RAIL_TOP]' % (
        pos[:, 1].max(), rail[:, 2].min(), rail[:, 2].max()))
    print('ahead of it   highest %.4f (z -0.320..-0.249)  [M700_FORE_TOP]' % top(tris, -0.320, -0.249))
    print('action        highest %.4f (z -0.087..-0.030)' % top(tris, -0.087, -0.030))
    print('wrist, comb   highest %.4f (z -0.030..0.300)  [M700_WRIST_TOP]' % top(tris, -0.030, 0.300))
    print('behind lens   highest %.4f (z 0.071..0.371)' % top(tris, 0.071, 0.371))
    for name, d in (('+x', (1, 0, 0)), ('-x', (-1, 0, 0)), ('+z', (0, 0, 1)), ('-z', (0, 0, -1)),
                    ('+y', (0, 1, 0)), ('-y', (0, -1, 0))):
        print('grip origin   ray %s hits at %.4f m' % (name, ray(tris, np.zeros(3), np.array(d, float))))


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'third_party', 'rifle')
    pos, nrm, uv, idx, base, occlusion, gltf = load_glb(src)
    print('source        %s: %d vertices, %d triangles; %s' % (
        os.path.basename(src), len(pos), len(idx), gltf.get('asset', {}).get('copyright', '')))
    body, unpacked = pack_mesh(pos, nrm, uv, idx)
    with open(os.path.join(out, 'm700.mesh'), 'wb') as f:
        f.write(body)
    jpg = io.BytesIO()
    colour_texture(base, occlusion).save(jpg, 'JPEG', quality=90, subsampling=0, optimize=True)
    with open(os.path.join(out, 'm700_color.jpg'), 'wb') as f:
        f.write(jpg.getvalue())
    print('wrote         m700.mesh %d bytes, m700_color.jpg %d bytes (%d x %d)' % (
        len(body), len(jpg.getvalue()), TEXTURE, TEXTURE))
    measure(unpacked, idx)


if __name__ == '__main__':
    main()
