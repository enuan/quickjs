/*
 * Source map support tests (enuan/quickjs fork)
 *
 * Copyright (c) 2026 Enuan
 *
 * MIT license (see quickjs-sourcemap.c)
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../quickjs.h"

static int failures;
static int checks;

#define CHECK(cond) do {                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
        }                                                               \
    } while (0)

static void check_map(JSRuntime *rt, int src_line, const char *filename,
                      int line, int col, const char *exp_source,
                      int exp_line, int exp_col)
{
    const char *source = NULL;
    int l = 0, c = 0;
    JS_BOOL ok = JS_MapSourcePosition(rt, filename, line, col, &source, &l, &c);

    checks++;
    if (!exp_source) {
        if (ok) {
            failures++;
            fprintf(stderr, "%s:%d: %s:%d:%d: expected no mapping, got %s:%d:%d\n",
                    __FILE__, src_line, filename, line, col, source, l, c);
        }
        return;
    }
    if (!ok || strcmp(source, exp_source) || l != exp_line || c != exp_col) {
        failures++;
        fprintf(stderr, "%s:%d: %s:%d:%d: expected %s:%d:%d, got ",
                __FILE__, src_line, filename, line, col,
                exp_source, exp_line, exp_col);
        if (ok)
            fprintf(stderr, "%s:%d:%d\n", source, l, c);
        else
            fprintf(stderr, "no mapping\n");
    }
}

#define MAP(rt, f, l, c, s, el, ec) check_map(rt, __LINE__, f, l, c, s, el, ec)

static void set_map(JSRuntime *rt, const char *filename, const char *json,
                    const char *map_url)
{
    CHECK(JS_SetSourceMap(rt, filename, json, strlen(json), map_url) == 0);
}

static void test_lookup(void)
{
    JSRuntime *rt = JS_NewRuntime();

    set_map(rt, "gen.js",
            "{\"version\":3,\"file\":\"gen.js\",\"sources\":[\"orig.ts\"],"
            "\"names\":[],\"mappings\":\"AAAA,UAEI,U;;IAGF\"}", NULL);
    MAP(rt, "gen.js", 1, 1, "orig.ts", 1, 1);
    MAP(rt, "gen.js", 1, 5, "orig.ts", 1, 1);
    MAP(rt, "gen.js", 1, 11, "orig.ts", 3, 5);
    MAP(rt, "gen.js", 1, 15, "orig.ts", 3, 5);
    MAP(rt, "gen.js", 1, 21, NULL, 0, 0); /* unmapped segment */
    MAP(rt, "gen.js", 1, 99, NULL, 0, 0);
    MAP(rt, "gen.js", 2, 1, NULL, 0, 0);  /* empty line */
    MAP(rt, "gen.js", 3, 1, NULL, 0, 0);  /* before the first segment */
    MAP(rt, "gen.js", 3, 5, "orig.ts", 6, 3);
    MAP(rt, "gen.js", 4, 1, NULL, 0, 0);  /* after the last line */
    MAP(rt, "gen.js", 0, 1, NULL, 0, 0);
    MAP(rt, "other.js", 1, 1, NULL, 0, 0);

    /* null source */
    set_map(rt, "null.js",
            "{\"version\":3,\"sources\":[\"a.ts\",null],\"mappings\":\"AAAA,KCCC\"}",
            NULL);
    MAP(rt, "null.js", 1, 1, "a.ts", 1, 1);
    MAP(rt, "null.js", 1, 6, NULL, 0, 0);

    /* unsorted segments: stable sort, the first of equal columns wins */
    set_map(rt, "unsorted.js",
            "{\"version\":3,\"sources\":[\"u.ts\"],\"mappings\":\"UACA,VADA,UAEA\"}",
            NULL);
    MAP(rt, "unsorted.js", 1, 1, "u.ts", 1, 1);
    MAP(rt, "unsorted.js", 1, 11, "u.ts", 2, 1);

    /* replace and delete */
    set_map(rt, "gen.js",
            "{\"version\":3,\"sources\":[\"new.ts\"],\"mappings\":\"AAAA\"}", NULL);
    MAP(rt, "gen.js", 1, 1, "new.ts", 1, 1);
    CHECK(JS_DeleteSourceMap(rt, "gen.js"));
    CHECK(!JS_DeleteSourceMap(rt, "gen.js"));
    MAP(rt, "gen.js", 1, 1, NULL, 0, 0);

    /* JSON escapes, unknown keys, whitespace, XSSI prefix */
    set_map(rt, "esc.js",
            ")]}'\n { \"x\" : [1, {\"y\": null}, true, -1.5e3],"
            " \"version\" : 3, \"sources\" : [ \"d\\u00e9j\\u00e0/\\ud83d\\ude00.ts\" ],"
            " \"sourcesContent\": [\"a\\\"b\"], \"mappings\" : \"AAAA\" } ", NULL);
    MAP(rt, "esc.js", 1, 1, "d\xc3\xa9j\xc3\xa0/\xf0\x9f\x98\x80.ts", 1, 1);

    JS_FreeRuntime(rt);
}

static void test_sources(void)
{
    JSRuntime *rt = JS_NewRuntime();

    /* resolved against the map URL */
    set_map(rt, "/p/dist/x.js",
            "{\"version\":3,\"sourceRoot\":\"\",\"sources\":[\"../src/x.ts\"],"
            "\"mappings\":\"AAAA\"}", "/p/dist/maps/x.js.map");
    MAP(rt, "/p/dist/x.js", 1, 1, "/p/dist/src/x.ts", 1, 1);
    /* default map URL = filename */
    set_map(rt, "/p/dist/y.js",
            "{\"version\":3,\"sources\":[\"../src/./y.ts\"],\"mappings\":\"AAAA\"}",
            NULL);
    MAP(rt, "/p/dist/y.js", 1, 1, "/p/src/y.ts", 1, 1);
    /* sourceRoot without trailing slash */
    set_map(rt, "/p/dist/z.js",
            "{\"version\":3,\"sourceRoot\":\"../lib\",\"sources\":[\"z.ts\"],"
            "\"mappings\":\"AAAA\"}", NULL);
    MAP(rt, "/p/dist/z.js", 1, 1, "/p/lib/z.ts", 1, 1);
    /* absolute sourceRoot */
    set_map(rt, "/p/dist/r.js",
            "{\"version\":3,\"sourceRoot\":\"/root/\",\"sources\":[\"r.ts\"],"
            "\"mappings\":\"AAAA\"}", NULL);
    MAP(rt, "/p/dist/r.js", 1, 1, "/root/r.ts", 1, 1);
    /* absolute sources ignore sourceRoot and the map URL */
    set_map(rt, "/p/dist/a.js",
            "{\"version\":3,\"sourceRoot\":\"/root\",\"sources\":[\"/abs/a.ts\","
            "\"webpack://pkg/./a.ts\"],\"mappings\":\"AAAA,CCAA\"}", NULL);
    MAP(rt, "/p/dist/a.js", 1, 1, "/abs/a.ts", 1, 1);
    MAP(rt, "/p/dist/a.js", 1, 2, "webpack://pkg/a.ts", 1, 1);
    /* URLs */
    set_map(rt, "file:///p/dist/u.js",
            "{\"version\":3,\"sources\":[\"../../../src/u.ts\"],\"mappings\":\"AAAA\"}",
            NULL);
    MAP(rt, "file:///p/dist/u.js", 1, 1, "file:///src/u.ts", 1, 1);
    /* relative filename */
    set_map(rt, "dist/v.js",
            "{\"version\":3,\"sources\":[\"../../v.ts\"],\"mappings\":\"AAAA\"}",
            NULL);
    MAP(rt, "dist/v.js", 1, 1, "../v.ts", 1, 1);
    set_map(rt, "<input>",
            "{\"version\":3,\"sources\":[\"w.ts\"],\"mappings\":\"AAAA\"}", NULL);
    MAP(rt, "<input>", 1, 1, "w.ts", 1, 1);

    JS_FreeRuntime(rt);
}

static void test_sections(void)
{
    JSRuntime *rt = JS_NewRuntime();

    set_map(rt, "idx.js",
            "{\"version\":3,\"sections\":["
            "{\"offset\":{\"line\":0,\"column\":0},\"map\":"
            "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA,GAAO\"}},"
            "{\"offset\":{\"line\":1,\"column\":5},\"url\":\"ignored.map\"},"
            "{\"offset\":{\"line\":1,\"column\":5},\"map\":"
            "{\"version\":3,\"sources\":[\"b.ts\"],\"mappings\":\"AAIA;EACC\"}}]}",
            "/m/idx.js.map");
    MAP(rt, "idx.js", 1, 1, "/m/a.ts", 1, 1);
    MAP(rt, "idx.js", 1, 4, "/m/a.ts", 1, 8);
    MAP(rt, "idx.js", 2, 1, NULL, 0, 0);
    MAP(rt, "idx.js", 2, 6, "/m/b.ts", 5, 1);
    MAP(rt, "idx.js", 2, 8, "/m/b.ts", 5, 1);
    MAP(rt, "idx.js", 3, 3, "/m/b.ts", 6, 2);

    /* nested index maps are invalid */
    set_map(rt, "nested.js",
            "{\"version\":3,\"sections\":[{\"offset\":{\"line\":0,\"column\":0},"
            "\"map\":{\"version\":3,\"sections\":[]}}]}", NULL);
    MAP(rt, "nested.js", 1, 1, NULL, 0, 0);

    JS_FreeRuntime(rt);
}

static void test_invalid(void)
{
    static const char *maps[] = {
        "",
        "null",
        "{\"version\":2,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA\"}",
        "{\"sources\":[\"a.ts\"],\"mappings\":\"AAAA\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"]}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AA\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA!\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"ACAA\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA,D\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA\"",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA\"} x",
        "{\"version\":3,\"sources\":[\"a.ts],\"mappings\":\"AAAA\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"gggggggggggg\"}",
        "{\"version\":3,\"sources\":[\"a.ts\"],\"mappings\":\"AAAA\","
        "\"x\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["
        "]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}",
    };
    JSRuntime *rt = JS_NewRuntime();
    size_t i;
    char name[32];

    for(i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        snprintf(name, sizeof(name), "bad%d.js", (int)i);
        set_map(rt, name, maps[i], NULL);
        MAP(rt, name, 1, 1, NULL, 0, 0);
        MAP(rt, name, 1, 1, NULL, 0, 0); /* decoded only once */
    }
    JS_FreeRuntime(rt);
}

static char *get_string_prop(JSContext *ctx, JSValueConst obj, const char *prop)
{
    JSValue val = JS_GetPropertyStr(ctx, obj, prop);
    const char *str;
    char *res = NULL;

    str = JS_ToCString(ctx, val);
    if (str) {
        res = strdup(str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, val);
    return res;
}

static int get_int_prop(JSContext *ctx, JSValueConst obj, const char *prop)
{
    JSValue val = JS_GetPropertyStr(ctx, obj, prop);
    int32_t v = -1;
    JS_ToInt32(ctx, &v, val);
    JS_FreeValue(ctx, val);
    return v;
}

/* evaluate 'code' and return the 'stack' property of the thrown error */
static char *eval_stack(JSContext *ctx, const char *code, const char *filename,
                        int flags)
{
    JSValue ret, exc;
    char *stack;

    ret = JS_Eval(ctx, code, strlen(code), filename, flags);
    if (JS_IsException(ret)) {
        exc = JS_GetException(ctx);
    } else if (JS_PromiseState(ctx, ret) == JS_PROMISE_REJECTED) {
        /* modules return a promise */
        exc = JS_PromiseResult(ctx, ret);
        JS_FreeValue(ctx, ret);
    } else {
        JS_FreeValue(ctx, ret);
        return NULL;
    }
    stack = get_string_prop(ctx, exc, "stack");
    JS_FreeValue(ctx, exc);
    return stack;
}

static void check_contains(int src_line, const char *str, const char *sub)
{
    checks++;
    if (!str || !strstr(str, sub)) {
        failures++;
        fprintf(stderr, "%s:%d: expected '%s' in:\n%s\n", __FILE__, src_line,
                sub, str ? str : "(null)");
    }
}

#define CONTAINS(str, sub) check_contains(__LINE__, str, sub)

static void test_backtrace(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    char *stack;
    const char *code =
        "function f() {\n"
        "  throw new Error(\"x\");\n"
        "}\n"
        "f();\n";

    stack = eval_stack(ctx, code, "gen.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "at f (gen.js:2:");
    CONTAINS(stack, "(gen.js:4:");
    free(stack);

    set_map(rt, "gen.js",
            "{\"version\":3,\"sources\":[\"orig.ts\"],"
            "\"mappings\":\"AAAA;AASA;AACA;AASE\"}", "/src/gen.js.map");
    stack = eval_stack(ctx, code, "gen.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "at f (/src/orig.ts:10:1)");
    CONTAINS(stack, "(/src/orig.ts:20:3)");
    free(stack);

    /* modules, and frames of other files are left untouched */
    stack = eval_stack(ctx, "import 'x'; throw new Error()", "other.js",
                       JS_EVAL_TYPE_MODULE);
    free(stack);
    stack = eval_stack(ctx, "throw new Error('m')", "gen.js",
                       JS_EVAL_TYPE_MODULE);
    CONTAINS(stack, "(/src/orig.ts:1:1)");
    free(stack);

    /* generated positions are kept when there is no mapping */
    stack = eval_stack(ctx, "\n\n\n\n\nthrow new Error('u')", "gen.js",
                       JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "(gen.js:6:");
    free(stack);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void test_syntax_error(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    const char *code = "var a = 1;\nvar b = ;\n";
    JSValue ret, exc;
    char *str;

    set_map(rt, "syn.js",
            "{\"version\":3,\"sources\":[\"syn.ts\"],\"mappings\":\"AAwCA;AAAA,QAAY\"}",
            NULL);
    ret = JS_Eval(ctx, code, strlen(code), "syn.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(ret));
    exc = JS_GetException(ctx);
    str = get_string_prop(ctx, exc, "fileName");
    CHECK(str && !strcmp(str, "syn.ts"));
    free(str);
    CHECK(get_int_prop(ctx, exc, "lineNumber") == 41);
    CHECK(get_int_prop(ctx, exc, "columnNumber") == 13);
    str = get_string_prop(ctx, exc, "stack");
    CONTAINS(str, "at syn.ts:41:13\n");
    free(str);
    JS_FreeValue(ctx, exc);

    /* no mapping: generated position */
    ret = JS_Eval(ctx, code, strlen(code), "nomap.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(ret));
    exc = JS_GetException(ctx);
    CHECK(get_int_prop(ctx, exc, "lineNumber") == 2);
    CHECK(get_int_prop(ctx, exc, "columnNumber") == 9);
    JS_FreeValue(ctx, exc);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static int loader_calls;
static char loader_url[256];

static void test_loader(JSContext *ctx, const char *filename, const char *url,
                        void *opaque)
{
    const char *json =
        "{\"version\":3,\"sources\":[\"loaded.ts\"],\"mappings\":\"AAAA;AAKA\"}";
    loader_calls++;
    snprintf(loader_url, sizeof(loader_url), "%s", url);
    CHECK(opaque == (void *)&loader_calls);
    JS_SetSourceMap(JS_GetRuntime(ctx), filename, json, strlen(json),
                    "/maps/x.map");
}

static void test_detection(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    char *stack;
    const char *inline_b64 =
        "function g() { throw new Error('y') }\n"
        "g();\n"
        "//# sourceMappingURL=data:application/json;charset=utf-8;base64,"
        "eyJ2ZXJzaW9uIjogMywgInNvdXJjZXMiOiBbImlubC50cyJdLCAibWFwcGluZ3MiOiAiQUFFQTtBQUtBIn0=\n\n";
    const char *inline_pct =
        "function g() { throw new Error('y') }\n"
        "g();\n"
        "  //@ sourceMappingURL=data:application/json,%7B%22version%22:3,"
        "%22sources%22:[%22pct.ts%22],%22mappings%22:%22AAEA;AAKA%22%7D  ";
    const char *external =
        "function g() { throw new Error('y') }\n"
        "g();\n"
        "//# sourceMappingURL=x.js.map";
    const char *not_last =
        "//# sourceMappingURL=data:application/json,%7B%22version%22:3,"
        "%22sources%22:[%22pct.ts%22],%22mappings%22:%22AAEA;AAKA%22%7D\n"
        "throw new Error('z')\n";

    CHECK(JS_GetSourceMapFlags(rt) == 0);
    /* disabled by default */
    stack = eval_stack(ctx, inline_b64, "inl.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "at g (inl.js:1:");
    free(stack);

    JS_SetSourceMapFlags(rt, JS_SOURCE_MAP_INLINE);
    CHECK(JS_GetSourceMapFlags(rt) == JS_SOURCE_MAP_INLINE);
    stack = eval_stack(ctx, inline_b64, "inl.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "at g (inl.ts:3:1)");
    CONTAINS(stack, "(inl.ts:8:1)");
    free(stack);

    stack = eval_stack(ctx, inline_pct, "pct.js", JS_EVAL_TYPE_MODULE);
    CONTAINS(stack, "at g (pct.ts:3:1)");
    free(stack);

    /* the comment must be the last non blank line */
    stack = eval_stack(ctx, not_last, "last.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "(last.js:2:");
    free(stack);

    /* eval() code is not considered */
    stack = eval_stack(ctx, "eval(\"throw new Error('e')\\n//# sourceMappingURL="
                       "data:application/json,%7B%22version%22:3,%22sources%22:"
                       "[%22ev.ts%22],%22mappings%22:%22AAAA%22%7D\")",
                       "ev.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(stack && !strstr(stack, "ev.ts"));
    free(stack);

    /* external URLs need a loader */
    stack = eval_stack(ctx, external, "ext.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "(ext.js:2:");
    free(stack);
    JS_SetSourceMapLoaderFunc(rt, test_loader, &loader_calls);
    stack = eval_stack(ctx, external, "ext.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(loader_calls == 1);
    CHECK(!strcmp(loader_url, "x.js.map"));
    CONTAINS(stack, "(/maps/loaded.ts:6:1)");
    free(stack);
    /* the loader is not called for data: URLs */
    stack = eval_stack(ctx, inline_b64, "inl.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(loader_calls == 1);
    free(stack);
    /* nor when the flag is cleared */
    JS_SetSourceMapFlags(rt, 0);
    JS_DeleteSourceMap(rt, "inl.js");
    stack = eval_stack(ctx, inline_b64, "inl.js", JS_EVAL_TYPE_GLOBAL);
    CONTAINS(stack, "(inl.js:2:");
    CHECK(loader_calls == 1);
    free(stack);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* columns are counted in UTF-16 code units */
static void test_utf16_columns(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    char *s1, *s2;

    s1 = eval_stack(ctx, "var s = 'ab'; null.x", "c.js", JS_EVAL_TYPE_GLOBAL);
    s2 = eval_stack(ctx, "var s = '\xf0\x9f\x98\x80'; null.x", "c.js",
                    JS_EVAL_TYPE_GLOBAL);
    CHECK(s1 && s2 && !strcmp(s1, s2));
    free(s1);
    free(s2);
    s1 = eval_stack(ctx, "var s = 'a'; null.x", "c.js", JS_EVAL_TYPE_GLOBAL);
    s2 = eval_stack(ctx, "var s = '\xc3\xa9'; null.x", "c.js",
                    JS_EVAL_TYPE_GLOBAL);
    CHECK(s1 && s2 && !strcmp(s1, s2));
    free(s1);
    free(s2);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    test_lookup();
    test_sources();
    test_sections();
    test_invalid();
    test_backtrace();
    test_syntax_error();
    test_detection();
    test_utf16_columns();
    if (failures) {
        fprintf(stderr, "test_source_map: %d/%d checks failed\n",
                failures, checks);
        return 1;
    }
    printf("test_source_map: %d checks passed\n", checks);
    return 0;
}
