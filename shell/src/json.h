/* Minimal JSON DOM.  Parsing allocates one block for all nodes and string
 * data; json_free() releases everything.  Meant for short-lived use: the
 * shell copies what it needs out of a document and frees it at once. */
#ifndef VX_JSON_H
#define VX_JSON_H

#include <stddef.h>

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

struct json {
    enum json_type type;
    const char *key;      /* member name when inside an object */
    struct json *next;    /* next sibling */
    struct json *child;   /* first element / member */
    const char *str;      /* JSON_STR (unescaped, UTF-8) */
    double num;           /* JSON_NUM, JSON_BOOL */
};

struct json *json_parse(const char *text, size_t len);
void json_free(struct json *root);

const struct json *json_get(const struct json *obj, const char *key);
const char *json_str(const struct json *v, const char *fallback);
const char *json_get_str(const struct json *obj, const char *key, const char *fallback);
int json_truthy(const struct json *v);

#endif
