/*
 * QuickJS source map support
 *
 * Copyright (c) 2026 Enuan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Source map v3 (ECMA-426) decoding and lookup.
 *
 * - regular maps and index maps ("sections" with embedded "map"; "url"
 *   sections are not supported and are ignored)
 * - 'sources' are resolved against the map URL/path (after 'sourceRoot')
 * - lookup: greatest generated column <= the requested one on the same
 *   generated line; segments without a source position are "unmapped"
 * - 'names' and 'sourcesContent' are parsed but not used
 *
 * The JSON parser below is self-contained: decoding must not run JS code
 * nor throw JS exceptions because it happens while a backtrace is built.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cutils.h"
#include "quickjs.h"
#include "quickjs-sourcemap.h"

#define SM_MAX_DEPTH 64

typedef struct {
    int32_t gen_col;
    int32_t source; /* -1 if the segment has no source position */
    int32_t line;
    int32_t col;
} SMSegment;

typedef struct {
    uint32_t start;
    uint32_t count;
} SMLine;

typedef struct SMMap SMMap;

typedef struct {
    int32_t line; /* 0-based offset of the section in the generated code */
    int32_t col;
    SMMap *map;
} SMSection;

struct SMMap {
    int sources_count;
    char **sources; /* resolved; NULL entries for null sources */
    int lines_count;
    SMLine *lines;
    uint32_t segments_count;
    SMSegment *segments;
    int sections_count; /* > 0 for an index map */
    SMSection *sections;
};

typedef enum {
    SM_STATE_PENDING,
    SM_STATE_DECODED,
    SM_STATE_INVALID,
} SMState;

typedef struct SMEntry {
    struct SMEntry *next;
    uint32_t hash;
    char *filename;
    char *json; /* undecoded map, freed once decoded */
    size_t json_len;
    char *base;
    SMState state;
    SMMap *map;
} SMEntry;

struct JSSourceMapRegistry {
    uint32_t hash_size; /* power of two */
    uint32_t count;
    SMEntry **hash;
};

/*******************************************************************/
/* utilities */

static char *sm_strndup(JSRuntime *rt, const char *s, size_t len)
{
    char *p = js_malloc_rt(rt, len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static uint32_t sm_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static BOOL sm_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static BOOL sm_is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/*******************************************************************/
/* map decoding */

static void sm_map_free(JSRuntime *rt, SMMap *m)
{
    int i;
    if (!m)
        return;
    for(i = 0; i < m->sources_count; i++)
        js_free_rt(rt, m->sources[i]);
    js_free_rt(rt, m->sources);
    js_free_rt(rt, m->lines);
    js_free_rt(rt, m->segments);
    for(i = 0; i < m->sections_count; i++)
        sm_map_free(rt, m->sections[i].map);
    js_free_rt(rt, m->sections);
    js_free_rt(rt, m);
}

typedef struct {
    JSRuntime *rt;
    const uint8_t *p;
    const uint8_t *end;
} SMParser;

/* raw (unresolved) content of a map object */
typedef struct {
    BOOL has_version;
    int64_t version;
    char *source_root;
    int sources_count;
    int sources_size;
    char **sources;
    char *mappings;
    BOOL has_sections;
    int sections_count;
    int sections_size;
    SMSection *sections;
} SMRawMap;

static void sm_skip_ws(SMParser *s)
{
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

static int sm_expect(SMParser *s, int c)
{
    sm_skip_ws(s);
    if (s->p >= s->end || *s->p != c)
        return -1;
    s->p++;
    return 0;
}

static int sm_hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int sm_parse_hex4(SMParser *s, uint32_t *pc)
{
    uint32_t c = 0;
    int i, h;
    if (s->end - s->p < 4)
        return -1;
    for(i = 0; i < 4; i++) {
        h = sm_hex(s->p[i]);
        if (h < 0)
            return -1;
        c = (c << 4) | h;
    }
    s->p += 4;
    *pc = c;
    return 0;
}

/* if 'pstr' is NULL, the string is only validated */
static int sm_parse_string(SMParser *s, char **pstr)
{
    DynBuf dbuf;
    uint8_t buf[UTF8_CHAR_LEN_MAX];
    uint32_t c, c2;
    int c1;

    sm_skip_ws(s);
    if (s->p >= s->end || *s->p != '"')
        return -1;
    s->p++;
    dbuf_init2(&dbuf, s->rt, (DynBufReallocFunc *)js_realloc_rt);
    for(;;) {
        if (s->p >= s->end)
            goto fail;
        c1 = *s->p++;
        if (c1 == '"')
            break;
        if (c1 < 0x20)
            goto fail;
        if (c1 != '\\') {
            if (pstr && dbuf_putc(&dbuf, c1))
                goto fail;
            continue;
        }
        if (s->p >= s->end)
            goto fail;
        c1 = *s->p++;
        switch(c1) {
        case '"': case '\\': case '/':
            c = c1;
            break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u':
            if (sm_parse_hex4(s, &c))
                goto fail;
            if (c >= 0xd800 && c < 0xdc00 && s->end - s->p >= 6 &&
                s->p[0] == '\\' && s->p[1] == 'u') {
                const uint8_t *p_save = s->p;
                s->p += 2;
                if (sm_parse_hex4(s, &c2) == 0 && c2 >= 0xdc00 && c2 < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
                } else {
                    s->p = p_save;
                }
            }
            break;
        default:
            goto fail;
        }
        if (pstr) {
            if (c < 0x80) {
                if (dbuf_putc(&dbuf, c))
                    goto fail;
            } else {
                if (dbuf_put(&dbuf, buf, unicode_to_utf8(buf, c)))
                    goto fail;
            }
        }
    }
    if (pstr) {
        if (dbuf_putc(&dbuf, '\0'))
            goto fail;
        *pstr = (char *)dbuf.buf;
    } else {
        dbuf_free(&dbuf);
    }
    return 0;
 fail:
    dbuf_free(&dbuf);
    return -1;
}

/* JSON number; *pis_int is set if it is an integer fitting in int64 */
static int sm_parse_number(SMParser *s, int64_t *pval, BOOL *pis_int)
{
    int64_t v = 0;
    BOOL neg = FALSE, is_int = TRUE;

    sm_skip_ws(s);
    if (s->p < s->end && *s->p == '-') {
        neg = TRUE;
        s->p++;
    }
    if (s->p >= s->end || !sm_is_digit(*s->p))
        return -1;
    while (s->p < s->end && sm_is_digit(*s->p)) {
        if (v > (INT64_MAX - 9) / 10)
            is_int = FALSE;
        else
            v = v * 10 + (*s->p - '0');
        s->p++;
    }
    if (s->p < s->end && *s->p == '.') {
        s->p++;
        if (s->p >= s->end || !sm_is_digit(*s->p))
            return -1;
        while (s->p < s->end && sm_is_digit(*s->p)) {
            if (*s->p != '0')
                is_int = FALSE;
            s->p++;
        }
    }
    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        s->p++;
        is_int = FALSE;
        if (s->p < s->end && (*s->p == '+' || *s->p == '-'))
            s->p++;
        if (s->p >= s->end || !sm_is_digit(*s->p))
            return -1;
        while (s->p < s->end && sm_is_digit(*s->p))
            s->p++;
    }
    *pval = neg ? -v : v;
    *pis_int = is_int;
    return 0;
}

static int sm_match_literal(SMParser *s, const char *lit)
{
    size_t len = strlen(lit);
    if ((size_t)(s->end - s->p) < len || memcmp(s->p, lit, len) != 0)
        return -1;
    s->p += len;
    return 0;
}

static int sm_skip_value(SMParser *s, int depth)
{
    int64_t v;
    BOOL is_int;

    if (depth > SM_MAX_DEPTH)
        return -1;
    sm_skip_ws(s);
    if (s->p >= s->end)
        return -1;
    switch(*s->p) {
    case '"':
        return sm_parse_string(s, NULL);
    case '{':
        s->p++;
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return 0;
        }
        for(;;) {
            if (sm_parse_string(s, NULL) || sm_expect(s, ':') ||
                sm_skip_value(s, depth + 1))
                return -1;
            sm_skip_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            return sm_expect(s, '}');
        }
    case '[':
        s->p++;
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            return 0;
        }
        for(;;) {
            if (sm_skip_value(s, depth + 1))
                return -1;
            sm_skip_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            return sm_expect(s, ']');
        }
    case 't':
        return sm_match_literal(s, "true");
    case 'f':
        return sm_match_literal(s, "false");
    case 'n':
        return sm_match_literal(s, "null");
    default:
        return sm_parse_number(s, &v, &is_int);
    }
}

static BOOL sm_peek_null(SMParser *s)
{
    sm_skip_ws(s);
    if (sm_match_literal(s, "null") == 0)
        return TRUE;
    return FALSE;
}

static void sm_raw_free(JSRuntime *rt, SMRawMap *r)
{
    int i;
    js_free_rt(rt, r->source_root);
    for(i = 0; i < r->sources_count; i++)
        js_free_rt(rt, r->sources[i]);
    js_free_rt(rt, r->sources);
    js_free_rt(rt, r->mappings);
    for(i = 0; i < r->sections_count; i++)
        sm_map_free(rt, r->sections[i].map);
    js_free_rt(rt, r->sections);
}

static int sm_parse_sources(SMParser *s, SMRawMap *r)
{
    char *str;

    if (sm_expect(s, '['))
        return -1;
    sm_skip_ws(s);
    if (s->p < s->end && *s->p == ']') {
        s->p++;
        return 0;
    }
    for(;;) {
        if (sm_peek_null(s)) {
            str = NULL;
        } else if (sm_parse_string(s, &str)) {
            return -1;
        }
        if (r->sources_count >= r->sources_size) {
            int new_size = r->sources_size * 2 + 8;
            char **tab = js_realloc_rt(s->rt, r->sources,
                                       sizeof(r->sources[0]) * new_size);
            if (!tab) {
                js_free_rt(s->rt, str);
                return -1;
            }
            r->sources = tab;
            r->sources_size = new_size;
        }
        r->sources[r->sources_count++] = str;
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        return sm_expect(s, ']');
    }
}

static int sm_parse_map_object(SMParser *s, const char *base, int depth,
                               SMMap **pmap);

/* { "line": n, "column": n } */
static int sm_parse_offset(SMParser *s, int32_t *pline, int32_t *pcol)
{
    char *key;
    int64_t v;
    BOOL is_int, has_line = FALSE, has_col = FALSE;

    if (sm_expect(s, '{'))
        return -1;
    sm_skip_ws(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return -1;
    }
    for(;;) {
        if (sm_parse_string(s, &key))
            return -1;
        if (sm_expect(s, ':')) {
            js_free_rt(s->rt, key);
            return -1;
        }
        if (!strcmp(key, "line") || !strcmp(key, "column")) {
            BOOL is_line = (key[0] == 'l');
            js_free_rt(s->rt, key);
            if (sm_parse_number(s, &v, &is_int) || !is_int ||
                v < 0 || v > INT32_MAX)
                return -1;
            if (is_line) {
                *pline = v;
                has_line = TRUE;
            } else {
                *pcol = v;
                has_col = TRUE;
            }
        } else {
            js_free_rt(s->rt, key);
            if (sm_skip_value(s, 1))
                return -1;
        }
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (sm_expect(s, '}'))
            return -1;
        return (has_line && has_col) ? 0 : -1;
    }
}

/* one element of "sections". *psection->map is NULL if the section must be
   ignored (e.g. "url" sections). */
static int sm_parse_section(SMParser *s, const char *base, int depth,
                            SMSection *psection)
{
    char *key;
    BOOL has_offset = FALSE;

    psection->map = NULL;
    if (sm_expect(s, '{'))
        return -1;
    sm_skip_ws(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return -1;
    }
    for(;;) {
        if (sm_parse_string(s, &key))
            goto fail;
        if (sm_expect(s, ':')) {
            js_free_rt(s->rt, key);
            goto fail;
        }
        if (!strcmp(key, "offset")) {
            js_free_rt(s->rt, key);
            if (sm_parse_offset(s, &psection->line, &psection->col))
                goto fail;
            has_offset = TRUE;
        } else if (!strcmp(key, "map") && !psection->map) {
            js_free_rt(s->rt, key);
            if (sm_parse_map_object(s, base, depth + 1, &psection->map))
                goto fail;
        } else {
            js_free_rt(s->rt, key);
            if (sm_skip_value(s, depth + 1))
                goto fail;
        }
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (sm_expect(s, '}') || !has_offset)
            goto fail;
        return 0;
    }
 fail:
    sm_map_free(s->rt, psection->map);
    psection->map = NULL;
    return -1;
}

static int sm_parse_sections(SMParser *s, const char *base, int depth,
                             SMRawMap *r)
{
    SMSection sec;

    if (sm_expect(s, '['))
        return -1;
    sm_skip_ws(s);
    if (s->p < s->end && *s->p == ']') {
        s->p++;
        return 0;
    }
    for(;;) {
        if (sm_parse_section(s, base, depth, &sec))
            return -1;
        if (sec.map) {
            if (r->sections_count > 0) {
                /* sections must be sorted and must not overlap */
                SMSection *prev = &r->sections[r->sections_count - 1];
                if (sec.line < prev->line ||
                    (sec.line == prev->line && sec.col < prev->col)) {
                    sm_map_free(s->rt, sec.map);
                    return -1;
                }
            }
            if (r->sections_count >= r->sections_size) {
                int new_size = r->sections_size * 2 + 4;
                SMSection *tab = js_realloc_rt(s->rt, r->sections,
                                               sizeof(r->sections[0]) * new_size);
                if (!tab) {
                    sm_map_free(s->rt, sec.map);
                    return -1;
                }
                r->sections = tab;
                r->sections_size = new_size;
            }
            r->sections[r->sections_count++] = sec;
        }
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        return sm_expect(s, ']');
    }
}

static int sm_base64_value(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int sm_decode_vlq(const char **pp, int64_t *pval)
{
    const char *p = *pp;
    uint64_t v = 0;
    int shift = 0, digit;

    for(;;) {
        digit = sm_base64_value((uint8_t)*p);
        if (digit < 0)
            return -1;
        p++;
        v |= (uint64_t)(digit & 31) << shift;
        if (!(digit & 32))
            break;
        shift += 5;
        if (shift > 35)
            return -1;
    }
    *pp = p;
    *pval = (v & 1) ? -(int64_t)(v >> 1) : (int64_t)(v >> 1);
    return 0;
}

static int sm_add_check(int64_t *pacc, int64_t delta)
{
    int64_t v = *pacc + delta;
    if (v < 0 || v > INT32_MAX)
        return -1;
    *pacc = v;
    return 0;
}

static int sm_decode_mappings(JSRuntime *rt, SMMap *m, const char *mappings)
{
    const char *p = mappings;
    int64_t gen_col = 0, src = 0, line = 0, col = 0, name = 0, v;
    uint32_t seg_size = 0, line_size = 0, i, j;
    int nfields;
    int64_t fields[5];
    SMSegment seg;

    m->lines_count = 0;
    m->segments_count = 0;
    /* first line */
    line_size = 16;
    m->lines = js_malloc_rt(rt, sizeof(m->lines[0]) * line_size);
    if (!m->lines)
        return -1;
    m->lines[0].start = 0;
    m->lines[0].count = 0;
    m->lines_count = 1;

    while (*p) {
        if (*p == ';') {
            p++;
            if (m->lines_count >= (int)line_size) {
                uint32_t new_size = line_size * 2;
                SMLine *tab = js_realloc_rt(rt, m->lines,
                                            sizeof(m->lines[0]) * new_size);
                if (!tab)
                    return -1;
                m->lines = tab;
                line_size = new_size;
            }
            m->lines[m->lines_count].start = m->segments_count;
            m->lines[m->lines_count].count = 0;
            m->lines_count++;
            gen_col = 0;
            continue;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        nfields = 0;
        while (*p && *p != ',' && *p != ';') {
            if (nfields >= 5 || sm_decode_vlq(&p, &v))
                return -1;
            fields[nfields++] = v;
        }
        if (nfields != 1 && nfields != 4 && nfields != 5)
            return -1;
        if (sm_add_check(&gen_col, fields[0]))
            return -1;
        seg.gen_col = gen_col;
        seg.source = -1;
        seg.line = 0;
        seg.col = 0;
        if (nfields >= 4) {
            if (sm_add_check(&src, fields[1]) ||
                sm_add_check(&line, fields[2]) ||
                sm_add_check(&col, fields[3]))
                return -1;
            if (src >= m->sources_count)
                return -1;
            if (nfields == 5 && sm_add_check(&name, fields[4]))
                return -1;
            /* a null source is equivalent to no source position */
            if (m->sources[src]) {
                seg.source = src;
                seg.line = line;
                seg.col = col;
            }
        }
        if (m->segments_count >= seg_size) {
            uint32_t new_size = seg_size * 2 + 64;
            SMSegment *tab = js_realloc_rt(rt, m->segments,
                                           sizeof(m->segments[0]) * new_size);
            if (!tab)
                return -1;
            m->segments = tab;
            seg_size = new_size;
        }
        m->segments[m->segments_count++] = seg;
        m->lines[m->lines_count - 1].count++;
    }

    /* segments are normally sorted by generated column; otherwise use a
       stable insertion sort so that the first of equal columns wins */
    for(i = 0; i < (uint32_t)m->lines_count; i++) {
        SMSegment *tab = m->segments + m->lines[i].start;
        uint32_t n = m->lines[i].count;
        for(j = 1; j < n; j++) {
            if (tab[j].gen_col < tab[j - 1].gen_col) {
                SMSegment tmp = tab[j];
                uint32_t k = j;
                while (k > 0 && tab[k - 1].gen_col > tmp.gen_col) {
                    tab[k] = tab[k - 1];
                    k--;
                }
                tab[k] = tmp;
            }
        }
    }
    return 0;
}

/* 'path' is modified in place: remove the empty, '.' and '..' segments
   after the prefix of length 'prefix_len' */
static void sm_normalize_path(char *path, size_t prefix_len)
{
    char *out = path + prefix_len; /* start of the output segments */
    char *src = out, *src_end, *dst = out, *seg, *last;
    size_t len;
    BOOL absolute = (prefix_len > 0 && path[prefix_len - 1] == '/');
    BOOL trailing_slash;

    src_end = src + strlen(src);
    trailing_slash = (src_end > src && src_end[-1] == '/');
    /* invariant: dst <= src and the output is the segments separated by
       '/' without trailing '/' */
    while (src < src_end) {
        seg = src;
        while (src < src_end && *src != '/')
            src++;
        len = src - seg;
        if (src < src_end)
            src++; /* skip the '/' */
        if (len == 0 || (len == 1 && seg[0] == '.'))
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (dst > out) {
                last = dst;
                while (last > out && last[-1] != '/')
                    last--;
                if (!(dst - last == 2 && last[0] == '.' && last[1] == '.')) {
                    /* remove the last segment and its separator */
                    dst = (last > out) ? last - 1 : out;
                    continue;
                }
            } else if (absolute) {
                continue; /* cannot go above the root */
            }
        }
        if (dst > out)
            *dst++ = '/';
        memmove(dst, seg, len);
        dst += len;
    }
    if (trailing_slash && dst > out)
        *dst++ = '/';
    *dst = '\0';
}

static BOOL sm_has_scheme(const char *s)
{
    const char *p;
    if (!sm_is_alpha(*s))
        return FALSE;
    for(p = s + 1; *p; p++) {
        if (*p == ':')
            return TRUE;
        if (!sm_is_alpha(*p) && !sm_is_digit(*p) &&
            *p != '+' && *p != '-' && *p != '.')
            return FALSE;
    }
    return FALSE;
}

/* length of the part of 'url' which is not a path: "scheme://authority",
   "scheme:" or "/" */
static size_t sm_path_prefix_len(const char *url)
{
    const char *p;
    if (url[0] == '/')
        return 1;
    if (sm_has_scheme(url)) {
        p = strchr(url, ':') + 1;
        if (p[0] == '/' && p[1] == '/') {
            p = strchr(p + 2, '/');
            if (!p)
                return strlen(url);
            return p - url + 1;
        }
        if (p[0] == '/')
            p++;
        return p - url;
    }
    return 0;
}

static char *sm_resolve_source(JSRuntime *rt, const char *base,
                               const char *source_root, const char *source)
{
    DynBuf dbuf;
    const char *slash;
    char *res;

    dbuf_init2(&dbuf, rt, (DynBufReallocFunc *)js_realloc_rt);
    if (source_root && source_root[0] != '\0' &&
        source[0] != '/' && !sm_has_scheme(source)) {
        size_t root_len = strlen(source_root);
        dbuf_putstr(&dbuf, source_root);
        if (source_root[root_len - 1] != '/')
            dbuf_putc(&dbuf, '/');
    }
    dbuf_putstr(&dbuf, source);
    if (dbuf_putc(&dbuf, '\0'))
        goto fail;

    res = (char *)dbuf.buf;
    if (res[0] != '/' && !sm_has_scheme(res) && base) {
        /* relative: resolve against the directory of the map */
        DynBuf dbuf2;
        dbuf_init2(&dbuf2, rt, (DynBufReallocFunc *)js_realloc_rt);
        slash = strrchr(base, '/');
        if (slash)
            dbuf_put(&dbuf2, (const uint8_t *)base, slash - base + 1);
        dbuf_putstr(&dbuf2, res);
        if (dbuf_putc(&dbuf2, '\0')) {
            dbuf_free(&dbuf2);
            goto fail;
        }
        dbuf_free(&dbuf);
        dbuf = dbuf2;
        res = (char *)dbuf.buf;
    }
    sm_normalize_path(res, sm_path_prefix_len(res));
    return res;
 fail:
    dbuf_free(&dbuf);
    return NULL;
}

static int sm_parse_map_object(SMParser *s, const char *base, int depth,
                               SMMap **pmap)
{
    SMRawMap r;
    SMMap *m = NULL;
    char *key;
    int64_t v;
    BOOL is_int;
    int i;

    *pmap = NULL;
    memset(&r, 0, sizeof(r));
    if (depth > 1) /* nested index maps are not allowed */
        return -1;
    if (sm_expect(s, '{'))
        return -1;
    sm_skip_ws(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        goto fail;
    }
    for(;;) {
        if (sm_parse_string(s, &key))
            goto fail;
        if (sm_expect(s, ':')) {
            js_free_rt(s->rt, key);
            goto fail;
        }
        if (!strcmp(key, "version")) {
            if (sm_parse_number(s, &v, &is_int) || !is_int)
                goto fail_key;
            r.has_version = TRUE;
            r.version = v;
        } else if (!strcmp(key, "sourceRoot") && !r.source_root) {
            if (!sm_peek_null(s) && sm_parse_string(s, &r.source_root))
                goto fail_key;
        } else if (!strcmp(key, "sources") && !r.sources) {
            if (sm_parse_sources(s, &r))
                goto fail_key;
        } else if (!strcmp(key, "mappings") && !r.mappings) {
            if (sm_parse_string(s, &r.mappings))
                goto fail_key;
        } else if (!strcmp(key, "sections") && !r.has_sections) {
            r.has_sections = TRUE;
            if (sm_parse_sections(s, base, depth, &r))
                goto fail_key;
        } else {
            if (sm_skip_value(s, depth + 1))
                goto fail_key;
        }
        js_free_rt(s->rt, key);
        sm_skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (sm_expect(s, '}'))
            goto fail;
        break;
    fail_key:
        js_free_rt(s->rt, key);
        goto fail;
    }

    if (!r.has_version || r.version != 3)
        goto fail;
    m = js_mallocz_rt(s->rt, sizeof(*m));
    if (!m)
        goto fail;
    if (r.has_sections) {
        if (r.mappings || r.sources)
            goto fail;
        m->sections = r.sections;
        m->sections_count = r.sections_count;
        r.sections = NULL;
        r.sections_count = 0;
    } else {
        if (!r.mappings)
            goto fail;
        m->sources_count = r.sources_count;
        if (r.sources_count > 0) {
            m->sources = js_mallocz_rt(s->rt, sizeof(m->sources[0]) *
                                       r.sources_count);
            if (!m->sources) {
                m->sources_count = 0;
                goto fail;
            }
            for(i = 0; i < r.sources_count; i++) {
                if (r.sources[i]) {
                    m->sources[i] = sm_resolve_source(s->rt, base,
                                                      r.source_root,
                                                      r.sources[i]);
                    if (!m->sources[i])
                        goto fail;
                }
            }
        }
        if (sm_decode_mappings(s->rt, m, r.mappings))
            goto fail;
    }
    sm_raw_free(s->rt, &r);
    *pmap = m;
    return 0;
 fail:
    sm_map_free(s->rt, m);
    sm_raw_free(s->rt, &r);
    return -1;
}

static SMMap *sm_decode(JSRuntime *rt, const char *json, size_t json_len,
                        const char *base)
{
    SMParser s;
    SMMap *m;

    s.rt = rt;
    s.p = (const uint8_t *)json;
    s.end = s.p + json_len;
    /* optional ")]}'" XSSI prefix line (accepted by the spec) */
    if (json_len >= 4 && !memcmp(json, ")]}'", 4)) {
        while (s.p < s.end && *s.p != '\n')
            s.p++;
    }
    /* UTF-8 BOM */
    if (s.end - s.p >= 3 && s.p[0] == 0xef && s.p[1] == 0xbb && s.p[2] == 0xbf)
        s.p += 3;
    if (sm_parse_map_object(&s, base, 0, &m))
        return NULL;
    sm_skip_ws(&s);
    if (s.p != s.end) {
        sm_map_free(rt, m);
        return NULL;
    }
    return m;
}

static BOOL sm_map_lookup(SMMap *m, int32_t line, int32_t col,
                          const char **psource, int *pline, int *pcol)
{
    const SMSegment *tab, *seg;
    uint32_t n, lo, hi, mid;

    if (m->sections_count > 0) {
        int lo1 = 0, hi1 = m->sections_count - 1, mid1, found = -1;
        const SMSection *sec;
        /* last section whose offset is <= (line, col) */
        while (lo1 <= hi1) {
            mid1 = (lo1 + hi1) / 2;
            sec = &m->sections[mid1];
            if (sec->line < line || (sec->line == line && sec->col <= col)) {
                found = mid1;
                lo1 = mid1 + 1;
            } else {
                hi1 = mid1 - 1;
            }
        }
        if (found < 0)
            return FALSE;
        sec = &m->sections[found];
        if (line == sec->line)
            col -= sec->col;
        line -= sec->line;
        return sm_map_lookup(sec->map, line, col, psource, pline, pcol);
    }

    if (line < 0 || line >= m->lines_count)
        return FALSE;
    tab = m->segments + m->lines[line].start;
    n = m->lines[line].count;
    /* greatest generated column <= col */
    lo = 0;
    hi = n;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (tab[mid].gen_col <= col)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return FALSE;
    seg = &tab[lo - 1];
    while (seg > tab && seg[-1].gen_col == seg->gen_col)
        seg--;
    if (seg->source < 0)
        return FALSE;
    *psource = m->sources[seg->source];
    *pline = seg->line + 1;
    *pcol = seg->col + 1;
    return TRUE;
}

/*******************************************************************/
/* registry */

static void sm_entry_free(JSRuntime *rt, SMEntry *e)
{
    js_free_rt(rt, e->filename);
    js_free_rt(rt, e->json);
    js_free_rt(rt, e->base);
    sm_map_free(rt, e->map);
    js_free_rt(rt, e);
}

void js_source_maps_free(JSRuntime *rt, JSSourceMapRegistry *reg)
{
    uint32_t i;
    SMEntry *e, *e_next;

    if (!reg)
        return;
    for(i = 0; i < reg->hash_size; i++) {
        for(e = reg->hash[i]; e != NULL; e = e_next) {
            e_next = e->next;
            sm_entry_free(rt, e);
        }
    }
    js_free_rt(rt, reg->hash);
    js_free_rt(rt, reg);
}

static SMEntry **sm_find(JSSourceMapRegistry *reg, const char *filename,
                         uint32_t hash)
{
    SMEntry **pe;
    for(pe = &reg->hash[hash & (reg->hash_size - 1)]; *pe != NULL;
        pe = &(*pe)->next) {
        if ((*pe)->hash == hash && !strcmp((*pe)->filename, filename))
            return pe;
    }
    return pe;
}

static int sm_resize(JSRuntime *rt, JSSourceMapRegistry *reg,
                     uint32_t new_size)
{
    SMEntry **new_hash, *e, *e_next;
    uint32_t i;

    new_hash = js_mallocz_rt(rt, sizeof(new_hash[0]) * new_size);
    if (!new_hash)
        return -1;
    for(i = 0; i < reg->hash_size; i++) {
        for(e = reg->hash[i]; e != NULL; e = e_next) {
            e_next = e->next;
            e->next = new_hash[e->hash & (new_size - 1)];
            new_hash[e->hash & (new_size - 1)] = e;
        }
    }
    js_free_rt(rt, reg->hash);
    reg->hash = new_hash;
    reg->hash_size = new_size;
    return 0;
}

int js_source_maps_set(JSRuntime *rt, JSSourceMapRegistry **preg,
                       const char *filename, const char *map, size_t map_len,
                       const char *base)
{
    JSSourceMapRegistry *reg = *preg;
    SMEntry *e, **pe;
    uint32_t hash;

    if (!reg) {
        reg = js_mallocz_rt(rt, sizeof(*reg));
        if (!reg)
            return -1;
        reg->hash_size = 16;
        reg->hash = js_mallocz_rt(rt, sizeof(reg->hash[0]) * reg->hash_size);
        if (!reg->hash) {
            js_free_rt(rt, reg);
            return -1;
        }
        *preg = reg;
    }

    e = js_mallocz_rt(rt, sizeof(*e));
    if (!e)
        return -1;
    e->filename = sm_strndup(rt, filename, strlen(filename));
    e->json = sm_strndup(rt, map, map_len);
    e->json_len = map_len;
    e->base = sm_strndup(rt, base ? base : filename,
                         strlen(base ? base : filename));
    if (!e->filename || !e->json || !e->base) {
        sm_entry_free(rt, e);
        return -1;
    }
    e->state = SM_STATE_PENDING;
    hash = sm_hash(filename);
    e->hash = hash;

    pe = sm_find(reg, filename, hash);
    if (*pe) {
        /* replace */
        e->next = (*pe)->next;
        sm_entry_free(rt, *pe);
        *pe = e;
        return 0;
    }
    if (reg->count >= reg->hash_size && reg->hash_size < (1u << 30)) {
        if (sm_resize(rt, reg, reg->hash_size * 2) == 0)
            pe = sm_find(reg, filename, hash);
    }
    e->next = NULL;
    *pe = e;
    reg->count++;
    return 0;
}

JS_BOOL js_source_maps_delete(JSRuntime *rt, JSSourceMapRegistry *reg,
                              const char *filename)
{
    SMEntry **pe, *e;

    if (!reg)
        return FALSE;
    pe = sm_find(reg, filename, sm_hash(filename));
    e = *pe;
    if (!e)
        return FALSE;
    *pe = e->next;
    sm_entry_free(rt, e);
    reg->count--;
    return TRUE;
}

JS_BOOL js_source_maps_lookup(JSRuntime *rt, JSSourceMapRegistry *reg,
                              const char *filename, int line, int col,
                              const char **psource, int *pline, int *pcol)
{
    SMEntry *e;

    if (!reg || !filename || line <= 0 || col <= 0)
        return FALSE;
    e = *sm_find(reg, filename, sm_hash(filename));
    if (!e)
        return FALSE;
    if (e->state == SM_STATE_PENDING) {
        e->map = sm_decode(rt, e->json, e->json_len, e->base);
        /* XXX: an out of memory error makes the map invalid */
        e->state = e->map ? SM_STATE_DECODED : SM_STATE_INVALID;
        js_free_rt(rt, e->json);
        e->json = NULL;
        e->json_len = 0;
    }
    if (e->state != SM_STATE_DECODED)
        return FALSE;
    return sm_map_lookup(e->map, line - 1, col - 1, psource, pline, pcol);
}

/*******************************************************************/
/* sourceMappingURL comments and data: URLs */

static BOOL sm_is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

const char *js_source_map_find_url(const char *input, size_t input_len,
                                   size_t *plen)
{
    const char *end = input + input_len;
    const char *line_start, *line_end, *p, *url;

    /* skip the trailing blank lines */
    line_end = end;
    for(;;) {
        line_start = line_end;
        while (line_start > input && line_start[-1] != '\n')
            line_start--;
        p = line_start;
        while (p < line_end && sm_is_space(*p))
            p++;
        if (p < line_end)
            break;
        if (line_start == input)
            return NULL;
        line_end = line_start - 1; /* before the '\n' */
    }
    /* p = first non blank char of the last non blank line */
    if (line_end - p < 4 || p[0] != '/' || p[1] != '/' ||
        (p[2] != '#' && p[2] != '@') || !sm_is_space(p[3]))
        return NULL;
    p += 4;
    while (p < line_end && sm_is_space(*p))
        p++;
    if ((size_t)(line_end - p) < 17 || memcmp(p, "sourceMappingURL=", 17) != 0)
        return NULL;
    p += 17;
    url = p;
    while (p < line_end && !sm_is_space(*p) &&
           *p != '"' && *p != '\'' && *p != '`')
        p++;
    if (p == url)
        return NULL;
    /* only blanks are allowed after the URL */
    *plen = p - url;
    while (p < line_end) {
        if (!sm_is_space(*p))
            return NULL;
        p++;
    }
    return url;
}

JS_BOOL js_source_map_is_data_url(const char *url, size_t url_len)
{
    return url_len >= 5 &&
        (url[0] | 0x20) == 'd' && (url[1] | 0x20) == 'a' &&
        (url[2] | 0x20) == 't' && (url[3] | 0x20) == 'a' && url[4] == ':';
}

static BOOL sm_ascii_ieq(const char *a, size_t a_len, const char *b)
{
    size_t i, b_len = strlen(b);
    if (a_len != b_len)
        return FALSE;
    for(i = 0; i < a_len; i++) {
        int c = (uint8_t)a[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != (uint8_t)b[i])
            return FALSE;
    }
    return TRUE;
}

int js_source_map_decode_data_url(JSRuntime *rt, const char *url,
                                  size_t url_len, char **pbuf, size_t *plen)
{
    const char *end = url + url_len, *p, *comma, *param;
    BOOL is_base64 = FALSE;
    char *buf;
    size_t len;

    if (!js_source_map_is_data_url(url, url_len))
        return -1;
    p = url + 5;
    comma = memchr(p, ',', end - p);
    if (!comma)
        return -1;
    /* media type: "application/json" with optional parameters */
    param = memchr(p, ';', comma - p);
    if (!sm_ascii_ieq(p, (param ? param : comma) - p, "application/json"))
        return -1;
    while (param) {
        const char *next = memchr(param + 1, ';', comma - param - 1);
        const char *pend = next ? next : comma;
        if (sm_ascii_ieq(param + 1, pend - param - 1, "base64")) {
            if (next)
                return -1; /* ';base64' must be the last parameter */
            is_base64 = TRUE;
        }
        param = next;
    }
    p = comma + 1;
    buf = js_malloc_rt(rt, end - p + 1);
    if (!buf)
        return -1;
    len = 0;
    if (is_base64) {
        uint32_t acc = 0;
        int bits = 0, v;
        for(; p < end; p++) {
            if (*p == '=')
                break;
            if (*p == '%' && end - p >= 3 && sm_hex(p[1]) >= 0 &&
                sm_hex(p[2]) >= 0 && sm_hex(p[1]) * 16 + sm_hex(p[2]) == '=')
                break;
            v = sm_base64_value((uint8_t)*p);
            if (v < 0)
                goto fail;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                buf[len++] = (acc >> bits) & 0xff;
            }
        }
    } else {
        for(; p < end; p++) {
            if (*p == '%' && end - p >= 3 && sm_hex(p[1]) >= 0 &&
                sm_hex(p[2]) >= 0) {
                buf[len++] = sm_hex(p[1]) * 16 + sm_hex(p[2]);
                p += 2;
            } else {
                buf[len++] = *p;
            }
        }
    }
    buf[len] = '\0';
    *pbuf = buf;
    *plen = len;
    return 0;
 fail:
    js_free_rt(rt, buf);
    return -1;
}
