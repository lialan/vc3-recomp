# Third-party files

What is here, where it came from, and its licence. Only CC0, CC-BY,
public-domain or MIT material goes in this folder.

## rifle/ — the VR rifle's model (Silent Scope VR)

- **What:** "Free Sniper Rifle M700 CC0" by **Stein Games**,
  <https://stein-indie.itch.io/m700>.
- **Licence:** Creative Commons Zero v1.0 Universal (CC0 1.0),
  <https://creativecommons.org/publicdomain/zero/1.0/>. The author dedicates
  it to the public domain and asks for no credit ("You don't need to give
  credit but I would of course appreciate it"). We credit it anyway: thank
  you, Stein Games.
- **Files** (changed from the download; only what the runtime needs):
  - `m700.mesh`: the rifle's mesh (11,332 vertices, 11,582 triangles):
    positions, normals and texture coordinates, quantised, with 16-bit
    indices. In the rifle's gun space: metres, +y up, the muzzle along -z,
    the origin the pistol grip's centre.
  - `m700_color.jpg`: its base colour multiplied by its ambient occlusion
    (in linear light), 1024 x 1024.
- **How they were made:** the download's FBX was converted to a static glTF
  mesh (skin and the hidden magazine cartridges removed; centimetres to
  metres; turned to the gun space above; the textures packed), and that GLB
  converted by `scripts/rifle-model.py`, which documents the formats. The
  download and the GLB (5.9 MB, with the 2048 px maps) are not kept here: the
  normal and roughness/metal maps are not used (the overlay lights per
  vertex).
- **Used by:** `runtime/src/gpu/vulkan/vk_rifle_assets.S` (embedded in the
  binary), `runtime/src/xr/quest_rifle_model.inc`, `vk_overlay.c`.
- The scope on it is not from the download: it is generated
  (`runtime/src/xr/quest_rifle_scope.inc`).

## stb/stb_image.h — JPEG decoder

- **What:** stb_image v2.30 by Sean Barrett, <https://github.com/nothings/stb>
  (commit f58f558c120e9b32c217290b80bad1a0729fbb2c), unmodified.
- **Licence:** public domain (Unlicense) or MIT, whichever you prefer; the
  full text is at the end of the file.
- **Used by:** `runtime/src/gpu/vulkan/vk_jpeg.c`, its JPEG decoder only, to
  decode `rifle/m700_color.jpg` at start-up.
