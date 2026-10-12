# enuan/quickjs

Fork of [bellard/quickjs](https://github.com/bellard/quickjs) maintained for
[quicktape](https://github.com/enuan/quicktape-py), which vendors a copy of
the `quicktape` branch.

## Branches

- `master`: mirror of `bellard/quickjs` `master` (fast-forward only, no local
  commits).
- `quicktape`: a short stack of commits on top of an upstream revision. It is
  rebased (not merged) onto upstream, so `git log upstream/master..quicktape`
  always lists exactly the local changes.

## Local changes

1. **Source maps** (`quickjs-sourcemap.c`, `quickjs-sourcemap.h`, with a few
   hooks in `quickjs.c`):
   - `JS_SetSourceMap()`, `JS_DeleteSourceMap()`, `JS_MapSourcePosition()`;
   - `JS_SetSourceMapFlags(rt, JS_SOURCE_MAP_INLINE)` and
     `JS_SetSourceMapLoaderFunc()` for `//# sourceMappingURL=` comments;
   - `js_std_source_map_loader()` in `quickjs-libc`, `qjs --source-map`.

   Backtrace positions and the `fileName`/`lineNumber`/`columnNumber` of
   syntax errors are remapped. See "Source maps" in `doc/quickjs.texi`.
2. **UTF-16 columns**: column numbers count UTF-16 code units, like JS
   strings, editors and source maps. Upstream counts code points.

Tests: `tests/test_source_map.c`, run by `make test`.

## Updating from upstream

```sh
git fetch upstream
git checkout master && git merge --ff-only upstream/master && git push origin master
git checkout quicktape && git rebase master   # or a specific upstream revision
make test
git push --force-with-lease origin quicktape
```

Keep the hooks in `quickjs.c` minimal so that rebases stay trivial; put the
logic in separate files.
