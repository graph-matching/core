// C ABI over the header-only matching engine, built as a shared library so the
// algorithms can be driven from Python (CLI or FastAPI) via ctypes.
//
// Everything is text in / text out: no graph handles are exposed, so callers
// never have to free anything but the returned GmResult.

#include "BipartiteGraph.h"
#include "GraphReader.h"
#include "NProposingMatching.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

extern "C" {

// alg codes, mirrored in graph_matcher.py
enum GmAlgorithm {
    GM_STABLE = 0,   // -s
    GM_POPULAR = 1,  // -p
    GM_MAX_CARD = 2  // -m
};

// All char* fields are malloc'd and owned by the caller; release with gm_free.
// Never NULL on return -- an empty result is an empty string.
struct GmResult {
    int status;       // 0 = success, 1 = parse/verification failure
    int parsed;       // 1 if the graph parsed; 0 means status 1 came from parsing
    char* matching;   // computed matching, "a_id,b_id,rank" per line
    char* signature;  // rank-distribution signature (empty unless requested)
    char* out;        // engine output that main.cc used to send to stdout
    char* err;        // engine output that main.cc used to send to stderr
};

}  // extern "C"

namespace {

char* dup_str(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) {
        std::memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

// The engine writes diagnostics straight to std::cout / std::cerr. Rather than
// thread those through every call site, swap the stream buffers for the
// duration of a call and hand the captured text back to the caller.
struct StreamCapture {
    std::ostringstream out, err;
    std::streambuf* saved_out;
    std::streambuf* saved_err;

    StreamCapture()
        : saved_out(std::cout.rdbuf(out.rdbuf())), saved_err(std::cerr.rdbuf(err.rdbuf())) {}
    ~StreamCapture() {
        std::cout.rdbuf(saved_out);
        std::cerr.rdbuf(saved_err);
    }
};

// ponytail: one global lock -- the std::cout/std::cerr swap above is process
// wide, so calls cannot overlap. Give the engine explicit ostream& parameters
// if a threaded server ever needs real concurrency here.
std::mutex g_lock;

std::unique_ptr<MatchingAlgorithm> make_algorithm(int alg) {
    switch (alg) {
        case GM_STABLE: return std::make_unique<StableMarriage>();
        case GM_POPULAR: return std::make_unique<MaxCardPopularMatching>();
        case GM_MAX_CARD: return std::make_unique<PopularMatchingAmongMaxCardMatchings>();
        default: return nullptr;
    }
}

GmResult finish(int status, int parsed, StreamCapture& cap, const std::string& matching = "",
                const std::string& signature = "") {
    GmResult r;
    r.status = status;
    r.parsed = parsed;
    r.matching = dup_str(matching);
    r.signature = dup_str(signature);
    r.out = dup_str(cap.out.str());
    r.err = dup_str(cap.err.str());
    return r;
}

// Parses "u_id,v_id,rank" lines against G. Diagnostics go to std::cerr (i.e.
// into the capture buffer); ok is false if the claim is syntactically or
// semantically invalid. Ported verbatim from the old main.cc.
Matching read_claimed_matching(const std::string& text, const BipartiteGraph* G, bool& ok) {
    ok = true;

    Matching M(G->size());

    std::unordered_map<std::string_view, const Vertex*> lookup;
    for (const auto& it : G->getPartitionA()) {
        lookup[it->id] = it.get();
    }
    for (const auto& it : G->getPartitionB()) {
        lookup[it->id] = it.get();
    }

    std::istringstream infile(text);
    std::string line;
    int line_num = 0;
    while (std::getline(infile, line)) {
        line_num++;
        while (!line.empty() && isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string_view line_view(line);
        size_t comma1 = line_view.find(',');
        if (comma1 == std::string_view::npos) {
            std::cerr << "Syntax Error: Line " << line_num
                      << " is not in 'u_id,v_id,rank' format: " << line << "\n";
            ok = false;
            return M;
        }
        std::string_view u_id = line_view.substr(0, comma1);

        std::string_view rest = line_view.substr(comma1 + 1);
        size_t comma2 = rest.find(',');
        if (comma2 == std::string_view::npos) {
            std::cerr << "Syntax Error: Line " << line_num
                      << " is not in 'u_id,v_id,rank' format: " << line << "\n";
            ok = false;
            return M;
        }
        std::string_view v_id = rest.substr(0, comma2);
        std::string_view rank_str = rest.substr(comma2 + 1);

        bool is_num = !rank_str.empty();
        long long rank_val = 0;
        for (char ch : rank_str) {
            if (!isdigit(static_cast<unsigned char>(ch))) {
                is_num = false;
                break;
            }
            rank_val = rank_val * 10 + (ch - '0');
            if (rank_val > std::numeric_limits<int>::max()) {
                std::cerr << "Syntax Error: Line " << line_num
                          << " has out-of-range rank: " << std::string(rank_str) << "\n";
                ok = false;
                return M;
            }
        }
        if (!is_num) {
            std::cerr << "Syntax Error: Line " << line_num
                      << " has non-numeric rank: " << std::string(rank_str) << "\n";
            ok = false;
            return M;
        }
        int rank = static_cast<int>(rank_val);

        auto it_u = lookup.find(u_id);
        auto it_v = lookup.find(v_id);

        if (it_u == lookup.end() || it_v == lookup.end()) {
            std::cerr << "Semantic Error: Line " << line_num << " references unknown vertex: "
                      << (it_u == lookup.end() ? std::string(u_id) : std::string(v_id)) << "\n";
            ok = false;
            return M;
        }

        const Vertex* u = it_u->second;
        const Vertex* v = it_v->second;

        size_t limitA = G->getPartitionA().size();
        bool u_in_A = (u->index < limitA);
        bool v_in_A = (v->index < limitA);
        if (u_in_A == v_in_A) {
            std::cerr << "Semantic Error: Line " << line_num
                      << " links vertices in the same partition: " << std::string(u_id) << " and "
                      << std::string(v_id) << "\n";
            ok = false;
            return M;
        }

        int computed_rank = u->getRank(v);
        if (computed_rank == -1) {
            std::cerr << "Semantic Error: Line " << line_num << ": " << std::string(v_id)
                      << " is not in the preference list of " << std::string(u_id) << "\n";
            ok = false;
            return M;
        }
        if (computed_rank != rank) {
            std::cerr << "Semantic Error: Line " << line_num << ": Stated rank " << rank
                      << " does not match actual rank " << computed_rank << "\n";
            ok = false;
            return M;
        }

        int computed_rank_v = v->getRank(u);
        if (computed_rank_v == -1) {
            std::cerr << "Semantic Error: Line " << line_num << ": " << u_id
                      << " is not in the preference list of " << v_id << "\n";
            ok = false;
            return M;
        }

        if (M.hasPartner(u, v)) {
            std::cerr << "Semantic Error: Line " << line_num << ": pair " << std::string(u_id) << ","
                      << std::string(v_id) << " is listed more than once\n";
            ok = false;
            return M;
        }

        M.addMatch(u, v, rank, computed_rank_v, 0);
    }

    auto check_quota = [&](const std::vector<std::unique_ptr<Vertex>>& partition) {
        for (const auto& it : partition) {
            const Vertex* u = it.get();
            if (M.getNumPartners(u) > u->upper_quota) {
                std::cerr << "Semantic Error: Vertex " << u->id << " exceeded its upper quota of "
                          << u->upper_quota << " (matched to " << M.getNumPartners(u)
                          << " partners)\n";
                ok = false;
                return;
            }
        }
    };
    check_quota(G->getPartitionA());
    if (ok) {
        check_quota(G->getPartitionB());
    }

    return M;
}

}  // namespace

extern "C" {

// Computes a matching for graph_text. Returns status 1 if the graph does not
// parse or if verification fails; the matching is still returned in that case,
// matching the CLI behaviour of printing an infeasible matching anyway.
GmResult gm_solve(const char* graph_text, int alg, int a_proposing, int verify,
                  int want_signature) {
    std::lock_guard<std::mutex> guard(g_lock);
    StreamCapture cap;

    auto algorithm = make_algorithm(alg);
    if (!algorithm) {
        std::cerr << "Error: Unknown algorithm code " << alg << "\n";
        return finish(1, 0, cap);
    }

    std::unique_ptr<BipartiteGraph> graph;
    try {
        std::istringstream input(graph_text ? graph_text : "");
        GraphReader reader(input);
        graph = reader.readGraph();
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return finish(1, 0, cap);
    }
    if (!graph) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return finish(1, 0, cap);
    }

    Matching result = algorithm->computeMatching(graph.get(), a_proposing != 0);

    bool check_passed = true;
    bool feasible = true;
    if (verify) {
        feasible = check_lower_quota_feasibility(graph.get(), result, std::cerr);
        // Stability/popularity verifier applies to -s and -p only.
        if (alg == GM_STABLE || alg == GM_POPULAR) {
            check_passed = algorithm->checker(graph.get(), result, a_proposing != 0, std::cerr);
        }
    }

    std::ostringstream matching, signature;
    print_matching(graph.get(), result, matching);
    if (want_signature) {
        print_signature(graph.get(), result, signature);
    }

    return finish((check_passed && feasible) ? 0 : 1, 1, cap, matching.str(), signature.str());
}

// Verifies a claimed matching against graph_text. alg selects the checker and
// must be GM_STABLE or GM_POPULAR.
GmResult gm_verify(const char* graph_text, const char* claimed_text, int alg, int a_proposing) {
    std::lock_guard<std::mutex> guard(g_lock);
    StreamCapture cap;

    if (alg != GM_STABLE && alg != GM_POPULAR) {
        std::cerr << "Error: Please specify -s (stable) or -p (popular) flag for verification.\n";
        return finish(1, 0, cap);
    }

    std::unique_ptr<BipartiteGraph> graph;
    try {
        std::istringstream input(graph_text ? graph_text : "");
        GraphReader reader(input);
        graph = reader.readGraph();
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return finish(1, 0, cap);
    }
    if (!graph) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return finish(1, 0, cap);
    }

    bool ok = false;
    Matching M = read_claimed_matching(claimed_text ? claimed_text : "", graph.get(), ok);
    if (!ok) {
        std::cerr << "Verification failed: matching is invalid.\n";
        return finish(1, 1, cap);
    }

    bool passed = false;
    auto algorithm = make_algorithm(alg);
    std::cout << (alg == GM_STABLE ? "Running Stable Marriage verification checker...\n"
                                   : "Running MaxCardPopular verification checker...\n");
    passed = algorithm->checker(graph.get(), M, a_proposing != 0, std::cout);
    bool feasible = check_lower_quota_feasibility(graph.get(), M, std::cout);

    return finish((passed && feasible) ? 0 : 1, 1, cap);
}

void gm_free(GmResult* r) {
    if (!r) return;
    std::free(r->matching);
    std::free(r->signature);
    std::free(r->out);
    std::free(r->err);
    r->matching = r->signature = r->out = r->err = nullptr;
}

}  // extern "C"
