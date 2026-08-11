#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <cstdlib>

// True (with `out` set) iff `s` is a non-negative integer.
static bool parse_int(const char* s, int& out) {
    if (s == nullptr || *s == '\0') return false;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    out = std::atoi(s);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <num_A> <num_B> <edge_prob> <max_quota> [lq_max] [output_file]\n"
                  << "  lq_max: if > 0, B vertices get a random lower quota in [0, min(lq_max, upper)]\n"
                  << "          (default 0 = no lower quotas). If the 5th argument is not an\n"
                  << "          integer it is treated as the output file (backward compatible).\n";
        return 1;
    }

    int num_A = 0, num_B = 0, max_quota = 0;
    double edge_prob = 0.0;
    try {
        num_A = std::stoi(argv[1]);
        num_B = std::stoi(argv[2]);
        edge_prob = std::stod(argv[3]);
        max_quota = std::stoi(argv[4]);
    } catch (const std::exception&) {
        std::cerr << "Error: num_A, num_B, max_quota must be integers and edge_prob a number\n";
        return 1;
    }
    if (num_A < 1 || num_B < 1) {
        std::cerr << "Error: num_A and num_B must be at least 1\n";
        return 1;
    }
    if (max_quota < 1) {
        std::cerr << "Error: max_quota must be at least 1\n";
        return 1;
    }
    if (edge_prob < 0.0 || edge_prob > 1.0) {
        std::cerr << "Error: edge_prob must be between 0 and 1\n";
        return 1;
    }

    // The 5th arg is lq_max only if it parses as an integer, otherwise it is the
    // output file, so old `gen A B p q out.txt` calls keep working unchanged.
    int lq_max = 0;
    const char* output_path = nullptr;
    if (argc > 5) {
        int parsed = 0;
        if (parse_int(argv[5], parsed)) {
            lq_max = parsed;
            if (argc > 6) {
                output_path = argv[6];
            }
        } else {
            output_path = argv[5];
        }
    }
    if (lq_max < 0) {
        std::cerr << "Error: lq_max must be non-negative\n";
        return 1;
    }

    std::ostream* out = &std::cout;
    std::ofstream fout;
    if (output_path) {
        fout.open(output_path);
        if (!fout) {
            std::cerr << "Error opening file: " << output_path << "\n";
            return 1;
        }
        out = &fout;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);
    std::uniform_int_distribution<> quota_dis(1, max_quota);
    std::uniform_int_distribution<> index_A(1, num_A);
    std::uniform_int_distribution<> index_B(1, num_B);

    std::vector<std::string> A(num_A);
    for (int i = 0; i < num_A; ++i) {
        A[i] = "a" + std::to_string(i + 1);
    }

    std::vector<std::string> B(num_B);
    std::vector<int> quotas(num_B);
    std::vector<int> lower_quotas(num_B, 0);
    for (int i = 0; i < num_B; ++i) {
        B[i] = "b" + std::to_string(i + 1);
        quotas[i] = quota_dis(gen);
        if (lq_max > 0) {
            int lq_cap = std::min(lq_max, quotas[i]);
            std::uniform_int_distribution<> lq_dis(0, lq_cap);
            lower_quotas[i] = lq_dis(gen);
        }
    }

    std::vector<std::vector<bool>> adj(num_A, std::vector<bool>(num_B, false));

    for (int i = 0; i < num_A; ++i) {
        int target = index_B(gen) - 1;
        adj[i][target] = true;
    }
    for (int j = 0; j < num_B; ++j) {
        int target = index_A(gen) - 1;
        adj[target][j] = true;
    }

    for (int i = 0; i < num_A; ++i) {
        for (int j = 0; j < num_B; ++j) {
            if (dis(gen) < edge_prob) {
                adj[i][j] = true;
            }
        }
    }

    *out << "@PartitionA\n";
    for (int i = 0; i < num_A; ++i) {
        *out << A[i] << (i == num_A - 1 ? " ;" : ", ");
    }
    *out << "\n@End\n\n";

    *out << "@PartitionB\n";
    for (int i = 0; i < num_B; ++i) {
        if (lower_quotas[i] > 0) {
            *out << B[i] << "(" << lower_quotas[i] << "," << quotas[i] << ")";
        } else {
            *out << B[i] << "(" << quotas[i] << ")";
        }
        *out << (i == num_B - 1 ? " ;" : ", ");
    }
    *out << "\n@End\n\n";

    *out << "@PreferenceListsA\n";
    for (int i = 0; i < num_A; ++i) {
        std::vector<std::string> prefs;
        for (int j = 0; j < num_B; ++j) {
            if (adj[i][j]) {
                prefs.push_back(B[j]);
            }
        }
        std::shuffle(prefs.begin(), prefs.end(), gen);
        *out << A[i] << " : ";
        for (size_t k = 0; k < prefs.size(); ++k) {
            *out << prefs[k] << (k == prefs.size() - 1 ? " ;" : ", ");
        }
        *out << "\n";
    }
    *out << "@End\n\n";

    *out << "@PreferenceListsB\n";
    for (int j = 0; j < num_B; ++j) {
        std::vector<std::string> prefs;
        for (int i = 0; i < num_A; ++i) {
            if (adj[i][j]) {
                prefs.push_back(A[i]);
            }
        }
        std::shuffle(prefs.begin(), prefs.end(), gen);
        *out << B[j] << " : ";
        for (size_t k = 0; k < prefs.size(); ++k) {
            *out << prefs[k] << (k == prefs.size() - 1 ? " ;" : ", ");
        }
        *out << "\n";
    }
    *out << "@End\n";

    return 0;
}
