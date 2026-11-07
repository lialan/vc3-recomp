/* The VR shotgun's model, in the binary (no file access on the headset):
 * third_party/shotgun/r870.mesh, r870_pump.mesh and r870_color.jpg
 * (scripts/rifle-model.py --shotgun; Gintoki1234's Remington 870, CC BY 4.0:
 * third_party/README.md), as vk_rifle_assets.S has the rifle's. The build
 * passes their paths as SHOTGUN_R870_MESH, SHOTGUN_R870_PUMP_MESH and
 * SHOTGUN_R870_JPG (quoted strings). Read (vk_overlay.c) as
 *     extern const unsigned char shotgun_r870_mesh[], shotgun_r870_mesh_end[],
 *                                shotgun_r870_pump_mesh[], shotgun_r870_pump_mesh_end[],
 *                                shotgun_r870_jpg[], shotgun_r870_jpg_end[];
 * One statement a line: ';' separates statements on some targets and starts
 * a comment on others (Apple's arm64). */
#if !defined(SHOTGUN_R870_MESH) || !defined(SHOTGUN_R870_PUMP_MESH) || !defined(SHOTGUN_R870_JPG)
#error "SHOTGUN_R870_MESH, SHOTGUN_R870_PUMP_MESH and SHOTGUN_R870_JPG: the assets' paths, as quoted strings"
#endif

#if defined(__APPLE__)
#define SYM(x) _##x
#define HIDE .private_extern
    .const_data
#else
#define SYM(x) x
#define HIDE .hidden
    .section .rodata
#endif

    .globl SYM(shotgun_r870_mesh)
    HIDE SYM(shotgun_r870_mesh)
    .globl SYM(shotgun_r870_mesh_end)
    HIDE SYM(shotgun_r870_mesh_end)
    .p2align 4
SYM(shotgun_r870_mesh):
    .incbin SHOTGUN_R870_MESH
SYM(shotgun_r870_mesh_end):

    .globl SYM(shotgun_r870_pump_mesh)
    HIDE SYM(shotgun_r870_pump_mesh)
    .globl SYM(shotgun_r870_pump_mesh_end)
    HIDE SYM(shotgun_r870_pump_mesh_end)
    .p2align 4
SYM(shotgun_r870_pump_mesh):
    .incbin SHOTGUN_R870_PUMP_MESH
SYM(shotgun_r870_pump_mesh_end):

    .globl SYM(shotgun_r870_jpg)
    HIDE SYM(shotgun_r870_jpg)
    .globl SYM(shotgun_r870_jpg_end)
    HIDE SYM(shotgun_r870_jpg_end)
    .p2align 4
SYM(shotgun_r870_jpg):
    .incbin SHOTGUN_R870_JPG
SYM(shotgun_r870_jpg_end):

#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
