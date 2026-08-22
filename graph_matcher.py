#!/usr/bin/env python3
"""Command line front end for the graph-matching-core shared library.

The C++ engine is loaded through ctypes, so the same entry points serve both
this CLI and any in-process caller (e.g. a FastAPI handler):

    from graph_matcher import solve, verify_matching, diff_matchings, statistics

    result = solve(graph_text, algorithm="stable")
    result.status    # 0 on success, 1 on parse/verification failure
    result.matching  # "a_id,b_id,rank" per line
"""

from __future__ import annotations

import argparse
import ctypes
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# Keep in sync with GmAlgorithm in src/api.cc.
ALGORITHMS = {"stable": 0, "popular": 1, "max-card": 2}

_LIB_NAMES = ("libgraphmatch.so", "libgraphmatch.dylib", "graphmatch.dll")


class _GmResult(ctypes.Structure):
    _fields_ = [
        ("status", ctypes.c_int),
        ("parsed", ctypes.c_int),
        ("matching", ctypes.c_char_p),
        ("signature", ctypes.c_char_p),
        ("out", ctypes.c_char_p),
        ("err", ctypes.c_char_p),
    ]


@dataclass(frozen=True)
class Result:
    """Outcome of a solve/verify call. All text fields are decoded UTF-8."""

    status: int
    parsed: bool = False
    matching: str = ""
    signature: str = ""
    out: str = ""
    err: str = ""

    @property
    def ok(self) -> bool:
        return self.status == 0


def _find_library() -> Path:
    override = os.environ.get("GRAPH_MATCHING_LIB")
    if override:
        return Path(override)
    root = Path(__file__).resolve().parent
    for name in _LIB_NAMES:
        candidate = root / "build" / name
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        f"shared library not found in {root / 'build'} (run `make`), "
        f"and GRAPH_MATCHING_LIB is unset"
    )


_lib = None


def _load():
    """Loads the shared library once, on first use."""
    global _lib
    if _lib is not None:
        return _lib
    lib = ctypes.CDLL(str(_find_library()))
    lib.gm_solve.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int]
    lib.gm_solve.restype = _GmResult
    lib.gm_verify.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.gm_verify.restype = _GmResult
    lib.gm_diff.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                            ctypes.c_int, ctypes.c_int]
    lib.gm_diff.restype = _GmResult
    lib.gm_stats.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.gm_stats.restype = _GmResult
    lib.gm_free.argtypes = [ctypes.POINTER(_GmResult)]
    lib.gm_free.restype = None
    _lib = lib
    return lib


def _consume(lib, raw: _GmResult) -> Result:
    """Copies a native result into Python objects and frees the native buffers."""
    try:
        return Result(
            status=raw.status,
            parsed=bool(raw.parsed),
            matching=(raw.matching or b"").decode("utf-8", "replace"),
            signature=(raw.signature or b"").decode("utf-8", "replace"),
            out=(raw.out or b"").decode("utf-8", "replace"),
            err=(raw.err or b"").decode("utf-8", "replace"),
        )
    finally:
        lib.gm_free(ctypes.byref(raw))


def _alg_code(algorithm: str) -> int:
    try:
        return ALGORITHMS[algorithm]
    except KeyError:
        raise ValueError(
            f"unknown algorithm {algorithm!r}; expected one of {sorted(ALGORITHMS)}"
        ) from None


def solve(graph_text: str, algorithm: str = "stable", a_proposing: bool = True,
          verify: bool = True, signature: bool = False) -> Result:
    """Computes a matching for `graph_text`.

    algorithm: "stable", "popular", or "max-card".
    a_proposing: True for partition A proposing, False for partition B.
    verify: run the lower-quota (and, for stable/popular, the checker) pass.
    signature: also produce the rank-distribution signature.
    """
    lib = _load()
    raw = lib.gm_solve(graph_text.encode("utf-8"), _alg_code(algorithm), int(a_proposing),
                       int(verify), int(signature))
    return _consume(lib, raw)


def verify_matching(graph_text: str, claimed_text: str, algorithm: str = "stable",
                    a_proposing: bool = True) -> Result:
    """Checks a claimed matching against `graph_text`.

    algorithm must be "stable" or "popular"; the checker output lands in
    `Result.out` and parse/validation errors in `Result.err`.
    """
    code = _alg_code(algorithm)
    lib = _load()
    raw = lib.gm_verify(graph_text.encode("utf-8"), claimed_text.encode("utf-8"), code,
                        int(a_proposing))
    return _consume(lib, raw)


def diff_matchings(graph_a: str, matching_a: str, graph_b: str, matching_b: str,
                   algorithm: str = "stable", a_proposing: bool = True) -> Result:
    """Compares two runs, each its own (graph, matching) pair.

    The two sides may be different instances -- that is the point, since a
    changed instance can still produce the same matching. The JSON report
    lands in `Result.matching`; parse and validation errors in `Result.err`.
    """
    code = _alg_code(algorithm)
    lib = _load()
    raw = lib.gm_diff(graph_a.encode("utf-8"), matching_a.encode("utf-8"),
                      graph_b.encode("utf-8"), matching_b.encode("utf-8"), code,
                      int(a_proposing))
    return _consume(lib, raw)


def statistics(graph_text: str, matching_text: str, a_proposing: bool = True) -> Result:
    """Statistics for one matching over one instance.

    Cardinality, capacity use, egalitarian cost, blocking pairs and per-side
    figures. The JSON report lands in `Result.matching`. Separate from solve()
    so a stored run can be measured again without recomputing the matching.
    """
    lib = _load()
    raw = lib.gm_stats(graph_text.encode("utf-8"), matching_text.encode("utf-8"),
                       int(a_proposing))
    return _consume(lib, raw)


class _Parser(argparse.ArgumentParser):
    """argparse parser that keeps the original getopt-era errors and exit code."""

    def error(self, message):
        missing = re.search(r"argument (-\w)[^:]*: expected one argument", message)
        if missing:
            self._fail(f"Option {missing.group(1)} requires an argument\n")
        unknown = re.search(r"unrecognized arguments: (\S+)", message)
        if unknown:
            self._fail(f"Unknown option: {unknown.group(1)}\n")
        self._fail(f"{message}\n")

    def _fail(self, text):
        sys.stderr.write(text)
        self.print_help(sys.stderr)
        raise SystemExit(1)


def build_parser() -> argparse.ArgumentParser:
    parser = _Parser(
        prog="graph_matcher",
        description="Stable and popular matchings on the bipartite Hospital/Residents problem.",
        epilog="Exit codes: 0 = success, 1 = usage/parse/file error or verification failed.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("-A", dest="a_proposing", action="store_const", const=True, default=True,
                        help="proposing partition is A (default)")
    parser.add_argument("-B", dest="a_proposing", action="store_const", const=False,
                        help="proposing partition is B")
    parser.add_argument("-s", "--stable", action="store_true", help="compute stable matching")
    parser.add_argument("-p", "--popular", action="store_true",
                        help="compute maximum cardinality popular matching")
    parser.add_argument("-m", "--max-card", action="store_true",
                        help="compute popular matching among max cardinality matchings")
    parser.add_argument("-i", "--input", metavar="FILE",
                        help="input graph file (reads stdin if omitted)")
    parser.add_argument("-o", "--output", metavar="FILE",
                        help="write the computed matching here (stdout if omitted)")
    parser.add_argument("-g", "--signature", metavar="FILE",
                        help="write the signature of the matching here")
    parser.add_argument("-n", "--no-verify", dest="verify", action="store_false", default=True,
                        help="do NOT run the verifier (verification runs by default)")
    parser.add_argument("-e", "--verify-claimed", metavar="FILE",
                        help="verify a claimed matching file (requires -s or -p)")
    return parser


def _selected_algorithm(args) -> str | None:
    # Priority matches the original CLI: -s beats -p beats -m.
    if args.stable:
        return "stable"
    if args.popular:
        return "popular"
    if args.max_card:
        return "max-card"
    return None


def _read(path: str | None, what: str) -> str | None:
    """Reads `path`, or stdin when path is None. Returns None after reporting an error."""
    if path is None:
        return sys.stdin.read()
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        sys.stderr.write(f"Error: Could not open {what} {path}\n")
        return None


def _write(path: str, text: str, what: str) -> bool:
    try:
        Path(path).write_text(text, encoding="utf-8")
        return True
    except OSError:
        sys.stderr.write(f"Error: Could not open {what} {path}\n")
        return False


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    algorithm = _selected_algorithm(args)

    if args.verify_claimed and algorithm not in ("stable", "popular"):
        sys.stderr.write(
            "Error: Please specify -s (stable) or -p (popular) flag for verification.\n")
        parser.print_help(sys.stderr)
        return 1
    if not args.verify_claimed and algorithm is None:
        sys.stderr.write("Error: Must specify algorithm flag (-s, -p, or -m).\n")
        parser.print_help(sys.stderr)
        return 1

    graph_text = _read(args.input, "input file")
    if graph_text is None:
        return 1

    if args.verify_claimed:
        claimed = _read(args.verify_claimed, "claimed matching file")
        if claimed is None:
            return 1
        result = verify_matching(graph_text, claimed, algorithm, args.a_proposing)
        sys.stdout.write(result.out)
        sys.stderr.write(result.err)
        return result.status

    result = solve(graph_text, algorithm, args.a_proposing, args.verify,
                   signature=args.signature is not None)
    sys.stdout.write(result.out)
    sys.stderr.write(result.err)
    if not result.parsed:
        return result.status

    # The matching is written even when infeasible; no alternative is suggested.
    if args.output:
        if not _write(args.output, result.matching, "output file"):
            return 1
    else:
        sys.stdout.write(result.matching)

    if args.signature and not _write(args.signature, result.signature, "signature file"):
        return 1

    return result.status


if __name__ == "__main__":
    sys.exit(main())
