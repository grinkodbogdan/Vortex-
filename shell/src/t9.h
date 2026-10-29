/* Multi-tap text entry on the 12-key pad, the way KaiOS does it:
 * press 2 twice for "b", wait (or press another key) to commit,
 * '#' cycles abc/Abc/123, '0' is space, back deletes. */
#ifndef VX_T9_H
#define VX_T9_H

#include "vx.h"

enum t9_mode { T9_LOWER, T9_CAPS_ONCE, T9_DIGITS };

struct t9 {
    char text[64];
    int len;
    enum t9_mode mode;
    enum vx_key pending_key;
    int tap;            /* index into the key's letters */
    long pending_since; /* ms */
};

void t9_reset(struct t9 *t);
/* Returns 1 if the key was consumed. */
int  t9_key(struct t9 *t, enum vx_key k, long now_ms);
/* Commit a pending letter after the timeout. Returns 1 if it did. */
int  t9_tick(struct t9 *t, long now_ms);
const char *t9_mode_label(const struct t9 *t);

#define T9_COMMIT_MS 900

#endif
