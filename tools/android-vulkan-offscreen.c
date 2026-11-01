/* Runs the real Android guest/backend without a headset session. Diagnostic
 * only: this does not measure compositor, tracking, overlays or VR frame time. */
#include <stdlib.h>
/* Bypass SDL_main's Java storage lookup and shared env/log files. */
extern int chihiro_main(int,char **);
int main(int argc,char **argv)
{
    setenv("CHIHIRO_VULKAN_OFFSCREEN","1",1);
    setenv("RECOMP_HEADLESS","1",1);
    setenv("RECOMP_FB_WINDOW","0",1);
    return chihiro_main(argc,argv);
}
