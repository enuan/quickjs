/*
 * QuickJS source map support (internal interface)
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
#ifndef QUICKJS_SOURCEMAP_H
#define QUICKJS_SOURCEMAP_H

#include <stddef.h>
#include "quickjs.h"

/* The registry is owned by the JSRuntime (created on first use, freed by
   JS_FreeRuntime). The engine never reads files: maps are given by the host
   (JS_SetSourceMap, the source map loader) or come from inline data: URLs. */
typedef struct JSSourceMapRegistry JSSourceMapRegistry;

void js_source_maps_free(JSRuntime *rt, JSSourceMapRegistry *reg);

/* store an undecoded copy of the map ('base' = URL/path the relative
   'sources' are resolved against, NULL means 'filename'). Return -1 if out
   of memory. */
int js_source_maps_set(JSRuntime *rt, JSSourceMapRegistry **preg,
                       const char *filename, const char *map, size_t map_len,
                       const char *base);
/* return TRUE if a map was registered for 'filename' */
JS_BOOL js_source_maps_delete(JSRuntime *rt, JSSourceMapRegistry *reg,
                              const char *filename);

/* map a generated position ('line' and 'col' are 1-based, 'col' in UTF-16
   code units). The map is decoded on first use; an invalid map never maps
   anything. Return TRUE and the 1-based original position if mapped. The
   returned source string belongs to the map. */
JS_BOOL js_source_maps_lookup(JSRuntime *rt, JSSourceMapRegistry *reg,
                              const char *filename, int line, int col,
                              const char **psource, int *pline, int *pcol);

/* return the URL of the trailing '//# sourceMappingURL=' comment of a script
   (not NUL terminated, *plen is its length) or NULL if there is none */
const char *js_source_map_find_url(const char *input, size_t input_len,
                                   size_t *plen);

/* return TRUE if 'url' is a data: URL */
JS_BOOL js_source_map_is_data_url(const char *url, size_t url_len);

/* decode a JSON data: URL (base64 or percent encoded) into a buffer
   allocated with js_malloc_rt(). Return -1 if it is not a valid JSON data:
   URL or out of memory. */
int js_source_map_decode_data_url(JSRuntime *rt, const char *url,
                                  size_t url_len, char **pbuf, size_t *plen);

#endif /* QUICKJS_SOURCEMAP_H */
