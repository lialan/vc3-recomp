#ifndef CH_VK_RUNTIME_H
#define CH_VK_RUNTIME_H
/* Start after the guest address space exists, before any guest GPU work.
 * This direct Vulkan path never creates an OpenGL context. */
int ch_vk_runtime_start(void);
/* Stop/drain executor workers before destroying the runtime. */
void ch_vk_runtime_stop(void);
#endif
