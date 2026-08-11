# graph-matching-core

A small, fast, header-only C++17 solver for stable and popular matchings on the
bipartite Hospital/Residents problem. It computes three matchings, verifies them
by default, and checks lower-quota feasibility.

## Features

- **Stable matching** (`-s`)
- **Maximum-cardinality popular matching** (`-p`)
- **Popular-among-maximum-cardinality matching** (`-m`)
- **Either side proposes** (`-A` = partition A proposes, default; `-B` = B proposes)
- **Verifier, on by default** (disable with `-n`):
  - lower-quota feasibility post-check for all three algorithms;
  - a stability (blocking-pair) check for `-s` and a popularity dual-certificate
    check for `-p`.
- **External verification** of a claimed matching file (`-e`, requires `-s`).
- **Upper quotas** honoured by the solver; **lower quotas** enforced as a
  post-check (a matching that leaves a vertex below its floor is reported as
  infeasible -- no alternate matching is suggested).

## Build

Requires `g++` with C++17 and `make` (Linux). The Python generator/profiler/tests
use [uv](https://docs.astral.sh/uv/) (install with `curl -LsSf https://astral.sh/uv/install.sh | sh`).

```
make
```

This produces `build/graph_matcher` (the solver) and `build/generator` (a random
instance generator).

## Usage

```
./build/graph_matcher [options]

  -A                Partition A proposes (default)
  -B                Partition B proposes
  -s                Stable matching
  -p                Max-cardinality popular matching
  -m                Popular among max-cardinality matchings
  -i <input_file>   Input graph (stdin if omitted)
  -o <output_file>  Output matching (stdout if omitted)
  -g <sig_file>     Write a signature (rank distribution) of the matching
  -n                Do NOT run the verifier (verification is on by default)
  -e <claimed_file> Verify a claimed matching file (requires -s or -p)
```

Exit codes: `0` = success; `1` = usage/parse/file error **or** verification
failed (unstable / no certificate / lower quota violated).

### Output format

One line per matched pair, keyed on partition A:

```
a_id,b_id,rank
```

where `rank` is the 1-based position of `b_id` in `a_id`'s preference list.

## Input format

Four `@`-delimited sections, each closed with `@End`; `#` starts a line comment.

```
@PartitionA
a1, a2, a3 ;
@End
@PartitionB
b1(2), b2(1,3), b3 ;          # (upper) or (lower,upper); default (0,1)
@End
@PreferenceListsA
a1 : b1, b2 ;                 # strict, comma-separated, ';'-terminated
a2 : b2, b3 ;
a3 : b1 ;
@End
@PreferenceListsB
b1 : a1, a3 ;
b2 : a1, a2 ;
b3 : a2 ;
@End
```

Quotas: `v` -> upper 1, lower 0; `v(u)` -> upper `u`; `v(l,u)` -> lower `l`, upper `u`.

## Generator

```
./build/generator <num_A> <num_B> <edge_prob> <max_quota> [lq_max] [output_file]
```

`lq_max` (optional): when `> 0`, B vertices receive a random lower quota in
`[0, min(lq_max, upper)]`, letting you produce instances that exercise the
lower-quota feasibility check. If the 5th argument is not an integer it is
treated as the output file, so `generator A B p q out.txt` keeps working.

## Python tooling (uv)

The generator/profiler/tests are driven with [uv](https://docs.astral.sh/uv/).
uv reads `pyproject.toml`, creates an isolated environment, and installs the
`pytest` dependency on first use -- no manual `venv`/`pip` steps.

## Profiler

```
uv run profile_solver.py            # default sweep
uv run profile_solver.py --help     # full option matrix
```

Times the solver across sizes, edge densities, proposing sides, algorithms,
verification on/off, signature on/off, and lower-quota instances, and writes a
CSV.

## Tests

```
make            # build the C++ binaries
uv run pytest test_solver.py -q
```

or simply:

```
make test       # builds, then runs `uv run pytest`
```
