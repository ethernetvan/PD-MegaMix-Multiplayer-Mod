#pragma once

/* Minimal JSON helpers over vendored jsmn (MpJsmn.h) plus a
 * snprintf-based writer. No allocation; safe on game threads. */

#define JSMN_STATIC
#include "MpJsmn.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mpjson {

/* Parse buf into tok[]. Returns token count or <0 on error. */
inline int Parse(const char* buf, size_t len, jsmntok_t* tok, int max_tok) {
    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, buf, len, tok, max_tok);
}

/* Compare a token to a C string. */
inline bool Eq(const char* buf, const jsmntok_t& t, const char* s) {
    return t.type == JSMN_STRING &&
           (int)strlen(s) == t.end - t.start &&
           strncmp(buf + t.start, s, t.end - t.start) == 0;
}

/* Number of stream slots occupied by the subtree rooted at tok[i]. */
inline int Skip(const jsmntok_t* tok, int i, int tok_count) {
    if (i >= tok_count) return 0;
    int n = 1;
    const jsmntok_t& t = tok[i];
    if (t.type == JSMN_OBJECT) {
        /* each pair: the key token has size 1, so skipping the key also
         * skips its value (see the string branch below) */
        for (int k = 0; k < t.size; k++)
            n += Skip(tok, i + n, tok_count);
    } else if (t.type == JSMN_ARRAY) {
        for (int k = 0; k < t.size; k++)
            n += Skip(tok, i + n, tok_count);
    } else if (t.size > 0) {
        /* string key whose value follows (jsmn sets key.size = 1) */
        n += Skip(tok, i + n, tok_count);
    }
    return n;
}

/* Find key in a top-level object token array. Returns the *value* token
 * index or -1. obj_idx should be index of the root object (usually 0). */
inline int Find(const char* buf, const jsmntok_t* tok, int tok_count,
                const char* key, int obj_idx = 0) {
    if (tok_count <= 0 || tok[obj_idx].type != JSMN_OBJECT)
        return -1;
    int i = obj_idx + 1;
    int fields = tok[obj_idx].size;
    for (int f = 0; f < fields && i < tok_count; f++) {
        int key_idx = i;
        int val_idx = i + 1;   /* keys are always a single token */
        if (val_idx >= tok_count) break;
        if (tok[key_idx].type == JSMN_STRING && Eq(buf, tok[key_idx], key))
            return val_idx;
        i = val_idx + Skip(tok, val_idx, tok_count);
    }
    return -1;
}

/* Copy token's raw text into out (NUL-terminated). */
inline int Copy(const char* buf, const jsmntok_t& t, char* out, size_t cap) {
    if (cap == 0) return 0;
    int n = t.end - t.start;
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    memcpy(out, buf + t.start, n);
    out[n] = '\0';
    return n;
}

inline bool ToInt(const char* buf, const jsmntok_t& t, int* out) {
    if (t.type != JSMN_PRIMITIVE) return false;
    char tmp[24];
    if (Copy(buf, t, tmp, sizeof(tmp)) <= 0) return false;
    *out = (int)strtol(tmp, nullptr, 10);
    return true;
}

inline bool ToBool(const char* buf, const jsmntok_t& t, bool* out) {
    if (t.type != JSMN_PRIMITIVE) return false;
    if (buf[t.start] == 't') { *out = true;  return true; }
    if (buf[t.start] == 'f') { *out = false; return true; }
    return false;
}

/* --- writer --------------------------------------------------------- */

/* Append JSON-escaped s into dst (cap total incl. NUL). Returns bytes
 * appended (excl. NUL) or 0 on overflow. */
inline size_t EscapeInto(char* dst, size_t cap, const char* s) {
    size_t o = 0;
    while (*s && o + 2 < cap) {
        unsigned char c = (unsigned char)*s++;
        const char* rep = nullptr;
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default: break;
        }
        if (rep) {
            dst[o++] = rep[0];
            dst[o++] = rep[1];
        } else if (c >= 0x20) {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

} // namespace mpjson
