#include "t9.h"

#include <ctype.h>
#include <string.h>

static const char *const letters[10] = {
    " 0", ".,?!-'@:/1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
};

void t9_reset(struct t9 *t)
{
    memset(t, 0, sizeof(*t));
}

static void commit(struct t9 *t)
{
    t->pending_key = VK_NONE;
    t->tap = 0;
    if (t->mode == T9_CAPS_ONCE && t->len > 0 && isalpha((unsigned char)t->text[t->len - 1]))
        t->mode = T9_LOWER;
}

static void put(struct t9 *t, char c)
{
    if (t->len < (int)sizeof(t->text) - 1) {
        t->text[t->len++] = c;
        t->text[t->len] = '\0';
    }
}

int t9_key(struct t9 *t, enum vx_key k, long now_ms)
{
    if (k == VK_HASH) {
        if (t->pending_key)
            commit(t);
        t->mode = t->mode == T9_LOWER ? T9_CAPS_ONCE : t->mode == T9_CAPS_ONCE ? T9_DIGITS : T9_LOWER;
        return 1;
    }
    if (k == VK_BACK) {
        if (t->len == 0)
            return 0; /* let the caller treat it as "leave" */
        t->text[--t->len] = '\0';
        t->pending_key = VK_NONE;
        t->tap = 0;
        return 1;
    }
    if (k < VK_0 || k > VK_9)
        return 0;

    int d = k - VK_0;
    if (t->mode == T9_DIGITS) {
        put(t, (char)('0' + d));
        return 1;
    }
    if (t->pending_key == k && now_ms - t->pending_since < T9_COMMIT_MS && t->len > 0) {
        /* cycle the letter under the cursor */
        const char *set = letters[d];
        t->tap = (t->tap + 1) % (int)strlen(set);
        t->text[t->len - 1] = t->mode == T9_CAPS_ONCE ? (char)toupper((unsigned char)set[t->tap]) : set[t->tap];
    } else {
        if (t->pending_key)
            commit(t);
        t->tap = 0;
        put(t, t->mode == T9_CAPS_ONCE ? (char)toupper((unsigned char)letters[d][0]) : letters[d][0]);
        t->pending_key = k;
    }
    t->pending_since = now_ms;
    return 1;
}

int t9_tick(struct t9 *t, long now_ms)
{
    if (t->pending_key && now_ms - t->pending_since >= T9_COMMIT_MS) {
        commit(t);
        return 1;
    }
    return 0;
}

const char *t9_mode_label(const struct t9 *t)
{
    return t->mode == T9_LOWER ? "abc" : t->mode == T9_CAPS_ONCE ? "Abc" : "123";
}
