/* Pipeline recipes packaged with the build (vk_target.c reads them at start
 * and precompiles every one before the game starts). The build passes the
 * file as CH_VK_PACKAGED_RECIPES (a quoted string): quest/build.sh gathers
 * the .recipes files in games/<id>/pipeline-cache/quest3, harvested on the headset
 * (CHIHIRO_VULKAN_PIPELINE_EXPORT). Without it the symbols mark an empty
 * range. Read as
 *     extern const unsigned char ch_vk_packaged_recipes[], ch_vk_packaged_recipes_end[];
 * One statement a line: ';' separates statements on some targets and starts
 * a comment on others (Apple's arm64). */
#if defined(__APPLE__)
#define SYM(x) _##x
#define HIDE .private_extern
    .const_data
#else
#define SYM(x) x
#define HIDE .hidden
    .section .rodata
#endif

    .globl SYM(ch_vk_packaged_recipes)
    HIDE SYM(ch_vk_packaged_recipes)
    .globl SYM(ch_vk_packaged_recipes_end)
    HIDE SYM(ch_vk_packaged_recipes_end)
    .p2align 4
SYM(ch_vk_packaged_recipes):
#if defined(CH_VK_PACKAGED_RECIPES)
    .incbin CH_VK_PACKAGED_RECIPES
#endif
SYM(ch_vk_packaged_recipes_end):

/* The CPU-specialized shaders likewise (vk_spirv_specialize.cpp), as
 * CH_VK_PACKAGED_SPECIALIZED: the .specialized file harvested beside them. */
    .globl SYM(ch_vk_packaged_specialized)
    HIDE SYM(ch_vk_packaged_specialized)
    .globl SYM(ch_vk_packaged_specialized_end)
    HIDE SYM(ch_vk_packaged_specialized_end)
    .p2align 4
SYM(ch_vk_packaged_specialized):
#if defined(CH_VK_PACKAGED_SPECIALIZED)
    .incbin CH_VK_PACKAGED_SPECIALIZED
#endif
SYM(ch_vk_packaged_specialized_end):
#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
