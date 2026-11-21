# tools/layout/

The repository layout: `paths.yml` is the one file that says where things live, what a consumer that vendors kb-core
copies, and what may sit at the repository root. `layout.py` reads it for scripts and runs two checks.

```bash
python3 tools/layout/layout.py get lib          # components/lib (--abs: absolute)
python3 tools/layout/layout.py list             # every entry
python3 tools/layout/layout.py vendor           # the files a vendoring consumer copies, resolved
python3 tools/layout/layout.py root             # fail on an unexpected top-level entry
python3 tools/layout/layout.py stale [--fix]    # fail on (or rewrite) a reference to a retired location
python3 tools/layout/tests/run.py               # the tests (ctest: kb_layout_unit, kb_layout_root, kb_layout_stale)
```

CMake reads the same file through `cmake/kb_layout.cmake`. How it works and how to regroup:
[Repository layout](../../docs/layout.md).
