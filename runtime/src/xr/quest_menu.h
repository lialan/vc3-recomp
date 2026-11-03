/*
 * quest_menu.h - the headset's settings panel, painted on the CPU (quest_menu.c)
 * and shown by the overlay (quest_overlay.c).
 */
#ifndef VC3_QUEST_MENU_H
#define VC3_QUEST_MENU_H

#define XM_W 768                /* the panel's picture, in pixels */
#define XM_H 808
#define XM_ROWS 19

typedef struct {
    const char *label;
    char value[40];             /* "" for an action */
} XmItem;

/* Paints the panel into rgba (XM_W x XM_H x 4): title, the items with their
 * values, hover highlighted (-1: none), and a hint at the bottom. */
void xm_paint(unsigned char *rgba, const char *title, const XmItem *items, int n, int hover, const char *hint);
/* The stats panel ("Show stats"): lines of text on a dark box, rgba
 * XM_STATS_W x XM_STATS_H x 4. */
#define XM_STATS_W 480
#define XM_STATS_H 160
void xm_paint_lines(unsigned char *rgba, const char *const *lines, int n);
/* The row under panel coordinates u, v (0..1 from the top left); -1 if none. */
int  xm_row_at(float u, float v, int n);

#endif
