#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Two passes: the first counts nodes and string bytes so a single
 * allocation can hold the whole tree; the second builds it. */
struct parser {
    const char *p, *end;
    struct json *nodes;
    char *strs;
    size_t n_nodes, n_strs;
    int build;
    int depth;
};

static void ws(struct parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static struct json *new_node(struct parser *ps, enum json_type t)
{
    struct json *n = NULL;
    if (ps->build) {
        n = &ps->nodes[ps->n_nodes];
        memset(n, 0, sizeof(*n));
        n->type = t;
    }
    ps->n_nodes++;
    return n;
}

static void put_byte(struct parser *ps, char c)
{
    if (ps->build)
        ps->strs[ps->n_strs] = c;
    ps->n_strs++;
}

static void put_utf8(struct parser *ps, unsigned cp)
{
    if (cp < 0x80) {
        put_byte(ps, (char)cp);
    } else if (cp < 0x800) {
        put_byte(ps, (char)(0xc0 | cp >> 6));
        put_byte(ps, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        put_byte(ps, (char)(0xe0 | cp >> 12));
        put_byte(ps, (char)(0x80 | (cp >> 6 & 0x3f)));
        put_byte(ps, (char)(0x80 | (cp & 0x3f)));
    } else {
        put_byte(ps, (char)(0xf0 | cp >> 18));
        put_byte(ps, (char)(0x80 | (cp >> 12 & 0x3f)));
        put_byte(ps, (char)(0x80 | (cp >> 6 & 0x3f)));
        put_byte(ps, (char)(0x80 | (cp & 0x3f)));
    }
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Parses a string token; returns its start in the string pool. */
static const char *parse_string(struct parser *ps, int *ok)
{
    size_t start = ps->n_strs;
    ps->p++; /* opening quote */
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char)c < 0x20) {
            *ok = 0;
            return NULL;
        }
        if (c != '\\') {
            put_byte(ps, c);
            continue;
        }
        if (ps->p >= ps->end) {
            *ok = 0;
            return NULL;
        }
        c = *ps->p++;
        switch (c) {
        case '"': case '\\': case '/': put_byte(ps, c); break;
        case 'b': put_byte(ps, '\b'); break;
        case 'f': put_byte(ps, '\f'); break;
        case 'n': put_byte(ps, '\n'); break;
        case 'r': put_byte(ps, '\r'); break;
        case 't': put_byte(ps, '\t'); break;
        case 'u': {
            unsigned cp, lo;
            if (ps->end - ps->p < 4 || hex4(ps->p, &cp)) {
                *ok = 0;
                return NULL;
            }
            ps->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' &&
                ps->p[1] == 'u' && !hex4(ps->p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                ps->p += 6;
            }
            put_utf8(ps, cp);
        } break;
        default:
            *ok = 0;
            return NULL;
        }
    }
    if (ps->p >= ps->end) {
        *ok = 0;
        return NULL;
    }
    ps->p++; /* closing quote */
    put_byte(ps, '\0');
    return ps->build ? ps->strs + start : NULL;
}

static struct json *parse_value(struct parser *ps, int *ok);

static struct json *parse_container(struct parser *ps, int *ok, int is_obj)
{
    struct json *n = new_node(ps, is_obj ? JSON_OBJ : JSON_ARR), *last = NULL;
    char close = is_obj ? '}' : ']';
    if (++ps->depth > 64) {
        *ok = 0;
        return NULL;
    }
    ps->p++;
    ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return n;
    }
    for (;;) {
        const char *key = NULL;
        struct json *v;
        ws(ps);
        if (is_obj) {
            if (ps->p >= ps->end || *ps->p != '"') {
                *ok = 0;
                return NULL;
            }
            key = parse_string(ps, ok);
            if (!*ok)
                return NULL;
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                *ok = 0;
                return NULL;
            }
            ps->p++;
        }
        v = parse_value(ps, ok);
        if (!*ok)
            return NULL;
        if (ps->build) {
            v->key = key;
            if (last)
                last->next = v;
            else
                n->child = v;
            last = v;
        }
        ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            ps->depth--;
            return n;
        }
        *ok = 0;
        return NULL;
    }
}

static struct json *parse_value(struct parser *ps, int *ok)
{
    struct json *n;
    ws(ps);
    if (ps->p >= ps->end) {
        *ok = 0;
        return NULL;
    }
    switch (*ps->p) {
    case '{': return parse_container(ps, ok, 1);
    case '[': return parse_container(ps, ok, 0);
    case '"': {
        const char *s;
        n = new_node(ps, JSON_STR);
        s = parse_string(ps, ok);
        if (n)
            n->str = s;
        return n;
    }
    case 't':
    case 'f':
    case 'n': {
        static const struct { const char *lit; enum json_type t; double v; } lits[] = {
            { "true", JSON_BOOL, 1 }, { "false", JSON_BOOL, 0 }, { "null", JSON_NULL, 0 },
        };
        for (size_t i = 0; i < 3; i++) {
            size_t l = strlen(lits[i].lit);
            if ((size_t)(ps->end - ps->p) >= l && memcmp(ps->p, lits[i].lit, l) == 0) {
                ps->p += l;
                n = new_node(ps, lits[i].t);
                if (n)
                    n->num = lits[i].v;
                return n;
            }
        }
        *ok = 0;
        return NULL;
    }
    default: {
        char buf[64];
        size_t l = 0;
        while (ps->p + l < ps->end && l < sizeof(buf) - 1 && strchr("+-0123456789.eE", ps->p[l]))
            l++;
        if (l == 0) {
            *ok = 0;
            return NULL;
        }
        memcpy(buf, ps->p, l);
        buf[l] = '\0';
        ps->p += l;
        n = new_node(ps, JSON_NUM);
        if (n)
            n->num = strtod(buf, NULL);
        return n;
    }
    }
}

struct json *json_parse(const char *text, size_t len)
{
    struct parser ps = { text, text + len, NULL, NULL, 0, 0, 0, 0 };
    int ok = 1;
    void *block;
    struct json *root;

    if (len >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0) /* BOM */
        ps.p += 3;
    parse_value(&ps, &ok);
    if (!ok || ps.n_nodes == 0)
        return NULL;

    block = malloc(ps.n_nodes * sizeof(struct json) + ps.n_strs + 1);
    if (!block)
        return NULL;
    ps.nodes = block;
    ps.strs = (char *)block + ps.n_nodes * sizeof(struct json);
    ps.p = text + (len >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0 ? 3 : 0);
    ps.n_nodes = ps.n_strs = 0;
    ps.build = 1;
    ps.depth = 0;
    root = parse_value(&ps, &ok);
    if (!ok) {
        free(block);
        return NULL;
    }
    return root; /* root == block: the first node allocated */
}

void json_free(struct json *root)
{
    free(root);
}

const struct json *json_get(const struct json *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJ)
        return NULL;
    for (const struct json *c = obj->child; c; c = c->next)
        if (strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

const char *json_str(const struct json *v, const char *fallback)
{
    return v && v->type == JSON_STR ? v->str : fallback;
}

const char *json_get_str(const struct json *obj, const char *key, const char *fallback)
{
    return json_str(json_get(obj, key), fallback);
}

int json_truthy(const struct json *v)
{
    if (!v)
        return 0;
    switch (v->type) {
    case JSON_BOOL:
    case JSON_NUM: return v->num != 0;
    case JSON_STR: return v->str[0] != '\0';
    case JSON_ARR:
    case JSON_OBJ: return 1;
    default: return 0;
    }
}
