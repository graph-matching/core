// C ABI over the header-only matching engine, built as a shared library so the
// algorithms can be driven from Python (CLI or FastAPI) via ctypes.
//
// Everything is text in / text out: no graph handles are exposed, so callers
// never have to free anything but the returned GmResult.

#include "BipartiteGraph.h"
#include "GraphReader.h"
#include "NProposingMatching.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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


// ---------------------------------------------------------------- diff ----
//
// Comparing two runs means comparing two things that can differ
// independently: the instances, and the matchings computed over them. Two
// different instances can produce the same matching, so the report keeps them
// apart rather than collapsing both into a single "identical".

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_string(const std::string& s) { return '"' + json_escape(s) + '"'; }

std::string json_strings(const std::vector<std::string>& items) {
    std::ostringstream o;
    o << '[';
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) o << ',';
        o << json_string(items[i]);
    }
    o << ']';
    return o.str();
}

// Items that are already JSON objects.
std::string json_objects(const std::vector<std::string>& items) {
    std::ostringstream o;
    o << '[';
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) o << ',';
        o << items[i];
    }
    o << ']';
    return o.str();
}

std::unique_ptr<BipartiteGraph> parse_graph(const char* text) {
    try {
        std::istringstream input(text ? text : "");
        GraphReader reader(input);
        return reader.readGraph();
    } catch (const std::exception&) {
        return nullptr;
    }
}

struct VertexView {
    std::vector<std::string> preferences;
    unsigned int lower_quota = 0;
    unsigned int upper_quota = 1;
};

std::map<std::string, VertexView> vertex_views(const BipartiteGraph* G) {
    std::map<std::string, VertexView> views;
    auto collect = [&](const std::vector<std::unique_ptr<Vertex>>& partition) {
        for (const auto& it : partition) {
            VertexView view;
            view.lower_quota = it->lower_quota;
            view.upper_quota = it->upper_quota;
            for (const Vertex* p : it->preferences) {
                view.preferences.push_back(p->id);
            }
            views.emplace(it->id, std::move(view));
        }
    };
    collect(G->getPartitionA());
    collect(G->getPartitionB());
    return views;
}

std::set<std::string> partition_ids(const std::vector<std::unique_ptr<Vertex>>& partition) {
    std::set<std::string> ids;
    for (const auto& it : partition) {
        ids.insert(it->id);
    }
    return ids;
}

std::vector<std::string> missing_from(const std::set<std::string>& lhs,
                                      const std::set<std::string>& rhs) {
    std::vector<std::string> only;
    for (const auto& id : lhs) {
        if (!rhs.count(id)) only.push_back(id);
    }
    return only;
}

// Partners are sorted so the comparison does not depend on the order the
// algorithm happened to add them in.
std::map<std::string, std::vector<std::string>> partners_by_agent(const BipartiteGraph* G,
                                                                  const Matching& M) {
    std::map<std::string, std::vector<std::string>> by_agent;
    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (!M.isMatched(u)) continue;
        std::vector<std::string> partners;
        for (const auto& p : M.getPartners(u)) {
            partners.push_back(p.vertex->id);
        }
        std::sort(partners.begin(), partners.end());
        by_agent.emplace(u->id, std::move(partners));
    }
    return by_agent;
}

struct PairRec {
    std::string a;
    std::string b;
    int rank;
};

std::vector<PairRec> matched_pairs(const BipartiteGraph* G, const Matching& M) {
    std::vector<PairRec> pairs;
    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (!M.isMatched(u)) continue;
        for (const auto& p : M.getPartners(u)) {
            pairs.push_back(PairRec{u->id, p.vertex->id, p.rank});
        }
    }
    return pairs;
}

std::string pair_json(const PairRec& pair) {
    std::ostringstream o;
    o << "{\"a\":" << json_string(pair.a) << ",\"b\":" << json_string(pair.b)
      << ",\"rank\":" << pair.rank << '}';
    return o.str();
}

// A vertex id cannot contain this separator, so it is safe as a pair key.
std::string pair_key(const PairRec& pair) { return pair.a + '\x1f' + pair.b; }

std::string instance_json(const BipartiteGraph* L, const BipartiteGraph* R) {
    const std::set<std::string> left_a = partition_ids(L->getPartitionA());
    const std::set<std::string> right_a = partition_ids(R->getPartitionA());
    const std::set<std::string> left_b = partition_ids(L->getPartitionB());
    const std::set<std::string> right_b = partition_ids(R->getPartitionB());

    const std::vector<std::string> a_only_left = missing_from(left_a, right_a);
    const std::vector<std::string> a_only_right = missing_from(right_a, left_a);
    const std::vector<std::string> b_only_left = missing_from(left_b, right_b);
    const std::vector<std::string> b_only_right = missing_from(right_b, left_b);

    const std::map<std::string, VertexView> left_views = vertex_views(L);
    const std::map<std::string, VertexView> right_views = vertex_views(R);

    std::vector<std::string> preference_changes;
    std::vector<std::string> quota_changes;
    for (const auto& entry : left_views) {
        const auto found = right_views.find(entry.first);
        if (found == right_views.end()) continue;
        const VertexView& left = entry.second;
        const VertexView& right = found->second;

        if (left.preferences != right.preferences) {
            std::ostringstream o;
            o << "{\"vertex\":" << json_string(entry.first)
              << ",\"left\":" << json_strings(left.preferences)
              << ",\"right\":" << json_strings(right.preferences) << '}';
            preference_changes.push_back(o.str());
        }
        if (left.lower_quota != right.lower_quota || left.upper_quota != right.upper_quota) {
            std::ostringstream o;
            o << "{\"vertex\":" << json_string(entry.first) << ",\"left\":[" << left.lower_quota
              << ',' << left.upper_quota << "],\"right\":[" << right.lower_quota << ','
              << right.upper_quota << "]}";
            quota_changes.push_back(o.str());
        }
    }

    const bool identical = a_only_left.empty() && a_only_right.empty() && b_only_left.empty() &&
                           b_only_right.empty() && preference_changes.empty() &&
                           quota_changes.empty();

    std::ostringstream o;
    o << "{\"identical\":" << (identical ? "true" : "false") << ",\"a_size\":[" << left_a.size()
      << ',' << right_a.size() << "],\"b_size\":[" << left_b.size() << ',' << right_b.size()
      << "],\"a_only_in_left\":" << json_strings(a_only_left)
      << ",\"a_only_in_right\":" << json_strings(a_only_right)
      << ",\"b_only_in_left\":" << json_strings(b_only_left)
      << ",\"b_only_in_right\":" << json_strings(b_only_right)
      << ",\"preference_changes\":" << json_objects(preference_changes)
      << ",\"quota_changes\":" << json_objects(quota_changes) << '}';
    return o.str();
}

std::string matchings_json(const BipartiteGraph* L, const Matching& ML, const BipartiteGraph* R,
                           const Matching& MR) {
    const std::vector<PairRec> left_pairs = matched_pairs(L, ML);
    const std::vector<PairRec> right_pairs = matched_pairs(R, MR);

    std::set<std::string> left_keys;
    std::set<std::string> right_keys;
    for (const auto& p : left_pairs) left_keys.insert(pair_key(p));
    for (const auto& p : right_pairs) right_keys.insert(pair_key(p));

    std::vector<std::string> only_left;
    std::vector<std::string> only_right;
    for (const auto& p : left_pairs) {
        if (!right_keys.count(pair_key(p))) only_left.push_back(pair_json(p));
    }
    for (const auto& p : right_pairs) {
        if (!left_keys.count(pair_key(p))) only_right.push_back(pair_json(p));
    }

    // Matched/unmatched transitions only mean something for agents that exist
    // in both instances; anyone else is an instance change, reported above.
    const std::set<std::string> left_agents = partition_ids(L->getPartitionA());
    const std::set<std::string> right_agents = partition_ids(R->getPartitionA());
    const std::map<std::string, std::vector<std::string>> left_partners = partners_by_agent(L, ML);
    const std::map<std::string, std::vector<std::string>> right_partners = partners_by_agent(R, MR);

    std::vector<std::string> changed;
    std::vector<std::string> newly_matched;
    std::vector<std::string> newly_unmatched;
    for (const auto& id : left_agents) {
        if (!right_agents.count(id)) continue;
        const auto in_left = left_partners.find(id);
        const auto in_right = right_partners.find(id);
        const bool matched_left = in_left != left_partners.end();
        const bool matched_right = in_right != right_partners.end();

        if (matched_left && matched_right) {
            if (in_left->second != in_right->second) {
                std::ostringstream o;
                o << "{\"agent\":" << json_string(id)
                  << ",\"left\":" << json_strings(in_left->second)
                  << ",\"right\":" << json_strings(in_right->second) << '}';
                changed.push_back(o.str());
            }
        } else if (matched_right) {
            newly_matched.push_back(id);
        } else if (matched_left) {
            newly_unmatched.push_back(id);
        }
    }

    std::ostringstream o;
    o << "{\"identical\":" << (only_left.empty() && only_right.empty() ? "true" : "false")
      << ",\"cardinality\":[" << left_pairs.size() << ',' << right_pairs.size()
      << "],\"only_in_left\":" << json_objects(only_left)
      << ",\"only_in_right\":" << json_objects(only_right)
      << ",\"partner_changed\":" << json_objects(changed)
      << ",\"newly_matched\":" << json_strings(newly_matched)
      << ",\"newly_unmatched\":" << json_strings(newly_unmatched) << '}';
    return o.str();
}

// Per-side facts that need the instance: the rank signature, lower-quota
// feasibility, and whether the matching really is stable/popular there.

// --------------------------------------------------------------- stats ----

// JSON has no NaN, and an average over zero pairs is not a number, so an empty
// matching reports null rather than a misleading 0.
std::string json_average(long long total, size_t count) {
    if (count == 0) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(total) / static_cast<double>(count));
    return buf;
}

// Everything a caller might want to say about one matching. Ranks are
// 1-based, so a lower average is a better matching for that side.
std::string stats_json(const BipartiteGraph* G, const Matching& M, bool a_proposing) {
    size_t pairs = 0;
    size_t a_matched = 0;
    long long a_rank_total = 0;
    long long b_rank_total = 0;
    long long egalitarian = 0;

    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (!M.isMatched(u)) continue;
        ++a_matched;
        for (const auto& p : M.getPartners(u)) {
            ++pairs;
            const int a_rank = p.rank;
            // -1 would mean u is absent from its partner's list, which
            // read_claimed_matching already rejects; guard anyway so a bad
            // rank cannot silently skew the totals.
            const int b_rank = p.vertex->getRank(u);
            a_rank_total += a_rank;
            if (b_rank > 0) b_rank_total += b_rank;
            egalitarian += a_rank + (b_rank > 0 ? b_rank : 0);
        }
    }

    size_t a_capacity = 0;
    for (const auto& it : G->getPartitionA()) a_capacity += it->upper_quota;

    size_t b_positions = 0;
    size_t b_matched = 0;
    for (const auto& it : G->getPartitionB()) {
        b_positions += it->upper_quota;
        if (M.isMatched(it.get())) ++b_matched;
    }

    const size_t a_vertices = G->getPartitionA().size();
    const size_t b_vertices = G->getPartitionB().size();
    const size_t blocking = find_blocking_pairs(G, M, a_proposing).size();

    std::ostringstream o;
    o << "{\"cardinality\":" << pairs
      << ",\"capacity\":{\"used\":" << pairs << ",\"total\":" << b_positions
      << ",\"ratio\":" << json_average(static_cast<long long>(pairs), b_positions) << "}"
      << ",\"egalitarian_cost\":" << egalitarian
      << ",\"blocking_pairs\":" << blocking
      << ",\"a\":{\"vertices\":" << a_vertices << ",\"capacity\":" << a_capacity
      << ",\"matched\":" << a_matched << ",\"unmatched\":" << (a_vertices - a_matched)
      << ",\"avg_rank\":" << json_average(a_rank_total, pairs) << "}"
      << ",\"b\":{\"vertices\":" << b_vertices << ",\"positions\":" << b_positions
      << ",\"filled\":" << pairs << ",\"vacant\":" << (b_positions - pairs)
      << ",\"matched\":" << b_matched
      << ",\"avg_rank\":" << json_average(b_rank_total, pairs) << "}}";
    return o.str();
}

std::string side_json(const BipartiteGraph* G, const Matching& M, int alg, int a_proposing) {
    std::ostringstream signature;
    print_signature(G, M, signature);

    std::ostringstream notes;
    const bool feasible = check_lower_quota_feasibility(G, M, notes);

    std::string verdict = "null";
    if (alg == GM_STABLE || alg == GM_POPULAR) {
        std::unique_ptr<MatchingAlgorithm> algorithm = make_algorithm(alg);
        const bool passed = algorithm->checker(G, M, a_proposing != 0, notes);
        verdict = passed ? "true" : "false";
    }

    std::ostringstream o;
    o << "{\"signature\":" << json_string(signature.str())
      << ",\"feasible\":" << (feasible ? "true" : "false") << ",\"passes_check\":" << verdict
      << ",\"report\":" << json_string(notes.str())
      << ",\"stats\":" << stats_json(G, M, a_proposing != 0) << '}';
    return o.str();
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

    std::unique_ptr<BipartiteGraph> graph = parse_graph(graph_text);
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

    std::unique_ptr<BipartiteGraph> graph = parse_graph(graph_text);
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


// Compares two runs. Each side is its own (graph, matching) pair, because the
// two questions may have been asked of different instances. The JSON report is
// returned in the `matching` field; `out`/`err` keep engine diagnostics.
GmResult gm_diff(const char* graph_a_text, const char* matching_a_text, const char* graph_b_text,
                 const char* matching_b_text, int alg, int a_proposing) {
    std::lock_guard<std::mutex> guard(g_lock);
    StreamCapture cap;

    std::unique_ptr<BipartiteGraph> left = parse_graph(graph_a_text);
    std::unique_ptr<BipartiteGraph> right = parse_graph(graph_b_text);
    if (!left || !right) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return finish(1, 0, cap);
    }

    bool left_ok = false;
    bool right_ok = false;
    Matching ML = read_claimed_matching(matching_a_text ? matching_a_text : "", left.get(), left_ok);
    Matching MR =
        read_claimed_matching(matching_b_text ? matching_b_text : "", right.get(), right_ok);
    if (!left_ok || !right_ok) {
        std::cerr << "Diff failed: a matching is not valid for its instance.\n";
        return finish(1, 1, cap);
    }

    std::ostringstream json;
    json << "{\"instance\":" << instance_json(left.get(), right.get())
         << ",\"matchings\":" << matchings_json(left.get(), ML, right.get(), MR)
         << ",\"left\":" << side_json(left.get(), ML, alg, a_proposing)
         << ",\"right\":" << side_json(right.get(), MR, alg, a_proposing) << '}';

    return finish(0, 1, cap, json.str());
}

// Statistics for one matching over one instance. Split out from gm_solve so the
// verifier and any already-stored run can ask for them without solving again.
GmResult gm_stats(const char* graph_text, const char* matching_text, int a_proposing) {
    std::lock_guard<std::mutex> guard(g_lock);
    StreamCapture cap;

    std::unique_ptr<BipartiteGraph> graph = parse_graph(graph_text);
    if (!graph) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return finish(1, 0, cap);
    }

    bool ok = false;
    Matching M = read_claimed_matching(matching_text ? matching_text : "", graph.get(), ok);
    if (!ok) {
        std::cerr << "Statistics failed: matching is not valid for this instance.\n";
        return finish(1, 1, cap);
    }

    return finish(0, 1, cap, stats_json(graph.get(), M, a_proposing != 0));
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
