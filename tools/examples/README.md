# tools/examples/

`examples.py` keeps the example index (`examples/examples.yml`) and everything that follows it in step.

```bash
python3 tools/examples/examples.py check    # folders, README table, documentation snippets, ESP-IDF app matrix, catalogue
python3 tools/examples/examples.py write    # regenerate the table in examples/README.md
python3 tools/examples/examples.py json     # the index as the configurator catalogue carries it
python3 tools/examples/tests/run.py         # the tests (ctest: kb_examples_unit, kb_examples_index)
```

The rules for examples are in [`examples/README.md`](../../examples/README.md) and on the
[Examples](../../docs/examples.md)
page.
