#ifndef CH_VK_RUNTIME_H
#define CH_VK_RUNTIME_H
#include "vk_backend.h"
/* Configure before start with an OpenXR-selected device and host frame hook.
 * Copies config; Vulkan handles/context remain host-owned until stop returns.
 * Guest mapping and texture decoder are supplied by start. Optional queue lock
 * must be paired. Cannot replace a running backend. NULL resets configuration. */
int ch_vk_runtime_configure(const ChVkBackendConfig *);
/* Start after the guest address space exists, before any guest GPU work.
 * This direct Vulkan path never creates an OpenGL context. */
int ch_vk_runtime_start(void);
/* Stop/drain executor workers before destroying the runtime. */
void ch_vk_runtime_stop(void);
#endif
