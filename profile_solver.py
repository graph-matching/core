#!/usr/bin/env python3
"""Comprehensive profiler for the graph-matching-core solver.

Sweeps the full parameter matrix:
  * instance size (num_A x num_B) and several edge densities
  * proposing side (A / B)
  * algorithm (-s / -p / -m)
  * verification: default-on, disabled (-n), and external claimed (-e)
  * signature output on / off
  * lower-quota instances (via the generator's lq_max mode)

Reports min/max/avg wall-clock per configuration and writes a CSV.
"""
import time
import subprocess
import os
import tempfile
import sys
import argparse
import csv
from typing import List, Tuple

BINARY_PATH = "./build/graph_matcher"
GENERATOR_PATH = "./build/generator"

DEFAULT_SIZES = [
    (100, 50, 0.5, 5),
    (100, 50, 0.1, 5),
    (500, 200, 0.1, 5),
    (500, 200, 0.02, 5),
    (1000, 500, 0.05, 5),
    (1000, 500, 0.01, 5),
    (5000, 2000, 0.01, 5),
    (5000, 2000, 0.002, 5),
]

def parse_sizes(sizes_str: str) -> List[Tuple[int, int, float, int]]:
    result = []
    try:
        for item in sizes_str.split(";"):
            if not item.strip():
                continue
            parts = item.split(",")
            if len(parts) != 4:
                raise ValueError("Each size configuration must have 4 values (num_a,num_b,edge_prob,max_quota)")
            num_a = int(parts[0])
            num_b = int(parts[1])
            edge_prob = float(parts[2])
            max_quota = int(parts[3])
            result.append((num_a, num_b, edge_prob, max_quota))
    except Exception as e:
        raise argparse.ArgumentTypeError(f"Invalid sizes format: {e}. Expected format: '100,50,0.2,5;500,200,0.1,5'")
    return result

def generate_graph(num_a: int, num_b: int, edge_prob: float, max_quota: int, lq_max: int) -> str:
    """Generates a random graph using build/generator and returns its file path."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
        graph_path = f.name
    args = [GENERATOR_PATH, str(num_a), str(num_b), str(edge_prob), str(max_quota)]
    if lq_max > 0:
        args.append(str(lq_max))
    args.append(graph_path)
    try:
        subprocess.run(args, check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        raise RuntimeError(f"Graph generator failed: {e.stderr.decode().strip()}") from e
    return graph_path

def measure_run(cmd: List[str], iterations: int) -> Tuple[float, float, float]:
    """Runs the command multiple times and returns (min_ms, max_ms, avg_ms)."""
    times = []
    for _ in range(iterations):
        start = time.perf_counter()
        res = subprocess.run(cmd, capture_output=True)
        end = time.perf_counter()
        # Exit 1 (verifier rejection / lower-quota violation) is still a valid
        # timed run; only an unexpected code (crash) aborts.
        if res.returncode not in (0, 1):
            cmd_str = " ".join(cmd)
            err_msg = res.stderr.decode().strip() if res.stderr else "Unknown error"
            raise RuntimeError(f"Command '{cmd_str}' failed with exit code {res.returncode}. Stderr: {err_msg}")
        times.append((end - start) * 1000.0)
    return min(times), max(times), sum(times) / len(times)

def profile_combination(
    graph_path: str,
    alg: str,
    proposer: str,
    verify_mode: str,
    sig_mode: str,
    iterations: int
) -> Tuple[float, float, float]:
    """Measures a specific configuration on the given graph."""
    temp_files = []
    try:
        alg_flag = f"-{alg}"
        proposer_flag = f"-{proposer}"

        # Signature (-g) is ignored by the solver in claimed (-e) mode.
        sig_args = []
        if sig_mode == "sig" and verify_mode == "claimed":
            raise ValueError("Signature generation (-g) is ignored by the solver in claimed verification (-e) mode.")
        if sig_mode == "sig":
            with tempfile.NamedTemporaryFile(delete=False, suffix=".sig") as f:
                sig_path = f.name
            temp_files.append(sig_path)
            sig_args = ["-g", sig_path]

        if verify_mode == "claimed":
            if alg == "m":
                raise ValueError("Algorithm -m does not support claimed matching verification.")

            with tempfile.NamedTemporaryFile(delete=False, suffix=".out") as f:
                matching_path = f.name
            temp_files.append(matching_path)

            # Produce the matching to verify against (not timed).
            subprocess.run(
                [BINARY_PATH, alg_flag, proposer_flag, "-n", "-i", graph_path, "-o", matching_path],
                check=True,
                capture_output=True
            )

            cmd = [BINARY_PATH, alg_flag, proposer_flag, "-i", graph_path, "-e", matching_path]

        elif verify_mode == "internal":
            cmd = [BINARY_PATH, alg_flag, proposer_flag, "-i", graph_path, "-o", "/dev/null"] + sig_args
        else:
            cmd = [BINARY_PATH, alg_flag, proposer_flag, "-n", "-i", graph_path, "-o", "/dev/null"] + sig_args

        return measure_run(cmd, iterations)

    finally:
        for p in temp_files:
            if os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass

def main():
    parser = argparse.ArgumentParser(
        description="Comprehensive profiling tool for the graph-matching-core solver."
    )
    parser.add_argument("--iterations", "-n", type=int, default=3,
                        help="Number of runs to average per configuration (default: 3).")
    parser.add_argument("--sizes", type=parse_sizes, default=DEFAULT_SIZES,
                        help="Semicolon-separated num_a,num_b,edge_prob,max_quota configs "
                             "(e.g. '100,50,0.2,5;500,200,0.1,5').")
    parser.add_argument("--csv", "-c", type=str, default="benchmark_results.csv",
                        help="Path to save results as a CSV file (default: benchmark_results.csv).")
    parser.add_argument("--algorithms", "-a", type=str, default="s,p,m",
                        help="Comma-separated algorithms: s, p, m, all (default: s,p,m).")
    parser.add_argument("--proposers", type=str, default="A,B",
                        help="Comma-separated proposing partitions: A, B, all (default: A,B).")
    parser.add_argument("--verify", type=str, default="internal,none",
                        help="Comma-separated verify modes to profile: none (-n), internal "
                             "(default-on), claimed (-e), all (default: internal,none).")
    parser.add_argument("--include-sig", action="store_true",
                        help="Also profile signature file generation (-g) overhead.")
    parser.add_argument("--lq-max", type=int, default=0,
                        help="If > 0, generate instances with random lower quotas up to this "
                             "value (exercises the lower-quota feasibility post-check). Default 0.")

    args = parser.parse_args()

    if args.iterations < 1:
        print("Error: --iterations must be at least 1.", file=sys.stderr)
        sys.exit(1)
    if args.lq_max < 0:
        print("Error: --lq-max must be non-negative.", file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(BINARY_PATH) or not os.path.exists(GENERATOR_PATH):
        print(f"Error: Binaries '{BINARY_PATH}' or '{GENERATOR_PATH}' not found.", file=sys.stderr)
        print("Please compile the project first using `make`.", file=sys.stderr)
        sys.exit(1)

    algs = [x.strip() for x in args.algorithms.split(",")]
    if "all" in algs:
        algs = ["s", "p", "m"]
    for alg in algs:
        if alg not in ["s", "p", "m"]:
            print(f"Error: Invalid algorithm '{alg}'. Choice must be s, p, or m.", file=sys.stderr)
            sys.exit(1)

    props = [x.strip() for x in args.proposers.split(",")]
    if "all" in props:
        props = ["A", "B"]
    for prop in props:
        if prop not in ["A", "B"]:
            print(f"Error: Invalid proposer '{prop}'. Choice must be A or B.", file=sys.stderr)
            sys.exit(1)

    verify_modes = [x.strip() for x in args.verify.split(",")]
    if "all" in verify_modes:
        verify_modes = ["none", "internal", "claimed"]
    for vm in verify_modes:
        if vm not in ["none", "internal", "claimed"]:
            print(f"Error: Invalid verify mode '{vm}'. Choice must be none, internal, or claimed.", file=sys.stderr)
            sys.exit(1)

    sig_modes = ["none"]
    if args.include_sig:
        sig_modes += ["sig"]

    header_fmt = "{:<20} | {:<4} | {:<4} | {:<8} | {:<5} | {:<10} | {:<10} | {:<10}"
    row_fmt = "{:<20} | {:<4} | {:<4} | {:<8} | {:<5} | {:<10.2f} | {:<10.2f} | {:<10.2f}"
    err_row_fmt = "{:<20} | {:<4} | {:<4} | {:<8} | {:<5} | {:<10} | {:<10} | {:<10}"

    print("\nStarting comprehensive profiling run...")
    print(f"(lower-quota mode: lq_max={args.lq_max})")
    print("=" * 95)
    print(header_fmt.format("Size (A x B)", "Alg", "Prop", "Verify", "Sig", "Min (ms)", "Max (ms)", "Avg (ms)"))
    print("-" * 95)

    csv_data = []

    for num_a, num_b, edge_prob, max_quota in args.sizes:
        size_str = f"{num_a}x{num_b} (p={edge_prob})"
        try:
            graph_path = generate_graph(num_a, num_b, edge_prob, max_quota, args.lq_max)
        except Exception as e:
            print(f"Failed to generate graph of size {num_a}x{num_b}: {e}")
            continue

        try:
            for alg in algs:
                for prop in props:
                    for verify in verify_modes:
                        if verify == "claimed" and alg == "m":
                            continue
                        for sig in sig_modes:
                            if verify == "claimed" and sig == "sig":
                                continue
                            try:
                                min_ms, max_ms, avg_ms = profile_combination(
                                    graph_path, alg, prop, verify, sig, args.iterations
                                )
                                print(row_fmt.format(size_str, alg, prop, verify, sig, min_ms, max_ms, avg_ms))
                                csv_data.append({
                                    "num_A": num_a, "num_B": num_b, "edge_prob": edge_prob,
                                    "max_quota": max_quota, "lq_max": args.lq_max,
                                    "algorithm": alg, "proposer": prop,
                                    "verification": verify, "signature": sig,
                                    "min_ms": round(min_ms, 3), "max_ms": round(max_ms, 3),
                                    "avg_ms": round(avg_ms, 3)
                                })
                            except Exception as e:
                                print(err_row_fmt.format(size_str, alg, prop, verify, sig, "ERROR", "ERROR", "ERROR"))
                                print(f"  [Error details]: {e}")
        finally:
            if os.path.exists(graph_path):
                try:
                    os.remove(graph_path)
                except OSError:
                    pass

    print("=" * 95)

    if args.csv and csv_data:
        try:
            with open(args.csv, mode="w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=[
                    "num_A", "num_B", "edge_prob", "max_quota", "lq_max",
                    "algorithm", "proposer", "verification", "signature",
                    "min_ms", "max_ms", "avg_ms"
                ])
                writer.writeheader()
                writer.writerows(csv_data)
            print(f"Results successfully written to: {args.csv}\n")
        except Exception as e:
            print(f"Failed to write CSV file '{args.csv}': {e}", file=sys.stderr)

if __name__ == "__main__":
    main()
