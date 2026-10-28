# Fast nonplanar development checks

Run `scripts/dev_planner.ps1` from a VS developer PowerShell (or pass
`-VisualStudioRoot`). It reuses one Ninja tree for the dependency-free geometry,
field and planner tests. `-BuildDirectory` and `-CMakePath` are optional overrides.
On the existing local tree the unchanged configure/build/test cycle took 1.49 s;
that is a warm no-change baseline, not a full application build benchmark.

The nonplanar review workflow runs these tests independently of the Windows
application build. Review-only clang Windows builds now use the existing
`build_win.bat --cache sccache` path. PCH is disabled for compiler-cache entries
and relative debug paths permit reuse. Release optimization, native tests and
portable installation remain enabled. NSIS setup is skipped when no installer
is being produced. Release/distribution jobs retain their previous build path.

The first compiler-cache run is cold and can still be slow. Inspect the emitted
cache statistics before claiming a speedup on subsequent runs. The official
[sccache GitHub Actions backend](https://github.com/mozilla/sccache/blob/main/docs/GHA.md)
documents the cache service used here. Focused tests do not replace exact-source
native, packaged slicing or GUI validation before updating the daily launcher.
