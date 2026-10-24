#pragma once
#include "cpu/cpu.h"
/* Controller state in GameCube PAD terms. Platform layers (SDL on Android/PC,
 * or a test script) fill this; PADRead (HLE) hands it to the game. */
#define PAD_LEFT  0x0001
#define PAD_RIGHT 0x0002
#define PAD_DOWN  0x0004
#define PAD_UP    0x0008
#define PAD_Z     0x0010
#define PAD_R     0x0020
#define PAD_L     0x0040
#define PAD_A     0x0100
#define PAD_B     0x0200
#define PAD_X     0x0400
#define PAD_Y     0x0800
#define PAD_START 0x1000
typedef struct {
    int connected;
    u16 buttons;
    s8 stick_x, stick_y, cstick_x, cstick_y;
    u8 trigger_l, trigger_r;
} PadState;
extern PadState g_pad[4];
void input_update(unsigned long long frame); /* test script: EED_INPUT="120-130:A,300-310:START" */
