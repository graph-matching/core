import subprocess
import os
import tempfile
import pytest

BINARY_PATH = "./build/graph_matcher"
GENERATOR_PATH = "./build/generator"

def generate_graph(num_a, num_b, edge_prob, max_quota, lq_max=0):
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
        graph_path = f.name
    args = [GENERATOR_PATH, str(num_a), str(num_b), str(edge_prob), str(max_quota)]
    if lq_max > 0:
        # 5th positional is lq_max only when it is an integer; the path follows.
        args.append(str(lq_max))
    args.append(graph_path)
    subprocess.run(args, check=True)
    return graph_path

@pytest.mark.parametrize("alg_flag", ["-s", "-p", "-m"])
def test_matching_verifier_runs_by_default_A_proposing(alg_flag):
    graph_path = generate_graph(10, 8, 0.5, 3)
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
            out_path = f.name

        cmd = [BINARY_PATH, alg_flag, "-A", "-i", graph_path, "-o", out_path]
        res = subprocess.run(cmd, capture_output=True, text=True)

        assert res.returncode == 0
        stderr = res.stderr

        assert "Lower quotas satisfied" in stderr
        if alg_flag == "-s":
            assert "Matching is Stable" in stderr
        elif alg_flag == "-p":
            assert "Passed. Certificate issued!" in stderr
        else:
            # -m has no stability/popularity verifier, only the lower-quota check.
            assert "Certificate issued" not in stderr
            assert "Matching is Stable" not in stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        if os.path.exists(out_path):
            os.remove(out_path)

def test_stable_matching_checker_B_proposing():
    graph_path = generate_graph(10, 8, 0.5, 3)
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
            out_path = f.name

        cmd = [BINARY_PATH, "-s", "-B", "-i", graph_path, "-o", out_path]
        res = subprocess.run(cmd, capture_output=True, text=True)

        assert res.returncode == 0
        assert "Matching is Stable" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        if os.path.exists(out_path):
            os.remove(out_path)

def test_no_verify_flag_disables_verification():
    graph_path = generate_graph(10, 8, 0.5, 3)
    try:
        cmd = [BINARY_PATH, "-s", "-n", "-i", graph_path]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0
        assert "Matching is Stable" not in res.stderr
        assert "Lower quota" not in res.stderr
        assert any(line.strip() for line in res.stdout.splitlines())
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

@pytest.mark.parametrize("alg_flag", ["-s"])
def test_claimed_matching_verification(alg_flag):
    graph_path = generate_graph(10, 8, 0.5, 3)
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
            out_path = f.name

        subprocess.run([BINARY_PATH, alg_flag, "-i", graph_path, "-o", out_path], check=True)

        cmd = [BINARY_PATH, alg_flag, "-i", graph_path, "-e", out_path]
        res = subprocess.run(cmd, capture_output=True, text=True)

        assert res.returncode == 0
        stdout = res.stdout
        assert "Running Stable Marriage verification checker" in stdout
        assert "Matching is Stable" in stdout
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        if os.path.exists(out_path):
            os.remove(out_path)

LOWER_QUOTA_SATISFIED_GRAPH = """@PartitionA
a1;
@End
@PartitionB
b1(1,1);
@End
@PreferenceListsA
a1: b1;
@End
@PreferenceListsB
b1: a1;
@End"""

LOWER_QUOTA_VIOLATED_GRAPH = """@PartitionA
a1;
@End
@PartitionB
b1, b2(1,1);
@End
@PreferenceListsA
a1: b1, b2;
@End
@PreferenceListsB
b1: a1;
b2: a1;
@End"""

@pytest.mark.parametrize("alg_flag", ["-s", "-p", "-m"])
def test_lower_quota_satisfied(alg_flag):
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(LOWER_QUOTA_SATISFIED_GRAPH)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, alg_flag, "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 0
        assert "Lower quotas satisfied" in res.stderr
        assert "a1,b1,1" in res.stdout
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

@pytest.mark.parametrize("alg_flag", ["-s", "-p", "-m"])
def test_lower_quota_violation_rejected(alg_flag):
    # a1 (quota 1) can fill only one B vertex, so b2's floor of 1 can never be
    # met: infeasible, yet the matching must still be printed with exit 1.
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(LOWER_QUOTA_VIOLATED_GRAPH)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, alg_flag, "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert "Lower quota violated: b2 matched 0, floor 1" in res.stderr
        assert "infeasible" in res.stderr
        assert "a1,b1,1" in res.stdout
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_lower_quota_violation_suppressed_by_no_verify():
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(LOWER_QUOTA_VIOLATED_GRAPH)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-n", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 0
        assert "Lower quota" not in res.stderr
        assert "a1,b1,1" in res.stdout
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_generator_lower_quota_mode_runs():
    # lq_max > 0 emits (l,u) quotas; the lower-quota check must engage either way.
    graph_path = generate_graph(8, 6, 0.5, 3, lq_max=2)
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode in (0, 1)
        assert "Lower quot" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

QUOTA_BOTH_SIDES_GRAPH = """@PartitionA
a1(0,2);
@End
@PartitionB
b1(0,2);
@End
@PreferenceListsA
a1: b1;
@End
@PreferenceListsB
b1: a1;
@End"""

@pytest.mark.parametrize("alg_flag", ["-s", "-p", "-m"])
def test_no_duplicate_edges_with_proposing_side_quotas(alg_flag):
    # A proposing vertex with leftover capacity re-proposes to its whole list
    # after a level-up; it must not match the same partner twice.
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(QUOTA_BOTH_SIDES_GRAPH)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, alg_flag, "-n", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 0
        edges = [line for line in res.stdout.splitlines() if line.strip()]
        assert edges == ["a1,b1,1"]
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

MANY_TO_MANY_GRAPH = """@PartitionA
a1(0,2), a2(0,2);
@End
@PartitionB
b1(0,2), b2(0,2);
@End
@PreferenceListsA
a1: b1, b2;
a2: b1, b2;
@End
@PreferenceListsB
b1: a1, a2;
b2: a1, a2;
@End"""

def test_many_to_many_stability_checker():
    # Stability checker shouldn't flag already-matched pairs in many-to-many matchings
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(MANY_TO_MANY_GRAPH)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 0
        assert "Matching is Stable" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_stability_checker_catches_undersubscribed_blocking_pair():
    # a1 has quota 2 but the claimed matching fills only one slot; b2 is
    # unmatched and mutually acceptable, so (a1, b2) blocks the matching.
    graph = """@PartitionA
a1(0,2);
@End
@PartitionB
b1, b2;
@End
@PreferenceListsA
a1: b1, b2;
@End
@PreferenceListsB
b1: a1;
b2: a1;
@End"""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph)
        graph_path = f.name
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write("a1,b1,1\n")
        claim_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path, "-e", claim_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert "Matching is not stable" in res.stdout
        assert "a1 -- b2 is a blocking pair" in res.stdout
    finally:
        for p in (graph_path, claim_path):
            if os.path.exists(p):
                os.remove(p)

def test_duplicate_vertex_across_partitions_rejected():
    # Ids must be globally unique: the matching output format identifies
    # vertices by id alone, so a shared id breaks -e verification
    graph = """@PartitionA
x, a2;
@End
@PartitionB
x, b2;
@End
@PreferenceListsA
x: b2;
a2: x;
@End
@PreferenceListsB
x: a2;
b2: x;
@End"""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert "already declared in the other partition" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_max_card_matching_B_proposing_levels():
    # With B proposing, the level cap must come from |B|, not |A|. Here
    # |A| - 1 = 0 would forbid b1's level-1 re-proposal round and change the
    # result. The correct popular-among-max-card matching is a1--b2.
    graph = """@PartitionA
a1;
@End
@PartitionB
b1, b2;
@End
@PreferenceListsA
a1: b2, b1;
@End
@PreferenceListsB
b1: a1;
b2: a1;
@End"""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-m", "-B", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 0
        assert "a1,b2,1" in res.stdout
        assert "Lower quotas satisfied" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_claimed_matching_rejects_duplicate_pair():
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(QUOTA_BOTH_SIDES_GRAPH)
        graph_path = f.name
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write("a1,b1,1\na1,b1,1\n")
        claim_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path, "-e", claim_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert "is listed more than once" in res.stderr
    finally:
        for p in (graph_path, claim_path):
            if os.path.exists(p):
                os.remove(p)

@pytest.mark.parametrize("quota,err_msg", [
    ("abc", "Invalid quota value 'abc'"),
    ("99999999999999999999", "Invalid quota value '99999999999999999999'"),
    ("1x2", "Invalid quota value '1x2'"),
])
def test_invalid_quota_is_rejected_not_crash(quota, err_msg):
    graph = f"""@PartitionA
r1({quota});
@End
@PartitionB
h1;
@End
@PreferenceListsA
r1: h1;
@End
@PreferenceListsB
h1: r1;
@End"""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph)
        graph_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert err_msg in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_claimed_matching_out_of_range_rank():
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(QUOTA_BOTH_SIDES_GRAPH)
        graph_path = f.name
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write("a1,b1,99999999999999999999\n")
        claim_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path, "-e", claim_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert "out-of-range rank" in res.stderr
    finally:
        for p in (graph_path, claim_path):
            if os.path.exists(p):
                os.remove(p)

def test_signature_counts_unmatched_vertices():
    # a1 has quota 2 and takes both hospitals; a2 stays unmatched and must
    # still be counted in the Unassigned line
    graph = """@PartitionA
a1(0,2), a2;
@End
@PartitionB
b1, b2;
@End
@PreferenceListsA
a1: b1, b2;
a2: b1;
@End
@PreferenceListsB
b1: a1, a2;
b2: a1;
@End"""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph)
        graph_path = f.name
    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
        sig_path = f.name
    try:
        res = subprocess.run([BINARY_PATH, "-s", "-i", graph_path, "-o", os.devnull, "-g", sig_path], capture_output=True, text=True)
        assert res.returncode == 0
        with open(sig_path) as f:
            assert "Unassigned 1" in f.read()
    finally:
        for p in (graph_path, sig_path):
            if os.path.exists(p):
                os.remove(p)

def test_sparse_graph_memory_is_linear_in_edges():
    # Rank lookup storage must be O(E), not O(V^2): on a sparse 20k-vertex
    # instance the old per-vertex flat rank arrays alone needed ~1.6 GB,
    # which cannot fit under this address-space cap
    import resource
    graph_path = generate_graph(10000, 10000, 0.001, 2)
    try:
        def limit_mem():
            cap = 512 * 1024 * 1024
            resource.setrlimit(resource.RLIMIT_AS, (cap, cap))
        res = subprocess.run([BINARY_PATH, "-s", "-n", "-i", graph_path, "-o", os.devnull],
                             capture_output=True, text=True, preexec_fn=limit_mem)
        assert res.returncode == 0
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_generator_rejects_invalid_args():
    cases = [
        (["5", "5", "0.5", "0"], "max_quota must be at least 1"),
        (["0", "5", "0.5", "2"], "num_A and num_B must be at least 1"),
        (["abc", "5", "0.5", "2"], "must be integers"),
        (["5", "5", "1.5", "2"], "edge_prob must be between 0 and 1"),
    ]
    for args, err_msg in cases:
        res = subprocess.run([GENERATOR_PATH] + args + [os.devnull], capture_output=True, text=True)
        assert res.returncode == 1
        assert err_msg in res.stderr

def test_missing_arg_handling():
    res = subprocess.run([BINARY_PATH, "-i"], capture_output=True, text=True)
    assert res.returncode == 1
    assert "Option -i requires an argument" in res.stderr

def test_missing_alg_flag():
    graph_path = generate_graph(5, 5, 0.5, 1)
    try:
        res = subprocess.run([BINARY_PATH, "-i", graph_path], capture_output=True, text=True)
        assert res.returncode == 1
        assert "Must specify algorithm flag" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)

def test_claimed_matching_verification_invalid():
    graph_path = generate_graph(5, 5, 0.5, 1)
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
            out_path = f.name
        with open(out_path, "w") as f:
            f.write("invalid_node1,invalid_node2,1\n")
        cmd = [BINARY_PATH, "-s", "-i", graph_path, "-e", out_path]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 1
        assert "Verification failed: matching is invalid." in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        if os.path.exists(out_path):
            os.remove(out_path)

def test_claimed_matching_verification_semantic_errors():
    graph_content = """@PartitionA
r1(0,1), r2(0,1);
@End
@PartitionB
h1(0,1), h2(0,1);
@End
@PreferenceListsA
r1: h1, h2;
r2: h1;
@End
@PreferenceListsB
h1: r2, r1;
h2: r1;
@End"""

    graph_content_no_r1_in_h2 = """@PartitionA
r1(0,1), r2(0,1);
@End
@PartitionB
h1(0,1), h2(0,1);
@End
@PreferenceListsA
r1: h1, h2;
r2: h1;
@End
@PreferenceListsB
h1: r2, r1;
h2: r2;
@End"""

    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph_content)
        graph_path = f.name

    with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
        f.write(graph_content_no_r1_in_h2)
        graph_path_no_r1_in_h2 = f.name

    try:
        errors = [
            (graph_path, "non_existent_file.txt", "Error: Could not open claimed matching file", 1),
            (graph_path, "r1,h1,abc\n", "Syntax Error: Line 1 has non-numeric rank: abc", 1),
            (graph_path, "r1,h1\n", "is not in 'u_id,v_id,rank' format", 1),
            (graph_path, "r1,unknown,1\n", "references unknown vertex: unknown", 1),
            (graph_path, "r1,r2,1\n", "links vertices in the same partition", 1),
            (graph_path, "r1,h1,2\n", "Stated rank 2 does not match actual rank 1", 1),
            (graph_path, "r2,h2,1\n", "h2 is not in the preference list of r2", 1),
            (graph_path_no_r1_in_h2, "r1,h2,2\n", "Incompatible preference lists", 1),
            (graph_path, "r1,h1,1\nr2,h1,1\n", "exceeded its upper quota", 1)
        ]

        for gp, file_content, err_msg, expected_code in errors:
            with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
                if file_content == "non_existent_file.txt":
                    claim_path = "non_existent_file_path.txt"
                else:
                    f.write(file_content)
                    claim_path = f.name
            try:
                cmd = [BINARY_PATH, "-s", "-i", gp, "-e", claim_path]
                res = subprocess.run(cmd, capture_output=True, text=True)
                assert res.returncode == expected_code
                assert err_msg in res.stderr or err_msg in res.stdout
            finally:
                if file_content != "non_existent_file.txt" and os.path.exists(claim_path):
                    os.remove(claim_path)
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        if os.path.exists(graph_path_no_r1_in_h2):
            os.remove(graph_path_no_r1_in_h2)

def test_parser_errors():
    cases = [
        ("""@PartitionA r1, r1; @End
@PartitionB h1; @End
@PreferenceListsA @End
@PreferenceListsB @End""", "Duplicate vertex: r1"),
        ("""@PartitionA r1(2,1); @End
@PartitionB h1; @End
@PreferenceListsA @End
@PreferenceListsB @End""", "Lower quota cannot be greater than Upper quota"),
        ("""@PartitionA r1 @End
@PartitionB h1; @End
@PreferenceListsA @End
@PreferenceListsB @End""", "Expected token ','"),
        ("""@PartitionA r1; @End
@PartitionA r2; @End
@PartitionB h1; @End
@PreferenceListsA @End
@PreferenceListsB @End""", "duplicate partition listing"),
        ("""@PartitionA r1; @End
@PartitionB h1; @End
@PreferenceListsA r1: h1; @End
@PreferenceListsA r1: h1; @End
@PreferenceListsB @End""", "duplicate preference listing"),
        ("""@PartitionA r1; @End
@PartitionB h1; @End
@PreferenceListsA r1: h2; @End
@PreferenceListsB @End""", "Vertex not found: h2"),
        ("""@PartitionA r1; @End
@PartitionB h1; @End
@PreferenceListsA r1: h1, h1; @End
@PreferenceListsB @End""", "is inserted multiple times in r1's preference list"),
        ("""@PartitionA r1; @End
@PartitionB h1; @End
@PreferenceListsA r1: h1; @End
@PreferenceListsB h1: ; @End""", "Incompatible preference lists"),
        ("""@PartitionA r1; @End
@PartitionB h1; @End
@PreferenceListsA r1: h1; @End
@PreferenceListsB h1: r2; @End""", "Vertex not found: r2"),
        ("""@PartitionA r1; @End
@PartitionB h1; @End
@PreferenceListsA r2: h1; @End
@PreferenceListsB @End""", "Vertex not found: r2")
    ]
    for graph_content, err_msg in cases:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt", mode="w") as f:
            f.write(graph_content)
            graph_path = f.name
        try:
            cmd = [BINARY_PATH, "-s", "-i", graph_path]
            res = subprocess.run(cmd, capture_output=True, text=True)
            # Parse errors must be detectable from the exit code
            assert res.returncode == 1
            assert err_msg in res.stderr or err_msg in res.stdout
        finally:
            if os.path.exists(graph_path):
                os.remove(graph_path)

def test_additional_command_flags():
    graph_path = generate_graph(5, 5, 0.5, 1)
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
            sig_path = f.name
        with tempfile.NamedTemporaryFile(delete=False, suffix=".txt") as f:
            valid_match_path = f.name

        cmd = [BINARY_PATH, "-s", "-i", graph_path, "-o", valid_match_path, "-g", sig_path]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0
        assert os.path.exists(sig_path)
        with open(sig_path, "r") as f:
            sig_content = f.read()
        assert "Signature" in sig_content

        cmd = [BINARY_PATH, "-z"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 1
        assert "Unknown option" in res.stderr

        cmd = [BINARY_PATH, "-s", "-i", "non_existent_graph.txt"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 1
        assert "Could not open input file" in res.stderr

        cmd = [BINARY_PATH, "-s", "-i", graph_path, "-o", "/non_existent_dir/out.txt"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 1
        assert "Could not open output file" in res.stderr

        cmd = [BINARY_PATH, "-i", graph_path, "-e", valid_match_path]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 1
        assert "Please specify -s (stable) or -p (popular) flag for verification" in res.stderr
    finally:
        if os.path.exists(graph_path):
            os.remove(graph_path)
        if os.path.exists(sig_path):
            os.remove(sig_path)
        if os.path.exists(valid_match_path):
            os.remove(valid_match_path)
