# Examples

Build first: `make` (produces `build/libgraphmatch.so`).

| File | What it shows |
| --- | --- |
| `simple.txt` | 3x3 one-to-one instance, no quotas |
| `quotas.txt` | Hospital/Residents: upper quotas, and a lower quota on `h2` |
| `infeasible.txt` | Lower quota that no stable matching can meet -> exit 1 |
| `simple_claimed_unstable.txt` | A valid but unstable matching for `simple.txt`, for `-e` |
| `malformed.txt` | A graph that fails to parse |
| `fastapi_app.py` | The same engine behind a FastAPI service |

## CLI

```bash
uv run graph_matcher.py -s -i examples/simple.txt
```

```
Lower quotas satisfied          # stderr
Matching is Stable              # stderr
a1,b1,1                         # stdout
a2,b2,1
```

Popular-among-max-cardinality, reading from stdin:

```bash
cat examples/quotas.txt | uv run graph_matcher.py -m
```

Lower quota that cannot be met -- the matching is still printed, exit code is 1:

```bash
uv run graph_matcher.py -s -i examples/infeasible.txt
```

```
Lower quota violated: b2 matched 0, floor 1
Matching is infeasible (lower quota not met)
a1,b1,1
```

Check a claimed matching (`-e`) -- the blocking pair is reported, exit code 1:

```bash
uv run graph_matcher.py -s -i examples/simple.txt -e examples/simple_claimed_unstable.txt
```

```
Running Stable Marriage verification checker...
Matching is not stable
Edge a1 -- b1 is a blocking pair!
```

A malformed graph is a normal error, exit code 1:

```bash
uv run graph_matcher.py -s -i examples/malformed.txt
```

```
Line 8: Duplicate vertex: b1 (already declared in the other partition)
Error: Failed to parse bipartite graph.
```

Write the matching and its signature to files instead of stdout:

```bash
uv run graph_matcher.py -s -i examples/quotas.txt -o matching.txt -g signature.txt
```

## FastAPI

The script declares its own dependencies inline (PEP 723), so uv installs
fastapi/uvicorn on first run:

```bash
uv run examples/fastapi_app.py
```

Then, in another shell:

```bash
curl -s -X POST localhost:8000/solve -H 'Content-Type: application/json' \
  -d "$(python3 -c 'import json,pathlib; print(json.dumps({"graph": pathlib.Path("examples/simple.txt").read_text(), "algorithm": "stable"}))')"
```

```json
{"matching":[{"a":"a1","b":"b1","rank":1},{"a":"a2","b":"b2","rank":1}],
 "verified":true,"signature":null,
 "report":"Lower quotas satisfied\nMatching is Stable\n"}
```

Posting `malformed.txt` returns `400` with the parser diagnostics and leaves the
worker running -- the engine reports parse failures instead of exiting the
process. `POST /verify` takes a `graph` plus a claimed `matching` and returns
`{"passed": false, "report": "..."}` for the unstable claim above.
