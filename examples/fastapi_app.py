#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = ["fastapi>=0.110", "uvicorn>=0.29", "pydantic>=2"]
# ///
"""FastAPI wrapper over the graph-matching-core shared library.

    make                            # build build/libgraphmatch.so first
    uv run examples/fastapi_app.py  # serves on http://127.0.0.1:8000

    curl -s -X POST localhost:8000/solve \
      --json "{\"graph\": \"$(cat examples/simple.txt)\", \"algorithm\": \"stable\"}"

The solver holds no state between calls and never exits the process on a
malformed graph, so a bad request is a 400, not a dead worker.
"""

import sys
from pathlib import Path
from typing import List, Optional

from fastapi import FastAPI, HTTPException
from fastapi.concurrency import run_in_threadpool
from pydantic import BaseModel

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import graph_matcher  # noqa: E402

app = FastAPI(title="graph-matching-core")


class SolveRequest(BaseModel):
    graph: str
    algorithm: str = "stable"
    a_proposing: bool = True
    verify: bool = True
    signature: bool = False


class Pair(BaseModel):
    a: str
    b: str
    rank: int


class SolveResponse(BaseModel):
    matching: List[Pair]
    verified: bool
    signature: Optional[str] = None
    report: str


class VerifyRequest(BaseModel):
    graph: str
    matching: str
    algorithm: str = "stable"
    a_proposing: bool = True


class VerifyResponse(BaseModel):
    passed: bool
    report: str


def _parse_pairs(text: str) -> List[Pair]:
    pairs = []
    for line in text.splitlines():
        if line.strip():
            a, b, rank = line.split(",")
            pairs.append(Pair(a=a, b=b, rank=int(rank)))
    return pairs


@app.post("/solve", response_model=SolveResponse)
async def solve(req: SolveRequest) -> SolveResponse:
    if req.algorithm not in graph_matcher.ALGORITHMS:
        raise HTTPException(422, f"algorithm must be one of {sorted(graph_matcher.ALGORITHMS)}")

    # The library serialises calls internally, so keep it off the event loop.
    result = await run_in_threadpool(
        graph_matcher.solve, req.graph, req.algorithm, req.a_proposing, req.verify, req.signature
    )
    if not result.parsed:
        raise HTTPException(400, result.err.strip() or "failed to parse graph")

    # status 1 with a parsed graph means unstable / infeasible, not a bad
    # request: return the matching and let the caller see the report.
    return SolveResponse(
        matching=_parse_pairs(result.matching),
        verified=result.ok,
        signature=result.signature or None,
        report=result.err,
    )


@app.post("/verify", response_model=VerifyResponse)
async def verify(req: VerifyRequest) -> VerifyResponse:
    if req.algorithm not in ("stable", "popular"):
        raise HTTPException(422, "algorithm must be 'stable' or 'popular' to verify")

    result = await run_in_threadpool(
        graph_matcher.verify_matching, req.graph, req.matching, req.algorithm, req.a_proposing
    )
    if not result.parsed:
        raise HTTPException(400, result.err.strip() or "failed to parse graph")

    return VerifyResponse(passed=result.ok, report=result.out + result.err)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=8000)
