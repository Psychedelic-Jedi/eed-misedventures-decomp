#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"

PadState g_pad[4] = {{1}};

/* Scripted input for headless testing: comma-separated "first-last:BUTTON[+BUTTON]"
 * entries, frame numbers in VI retraces. */
void input_update(unsigned long long frame) {
    static const struct { const char *n; u16 b; } names[] = {
        {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"START", PAD_START}, {"Z", PAD_Z},
        {"L", PAD_L}, {"R", PAD_R}, {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}};
    const char *script = getenv("EED_INPUT");
    g_pad[0].buttons = 0;
    g_pad[0].stick_x = g_pad[0].stick_y = 0;
    if (!script) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", script);
    char *outer;
    for (char *e = strtok_r(buf, ",", &outer); e; e = strtok_r(NULL, ",", &outer)) {
        unsigned long long a, b;
        char btns[128];
        if (sscanf(e, "%llu-%llu:%127s", &a, &b, btns) != 3 || frame < a || frame > b) continue;
        char *save;
        for (char *t = strtok_r(btns, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
            for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
                if (!strcmp(t, names[i].n)) g_pad[0].buttons |= names[i].b;
            if (!strcmp(t, "SUP")) g_pad[0].stick_y = 100;     /* analog stick */
            if (!strcmp(t, "SDOWN")) g_pad[0].stick_y = -100;
            if (!strcmp(t, "SLEFT")) g_pad[0].stick_x = -100;
            if (!strcmp(t, "SRIGHT")) g_pad[0].stick_x = 100;
        }
    }
}
