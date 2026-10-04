#ifndef DS4_JSON_H
#define DS4_JSON_H

/* Shared JSON helpers for the DS4 front ends (server, agent).
 *
 * These were extracted verbatim from ds4_server.c so the server and the agent
 * share one implementation. The header has two halves.
 *
 * Reading: the json_*() functions are a selective reader, not a document
 * model. Each call consumes one value at a cursor (const char **p) that the
 * caller advances by hand, and fields the caller does not ask for are skipped
 * structurally by json_skip_value(). There is no tree to walk: to read a field
 * you must already know its name and position. Successful calls return true and
 * leave a malloc'd, caller-owned string in *out; on failure they return false,
 * leave *out NULL, and the cursor is left at an unspecified position, so a
 * failed read means "reject the document", not "try the next field".
 *
 * Writing: builders append into a buf. json_escape() emits one quoted string
 * and json_append_*() emit whole objects; every other shape is written by hand
 * at the call site as literal buf_puts()/buf_printf() fragments. There is no
 * schema- or struct-driven serializer, so a new output shape is new code.
 *
 * json_args is the one intermediate value model: a flat key/value view of a
 * top-level object that can be parsed, filtered, reordered and re-emitted. It
 * does not descend into nested values -- those are kept as minified raw JSON
 * text and treated as opaque.
 *
 * Out-of-memory and internal buffer errors are fatal. The default handler
 * prints "ds4-json: <msg>" to stderr and exits 1; a program with its own fatal
 * helper should install it via ds4_json_set_fatal() so the message keeps the
 * program's own prefix. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Growable byte buffer. ptr is always NUL-terminated once it is allocated,
 * len is the used length, cap is the allocation size. {0} is a valid empty
 * buffer. buf_take() hands ownership of ptr to the caller and resets the
 * buffer; buf_free() releases it. */
typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
} buf;

void buf_reserve(buf *b, size_t add);
void buf_append(buf *b, const void *p, size_t n);
void buf_putc(buf *b, char c);
void buf_puts(buf *b, const char *s);
void buf_printf(buf *b, const char *fmt, ...);
char *buf_take(buf *b);
void buf_free(buf *b);

/* Fatal handler used by the buffer layer. Passing NULL restores the default. */
typedef void (*ds4_json_fatal_fn)(const char *msg);
void ds4_json_set_fatal(ds4_json_fatal_fn fn);

/* --- reading --- */

/* Skipping unknown values recurses, so it is capped: without a ceiling, a
 * single ignored field like {"x":[[[...]]]} spends the whole C stack before
 * the document is rejected. */
#define JSON_MAX_NESTING 256

/* Skip whitespace in place. */
void json_ws(const char **p);
/* Consume an exact literal ("true", "null", ...). Does not skip whitespace. */
bool json_lit(const char **p, const char *lit);
/* Read a string, unescaping it into UTF-8 (surrogate pairs decoded). */
bool json_string(const char **p, char **out);
bool json_number(const char **p, double *out);
/* Read a number and clamp it to [0, INT_MAX]. strtod() accepts NaN and
 * Infinity, which are clamped rather than left to an undefined cast. */
bool json_int(const char **p, int *out);
bool json_bool(const char **p, bool *out);
/* Skip one value of any type, including containers. */
bool json_skip_value(const char **p);
/* Copy one value verbatim (whitespace and all) into a new string. */
bool json_raw_value(const char **p, char **out);
/* Re-serialize a whole value with inter-token whitespace removed. On a parse
 * failure the input is returned duplicated unchanged. */
char *json_minify_raw_value(const char *json);
/* Read an OpenAI-style "content": a string, null, or an array of strings and
 * {"text": ...} parts, flattened into one string. Other shapes yield "". */
bool json_content(const char **p, char **out);
/* Variants that free *dst and store the new value, leaving *dst NULL if the
 * read fails. */
bool json_string_replace(const char **p, char **dst);
bool json_raw_value_replace(const char **p, char **dst);
bool json_content_replace(const char **p, char **dst);

/* --- flat object model --- */

/* One member of a top-level object. is_string is true when the source value
 * was a JSON string, in which case value is the unescaped text; otherwise
 * value is the minified raw JSON text (numbers, bools, null, arrays, objects).
 * used is scratch space for callers that consume members in schema order. */
typedef struct {
    char *key;
    char *value;
    bool is_string;
    bool used;
} json_arg;

typedef struct {
    json_arg *v;
    int len;
    int cap;
} json_args;

void json_args_free(json_args *args);
void json_args_push(json_args *args, json_arg arg);
/* Index of the first unused member with this key, or -1. */
int json_args_find_unused(json_args *args, const char *key);
/* Flatten a top-level object into args. Requires '{' ... '}'; nested values
 * are kept as opaque raw text. On failure args is left zeroed. */
bool json_args_parse(const char *json, json_args *args);

/* --- writing --- */

/* Emit a quoted string. Escapes '"' '\\' \n \r \t and other controls as
 * \u00xx; bytes >= 0x80 pass through, so input must already be UTF-8. */
void json_escape(buf *b, const char *s);
void json_escape_n(buf *b, const char *s, size_t n);
/* Like json_escape() without the surrounding quotes. */
void json_escape_fragment_n(buf *b, const char *s, size_t n);
/* Emit "key":value for one member (no separating comma). */
void append_json_arg_pair(buf *b, const json_arg *arg);
/* Re-emit a whole object, or "{}" when json does not parse as one. */
void append_json_object_or_empty(buf *b, const char *json);
/* Same, then emitted as an escaped string (for arguments-as-string fields). */
void append_json_object_string(buf *b, const char *json);

#endif /* DS4_JSON_H */
