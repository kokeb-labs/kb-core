# tools/release/

`port_sdk.py` builds and checks the port SDK asset of a release ([Port SDK](../../docs/port-sdk.md)).

```bash
python3 tools/release/port_sdk.py build --out dist --version 0.1.0-rc.2     # from the checkout (any version text)
python3 tools/release/port_sdk.py build --out dist --tag v0.1.0-rc.2        # the release job: the tag must match cmake/kb_version.cmake
python3 tools/release/port_sdk.py verify dist/kb-port-sdk-0.1.0-rc.2.tar.gz --manifest dist/kb-port-sdk-0.1.0-rc.2.manifest.json
python3 tools/release/tests/run.py                                           # the tests (ctest: kb_port_sdk)
```

The test builds a minimal port (`tests/port_sdk/minimal_port/`) from the extracted archive with CMake, which needs a
C++20 compiler.
