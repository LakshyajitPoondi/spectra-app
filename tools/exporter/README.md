# Data pack exporter and golden generator

Both tools run against the Python reference, pinned to
`spektrafilm@6cd00c8d4f30b5b550f50f4bbd3753c9f2a48507` (branch `dev`). They refuse to run
against any other commit unless `--allow-unpinned` is passed.

Set up the environment once (Python 3.13):

```bash
git clone --branch dev https://github.com/andreavolpato/spektrafilm.git ref/spektrafilm
```

```bash
git -C ref/spektrafilm checkout 6cd00c8d4f30b5b550f50f4bbd3753c9f2a48507
```

```bash
python3 -m venv venv && venv/bin/pip install -r tools/exporter/requirements.txt
```

```bash
venv/bin/pip install --no-deps -e ref/spektrafilm
```

Export the data pack (`data/spektrafilm.sfpk` and `data/pack_manifest.json`):

```bash
venv/bin/python tools/exporter/export_pack.py
```

Regenerate the parity goldens (`tests/golden/<suite>/`):

```bash
venv/bin/python tools/parity/make_golden.py
```

To move to a newer upstream commit: change `PINNED_COMMIT` in both scripts, re-export,
regenerate the goldens, run the test suite, and add an entry to `data/CHANGELOG.txt`.
The pack and the goldens are committed, so building and testing the C++ code needs no Python.
