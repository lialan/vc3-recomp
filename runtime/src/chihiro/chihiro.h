/* Chihiro baseboard HLE: shared declarations. */
#ifndef VC3_CHIHIRO_H
#define VC3_CHIHIRO_H

#include <stdint.h>

/* Per-player JVS switch word, high byte first on the wire. */
#define JVS_SW_START    0x8000
#define JVS_SW_SERVICE  0x4000
#define JVS_SW_UP       0x2000
#define JVS_SW_DOWN     0x1000
#define JVS_SW_LEFT     0x0800
#define JVS_SW_RIGHT    0x0400
#define JVS_SW_PUSH1    0x0200
#define JVS_SW_PUSH2    0x0100
#define JVS_SW_PUSH3    0x0080  /* VC3: CHANGE (input test) */
#define JVS_SW_PUSH4    0x0040  /* VC3: PEDAL */

/* What the I/O board reports, refreshed by the host input layer. */
typedef struct {
    int test;                  /* system switch: test button */
    uint16_t player[2];        /* JVS_SW_* */
    uint16_t analog[8];        /* 16-bit, value in the top bits */
    uint16_t screen_x[2];
    uint16_t screen_y[2];
} chihiro_input_t;

const char *chihiro_save_dir(void);
void chihiro_init(const char *save_dir);

void chihiro_input_set(const chihiro_input_t *in);
void chihiro_coin_insert(int slot);    /* 0-based */

/* src/host_input.c: mouse/keyboard into a snapshot (coins via insert). */
void host_input_poll(chihiro_input_t *in);

#endif
