/* The VR rifle's model, in the binary (no file access on the headset):
 * third_party/rifle/m700.mesh and m700_color.jpg (scripts/rifle-model.py;
 * the Stein Games M700, CC0: third_party/README.md). The build passes their
 * paths as RIFLE_M700_MESH and RIFLE_M700_JPG (quoted strings): CMake, and
 * scripts/vr-rifle-test.sh for the tests. Read (quest_rifle_model.inc) as
 *     extern const unsigned char rifle_m700_mesh[], rifle_m700_mesh_end[],
 *                                rifle_m700_jpg[], rifle_m700_jpg_end[];
 * One statement a line: ';' separates statements on some targets and starts
 * a comment on others (Apple's arm64). */
#if !defined(RIFLE_M700_MESH) || !defined(RIFLE_M700_JPG)
#error "RIFLE_M700_MESH and RIFLE_M700_JPG: the assets' paths, as quoted strings"
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

    .globl SYM(rifle_m700_mesh)
    HIDE SYM(rifle_m700_mesh)
    .globl SYM(rifle_m700_mesh_end)
    HIDE SYM(rifle_m700_mesh_end)
    .p2align 4
SYM(rifle_m700_mesh):
    .incbin RIFLE_M700_MESH
SYM(rifle_m700_mesh_end):

    .globl SYM(rifle_m700_jpg)
    HIDE SYM(rifle_m700_jpg)
    .globl SYM(rifle_m700_jpg_end)
    HIDE SYM(rifle_m700_jpg_end)
    .p2align 4
SYM(rifle_m700_jpg):
    .incbin RIFLE_M700_JPG
SYM(rifle_m700_jpg_end):

#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
