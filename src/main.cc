#include "BipartiteGraph.h"
#include "GraphReader.h"
#include "NProposingMatching.h"
#include <iostream>
#include <fstream>
#include <unistd.h>

void usage(const char *prog) {
    std::cerr << "Usage: "<< prog << " [options]\n"
              << "  -A                  Proposing partition is A (default)\n"
              << "  -B                  Proposing partition is B\n"
              << "  -s                  Compute stable matching\n"
              << "  -p                  Compute maximum cardinality popular matching\n"
              << "  -m                  Compute popular matching among max cardinality matchings\n"
              << "  -i <input_file>     Path to the input graph file (reads from stdin if omitted)\n"
              << "  -o <output_file>    Path to write the computed matching (prints to stdout if omitted)\n"
              << "  -g <sig_file>       Path to write the signature of the matching\n"
              << "  -n                  Do NOT run the verifier (verification runs by default)\n"
              << "  -e <claimed_file>   Verify a claimed matching file (requires -s or -p)\n"
              << "Verification (on by default, disable with -n): a lower-quota feasibility\n"
              << "check runs for all algorithms; the stability (-s) / popularity (-p)\n"
              << "checker runs for -s and -p only.\n"
              << "Exit codes: 0 = success, 1 = usage/parse/file error or verification failed\n";
}

Matching read_claimed_matching(std::string_view filepath, const BipartiteGraph* G, bool& syntax_ok, bool& semantic_ok) {
    syntax_ok = true;
    semantic_ok = true;

    size_t V = G->size();
    Matching M(V);

    std::ifstream infile((std::string(filepath)));
    if (!infile.is_open()) {
        std::cerr << "Error: Could not open claimed matching file: " << filepath << "\n";
        syntax_ok = false;
        return M;
    }

    std::unordered_map<std::string_view, const Vertex*> lookup;
    for (const auto& it : G->getPartitionA()) {
        lookup[it->id] = it.get();
    }
    for (const auto& it : G->getPartitionB()) {
        lookup[it->id] = it.get();
    }

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
            syntax_ok = false;
            return M;
        }
        std::string_view u_id = line_view.substr(0, comma1);

        std::string_view rest = line_view.substr(comma1 + 1);
        size_t comma2 = rest.find(',');
        if (comma2 == std::string_view::npos) {
            std::cerr << "Syntax Error: Line " << line_num
                      << " is not in 'u_id,v_id,rank' format: " << line << "\n";
            syntax_ok = false;
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
                syntax_ok = false;
                return M;
            }
        }
        if (!is_num) {
            std::cerr << "Syntax Error: Line " << line_num
                      << " has non-numeric rank: " << std::string(rank_str) << "\n";
            syntax_ok = false;
            return M;
        }
        int rank = static_cast<int>(rank_val);

        auto it_u = lookup.find(u_id);
        auto it_v = lookup.find(v_id);

        if (it_u == lookup.end() || it_v == lookup.end()) {
            std::cerr << "Semantic Error: Line " << line_num
                      << " references unknown vertex: "
                      << (it_u == lookup.end() ? std::string(u_id) : std::string(v_id)) << "\n";
            semantic_ok = false;
            return M;
        }

        const Vertex* u = it_u->second;
        const Vertex* v = it_v->second;

        size_t limitA = G->getPartitionA().size();
        bool u_in_A = (u->index < limitA);
        bool v_in_A = (v->index < limitA);
        if (u_in_A == v_in_A) {
            std::cerr << "Semantic Error: Line " << line_num
                      << " links vertices in the same partition: " << std::string(u_id) << " and " << std::string(v_id) << "\n";
            semantic_ok = false;
            return M;
        }

        int computed_rank = u->getRank(v);
        if (computed_rank == -1) {
            std::cerr << "Semantic Error: Line " << line_num << ": " << std::string(v_id)
                      << " is not in the preference list of " << std::string(u_id) << "\n";
            semantic_ok = false;
            return M;
        }
        if (computed_rank != rank) {
            std::cerr << "Semantic Error: Line " << line_num << ": Stated rank "
                      << rank << " does not match actual rank " << computed_rank << "\n";
            semantic_ok = false;
            return M;
        }

        int computed_rank_v = v->getRank(u);
        if (computed_rank_v == -1) {
            std::cerr << "Semantic Error: Line " << line_num << ": " << u_id
                      << " is not in the preference list of " << v_id << "\n";
            semantic_ok = false;
            return M;
        }

        if (M.hasPartner(u, v)) {
            std::cerr << "Semantic Error: Line " << line_num << ": pair "
                      << std::string(u_id) << "," << std::string(v_id)
                      << " is listed more than once\n";
            semantic_ok = false;
            return M;
        }

        M.addMatch(u, v, rank, computed_rank_v, 0);
    }

    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (M.getNumPartners(u) > u->upper_quota) {
            std::cerr << "Semantic Error: Vertex " << u->id
                      << " exceeded its upper quota of " << u->upper_quota
                      << " (matched to " << M.getNumPartners(u) << " partners)\n";
            semantic_ok = false;
            return M;
        }
    }
    for (const auto& it : G->getPartitionB()) {
        const Vertex* u = it.get();
        if (M.getNumPartners(u) > u->upper_quota) {
            std::cerr << "Semantic Error: Vertex " << u->id
                      << " exceeded its upper quota of " << u->upper_quota
                      << " (matched to " << M.getNumPartners(u) << " partners)\n";
            semantic_ok = false;
            return M;
        }
    }

    return M;
}

int main(int argc, char* argv[]) {
    int c = 0;
    bool compute_stable = false;
    bool compute_popular = false;
    bool compute_max_card = false;
    bool verify = true;
    bool A_proposing = true;
    const char* input_file = nullptr;
    const char* output_file = nullptr;
    const char* sig_file = nullptr;
    const char* claimed_file = nullptr;

    opterr = 0;
    while ((c = getopt(argc, argv, ":ABspmng:i:o:e:")) != -1) {
        switch (c) {
            case 'A': A_proposing = true; break;
            case 'B': A_proposing = false; break;
            case 's': compute_stable = true;  break;
            case 'p': compute_popular = true; break;
            case 'm': compute_max_card = true; break;
            case 'n': verify = false; break;
            case 'g': sig_file = optarg; break;
            case 'i': input_file = optarg; break;
            case 'o': output_file = optarg; break;
            case 'e': claimed_file = optarg; break;
            case ':':
                std::cerr << "Option -" << (char)optopt << " requires an argument\n";
                usage(argv[0]);
                return 1;
            case '?':
                std::cerr << "Unknown option: -" << (char)optopt << "\n";
                usage(argv[0]);
                return 1;
            default: break;
        }
    }

    std::istream* input = &std::cin;
    std::ifstream fileStream;

    if (input_file) {
        fileStream.open(input_file);
        if (!fileStream) {
            std::cerr << "Error: Could not open input file " << input_file << "\n";
            return 1;
        }
        input = &fileStream;
    }

    GraphReader reader(*input);
    std::unique_ptr<BipartiteGraph> graph = reader.readGraph();

    if (input_file) {
        fileStream.close();
    }

    if (!graph) {
        std::cerr << "Error: Failed to parse bipartite graph.\n";
        return 1;
    }

    if (claimed_file) {
        bool syntax_ok = false, semantic_ok = false;
        Matching M = read_claimed_matching(claimed_file, graph.get(), syntax_ok, semantic_ok);
        if (!syntax_ok || !semantic_ok) {
            std::cerr << "Verification failed: matching is invalid.\n";
            return 1;
        }

        bool passed = false;
        if (compute_stable) {
            std::cout << "Running Stable Marriage verification checker...\n";
            StableMarriage alg;
            passed = alg.checker(graph.get(), M, A_proposing, std::cout);
        } else if (compute_popular) {
            std::cout << "Running MaxCardPopular verification checker...\n";
            MaxCardPopularMatching alg;
            passed = alg.checker(graph.get(), M, A_proposing, std::cout);
        } else {
            std::cerr << "Error: Please specify -s (stable) or -p (popular) flag for verification.\n";
            usage(argv[0]);
            return 1;
        }
        bool feasible = check_lower_quota_feasibility(graph.get(), M, std::cout);
        return (passed && feasible) ? 0 : 1;
    }

    std::unique_ptr<MatchingAlgorithm> algorithm;
    if (compute_stable) {
        algorithm = std::make_unique<StableMarriage>();
    } else if (compute_popular) {
        algorithm = std::make_unique<MaxCardPopularMatching>();
    } else if (compute_max_card) {
        algorithm = std::make_unique<PopularMatchingAmongMaxCardMatchings>();
    } else {
        std::cerr << "Error: Must specify algorithm flag (-s, -p, or -m).\n";
        usage(argv[0]);
        return 1;
    }

    Matching result = algorithm->computeMatching(graph.get(), A_proposing);

    bool check_passed = true;
    bool feasible = true;
    if (verify) {
        feasible = check_lower_quota_feasibility(graph.get(), result, std::cerr);
        // Stability/popularity verifier applies to -s and -p only.
        if (compute_stable || compute_popular) {
            check_passed = algorithm->checker(graph.get(), result, A_proposing, std::cerr);
        }
    }

    // Printed even when infeasible; no alternate matching is suggested.
    if (output_file) {
        std::ofstream fileout(output_file);
        if (!fileout) {
            std::cerr << "Error: Could not open output file " << output_file << "\n";
            return 1;
        }
        print_matching(graph.get(), result, fileout);
        fileout.close();
    } else {
        print_matching(graph.get(), result, std::cout);
    }

    if (sig_file) {
        std::ofstream filesig(sig_file);
        if (!filesig) {
            std::cerr << "Error: Could not open signature file " << sig_file << "\n";
            return 1;
        }
        print_signature(graph.get(), result, filesig);
        filesig.close();
    }

    return (check_passed && feasible) ? 0 : 1;
}
