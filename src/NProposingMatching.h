#ifndef N_PROPOSING_MATCHING_H
#define N_PROPOSING_MATCHING_H

#include "MatchingAlgorithm.h"
#include <vector>
#include <stack>
#include <map>
#include <unordered_map>
#include <sstream>
#include <iostream>
#include <memory>
#include <algorithm>

struct VertexBookkeeping {
    size_t begin = 0;
    size_t end = 0;
    int level = 0;
    bool in_free_list = false;

    VertexBookkeeping() = default;
    VertexBookkeeping(size_t begin, size_t end)
        : begin(begin), end(end), level(0), in_free_list(false) {}

    bool is_exhausted() const {
        return begin >= end;
    }
};

class NProposingMatching : public MatchingAlgorithm {
protected:
    int maxLevel;
    std::vector<int> levels; // Tracks proposing level of each vertex (size V)

public:
    explicit NProposingMatching(int maxLevel) : maxLevel(maxLevel) {}
    ~NProposingMatching() override = default;

    Matching computeMatching(const BipartiteGraph* graph, bool isAProposing) override {
        size_t V = graph->size();
        Matching matching(V);
        levels.assign(V, 0);

        int resolvedMaxLevel = maxLevel;
        const auto& proposing = isAProposing ? graph->getPartitionA() : graph->getPartitionB();
        if (resolvedMaxLevel < 0) {
            // Levels bound how often a proposing vertex restarts its list, so
            // the cap must come from the proposing partition (|B|-1 when B
            // proposes), not unconditionally from partition A
            resolvedMaxLevel = static_cast<int>(proposing.size()) - 1;
        }

        std::vector<VertexBookkeeping> bookkeeping(V);
        std::vector<const Vertex*> free_list;

        // Initialize free list and bookkeeping for proposing partition
        for (const auto& v_ptr : proposing) {
            const Vertex* u = v_ptr.get();
            free_list.push_back(u);
            bookkeeping[u->index] = VertexBookkeeping(0, u->preferences.size());
            bookkeeping[u->index].in_free_list = true;
        }

        while (!free_list.empty()) {
            const Vertex* u = free_list.back();
            free_list.pop_back();
            bookkeeping[u->index].in_free_list = false;

            if (u->upper_quota > 0 && !bookkeeping[u->index].is_exhausted()) {
                const Vertex* v = u->preferences[bookkeeping[u->index].begin];

                if (v->upper_quota == 0 || v->preferences.empty() || matching.hasPartner(u, v)) {
                    // Do nothing: v is unmatchable, or u is already matched to v
                    // (a vertex with leftover capacity re-proposes to its whole
                    // list after a level-up and must not match the same partner twice)
                } else if (matching.getNumPartners(v) == v->upper_quota) {
                    Partner v_worst = matching.getLeastPreferred(v);

                    int u_rank_in_v = v->getRank(u);
                    int u_level = bookkeeping[u->index].level;
                    Partner possible_partner(u, u_rank_in_v, u_level);

                    if (v_worst < possible_partner) {
                        // Displace worst partner
                        matching.removeLeastPreferred(v);

                        const Vertex* w = v_worst.vertex;

                        // Add u and v to matching
                        int u_rank_in_pref = static_cast<int>(bookkeeping[u->index].begin) + 1;
                        matching.addMatch(u, v, u_rank_in_pref, u_rank_in_v, u_level);

                        // Put displaced partner w back in the free list
                        if (!bookkeeping[w->index].in_free_list) {
                            bookkeeping[w->index].begin++;
                            bookkeeping[w->index].in_free_list = true;
                            free_list.push_back(w);
                        }
                    }
                } else {
                    // Match u and v directly
                    int u_rank_in_pref = static_cast<int>(bookkeeping[u->index].begin) + 1;
                    int u_rank_in_v = v->getRank(u);
                    int u_level = bookkeeping[u->index].level;
                    matching.addMatch(u, v, u_rank_in_pref, u_rank_in_v, u_level);
                }

                // If u still has capacity, push it back to propose to its next choices
                if (u->upper_quota > matching.getNumPartners(u)) {
                    if (!bookkeeping[u->index].in_free_list) {
                        bookkeeping[u->index].begin++;
                        bookkeeping[u->index].in_free_list = true;
                        free_list.push_back(u);
                    }
                }
            } else if (bookkeeping[u->index].level < resolvedMaxLevel) {
                // Increment level and reset proposal index to start over
                bookkeeping[u->index].level++;
                bookkeeping[u->index].begin = 0;
                bookkeeping[u->index].in_free_list = true;
                free_list.push_back(u);
            }
        }

        // Record the final proposing levels
        for (const auto& v_ptr : proposing) {
            const Vertex* u = v_ptr.get();
            levels[u->index] = bookkeeping[u->index].level;
        }

        return matching;
    }
};

// stable marriage = n proposing matching (bipartite graph, is a proposing, max level = 0)
class StableMarriage : public NProposingMatching {
public:
    StableMarriage() : NProposingMatching(0) {}

    bool checker(const BipartiteGraph* G, const Matching& M, bool A_proposing, std::ostream& out) override {
        std::stringstream stmp;
        const auto& proposing_partition = A_proposing ? G->getPartitionA() : G->getPartitionB();

        // Each preference edge is visited exactly once, so its weight can be
        // computed and checked in place (no V x V weight matrix needed).
        int flag = 1;
        for (const auto& it : proposing_partition) {
            const Vertex* u = it.get();
            for (const Vertex* v : u->preferences) {
                int weight = 0;

                // Contribution from v (non-proposing side).
                // An undersubscribed vertex has a free slot and accepts any
                // acceptable partner outright; only a full vertex compares
                // the candidate against its worst current partner.
                if (M.getNumPartners(v) < v->upper_quota) {
                    weight += 1;
                } else if (M.isMatched(v)) {
                    Partner v_worst = M.getLeastPreferred(v);
                    int u_rank = v->getRank(u);
                    if (v_worst.rank > u_rank) {
                        weight += 1;
                    } else if (u_rank > v_worst.rank) {
                        weight += -1;
                    }
                }

                // Contribution from u (proposing side)
                if (M.getNumPartners(u) < u->upper_quota) {
                    weight += 1;
                } else if (M.isMatched(u)) {
                    Partner u_worst = M.getLeastPreferred(u);
                    int v_rank = u->getRank(v);
                    if (u_worst.rank > v_rank) {
                        weight += 1;
                    } else if (v_rank > u_worst.rank) {
                        weight += -1;
                    }
                }

                // If u prefers v over M(u) and vice-versa (weight is 2), and they are not already matched, it's a blocking pair
                if (weight == 2 && v->upper_quota > 0 && u->upper_quota > 0 && !M.hasPartner(u, v)) {
                    if (flag) {
                        stmp << "Matching is not stable\n";
                    }
                    flag = 0;
                    stmp << "Edge " << u->id << " -- " << v->id << " is a blocking pair!\n";
                }
            }
        }

        if (flag) {
            stmp << "Matching is Stable\n";
        }

        out << stmp.str();
        return flag != 0;
    }
};

// max card popular matching = n proposing matching (bipartite graph, is a proposing, max level = 1)
class MaxCardPopularMatching : public NProposingMatching {
public:
    MaxCardPopularMatching() : NProposingMatching(1) {}

    bool checker(const BipartiteGraph* G, const Matching& M, bool A_proposing, std::ostream& out) override {
        std::stringstream stmp;
        size_t V = G->size();

        // If levels vector is empty (e.g. verifier mode), initialize it to size V filled with 0
        if (levels.empty()) {
            levels.assign(V, 0);
        }

        const auto& proposing_partition = A_proposing ? G->getPartitionA() : G->getPartitionB();
        const auto& non_proposing_partition = A_proposing ? G->getPartitionB() : G->getPartitionA();

        // 1. Create clones for non-proposing partition
        std::unordered_map<std::string, std::unique_ptr<Vertex>> Bm;
        std::unordered_map<const Vertex*, int> matchedB; // count of matched clones for each original non-proposing vertex

        for (const auto& it : non_proposing_partition) {
            const Vertex* u = it.get();
            matchedB[u] = 0;
            for (unsigned int i = 0; i < u->upper_quota; ++i) {
                std::string u_id = u->id + "///" + std::to_string(i);
                Bm[u_id] = std::make_unique<Vertex>(u_id, 0, 1);
            }
        }

        // 2. Create the matched pair matching for clones (Mm) using pointers as keys
        std::unordered_map<const Vertex*, std::vector<Partner>> Mm;

        for (const auto& it : proposing_partition) {
            const Vertex* u = it.get();
            int u_level = levels[u->index];

            if (M.isMatched(u)) {
                const auto& partners = M.getPartners(u);
                for (const auto& p : partners) {
                    const Vertex* v = p.vertex;
                    std::string v_id = v->id + "///" + std::to_string(matchedB[v]);
                    const Vertex* v_clone = Bm[v_id].get();

                    int v_rank = v->getRank(u);

                    Mm[u].emplace_back(v_clone, p.rank, u_level);
                    Mm[v_clone].emplace_back(u, v_rank, u_level);

                    matchedB[v]++;
                }
            }
        }

        // 3. Assign dual values
        std::unordered_map<const Vertex*, int> dual_value;

        for (const auto& it : proposing_partition) {
            const Vertex* u = it.get();
            int u_level = levels[u->index];

            auto it_mm = Mm.find(u);
            if (it_mm != Mm.end() && !it_mm->second.empty()) {
                dual_value[u] = (u_level ? -1 : 1);
                for (const auto& p : it_mm->second) {
                    const Vertex* v_clone = p.vertex;
                    dual_value[v_clone] = (u_level ? 1 : -1);
                }
            }
        }

        // Helper to get least preferred partner from Mm
        auto get_least_preferred_Mm = [&](const Vertex* node) -> Partner {
            const auto& list = Mm[node];
            auto worst_it = std::min_element(list.begin(), list.end());
            return *worst_it;
        };

        // Helper to check if node is matched in Mm
        auto is_matched_Mm = [&](const Vertex* node) -> bool {
            auto it = Mm.find(node);
            return it != Mm.end() && !it->second.empty();
        };

        // 4. Check coverage
        int flag = 1;
        std::unordered_map<const Vertex*, std::unordered_map<const Vertex*, int>> edge_weights;

        for (const auto& it : proposing_partition) {
            const Vertex* u = it.get();

            for (const Vertex* v : u->preferences) {
                int u_contribution = 0, v_contribution = 0;

                // Check if (u, v) is matched in the original matching M
                bool is_partner = false;
                if (M.isMatched(u)) {
                    for (const auto& p : M.getPartners(u)) {
                        if (p.vertex == v) {
                            is_partner = true;
                            break;
                        }
                    }
                }

                if (is_partner) {
                    const Vertex* v_matched_clone = Mm[u].front().vertex;
                    edge_weights[u][v_matched_clone] = 0;

                    if (dual_value[u] + dual_value[v_matched_clone] < 0) {
                        flag = 0;
                        stmp << u->id << "," << v->id << "\n";
                    }
                    continue;
                }

                // Non-matching edge: u's contribution
                if (is_matched_Mm(u)) {
                    const auto& partners = Mm[u];
                    if (partners.empty()) {
                        u_contribution = 1;
                    } else {
                        Partner u_worst = get_least_preferred_Mm(u);
                        int v_rank = u->getRank(v);

                        if (u_worst.rank > v_rank) {
                            u_contribution = 1;
                        } else if (v_rank > u_worst.rank) {
                            u_contribution = -1;
                        } else {
                            u_contribution = 0;
                        }
                    }
                } else {
                    u_contribution = 1;
                }

                // Non-matching edge: v's contribution
                for (unsigned int i = 0; i < v->upper_quota; ++i) {
                    std::string v_id = v->id + "///" + std::to_string(i);
                    const Vertex* v_clone = Bm[v_id].get();

                    if (is_matched_Mm(v_clone)) {
                        const auto& partners = Mm[v_clone];
                        if (partners.empty()) {
                            v_contribution = 1;
                        } else {
                            Partner v_worst = get_least_preferred_Mm(v_clone);
                            int u_rank = v->getRank(u);

                            if (v_worst.rank > u_rank) {
                                v_contribution = 1;
                            } else if (u_rank > v_worst.rank) {
                                v_contribution = -1;
                            } else {
                                v_contribution = 0;
                            }
                        }
                    } else {
                        v_contribution = 1;
                    }

                    int edge_weight = u_contribution + v_contribution;
                    if (dual_value[u] + dual_value[v_clone] < edge_weight) {
                        flag = 0;
                        stmp << u->id << "," << v->id << "\n";
                    }
                }
            }
        }

        // Bounded dual value check
        for (const auto& it : proposing_partition) {
            const Vertex* u = it.get();
            if (is_matched_Mm(u)) {
                for (const auto& p : Mm[u]) {
                    const Vertex* v_clone = p.vertex;
                    if (dual_value[v_clone] < -1) {
                        flag = 0;
                        stmp << v_clone->id << "," << v_clone->id << "\n";
                    }
                    if (dual_value[u] < -1) {
                        flag = 0;
                        stmp << u->id << "," << u->id << "\n";
                    }
                }
            }
        }

        // Calculate dual value sum
        int dual_sum = 0;
        for (const auto& it : proposing_partition) {
            auto it_dv = dual_value.find(it.get());
            if (it_dv != dual_value.end()) {
                dual_sum += it_dv->second;
            }
        }
        for (const auto& it : Bm) {
            auto it_dv = dual_value.find(it.second.get());
            if (it_dv != dual_value.end()) {
                dual_sum += it_dv->second;
            }
        }

        if (!flag) {
            stmp << "Edges not covered. \n";
        }
        if (dual_sum) {
            stmp << "Popularity sum is non-zero, equal to " << dual_sum << ". \n";
        }
        if (flag && !dual_sum) {
            stmp << "Passed. Certificate issued!\n";
        }

        out << stmp.str();
        return flag && !dual_sum;
    }
};

// popular matching among max card matchings = n proposing matching (bipartite graph, is a proposing, max level = no of vertices in partition a - 1)
class PopularMatchingAmongMaxCardMatchings : public NProposingMatching {
public:
    PopularMatchingAmongMaxCardMatchings() : NProposingMatching(-1) {}

    bool checker(const BipartiteGraph* G, const Matching& M, bool A_proposing, std::ostream& out) override {
        // TODO: implement this
        return true;
    }
};

// Post-check: the engine honours only upper quotas, so a computed matching may
// leave a vertex below its lower quota. Reports any such violation over both
// partitions; does not repair. Returns true iff every floor is met.
inline bool check_lower_quota_feasibility(const BipartiteGraph* G, const Matching& M, std::ostream& out) {
    std::stringstream stmp;
    bool feasible = true;

    auto check_partition = [&](const std::vector<std::unique_ptr<Vertex>>& partition) {
        for (const auto& it : partition) {
            const Vertex* v = it.get();
            if (v->lower_quota == 0) {
                continue;
            }
            size_t matched = M.getNumPartners(v);
            if (matched < v->lower_quota) {
                feasible = false;
                stmp << "Lower quota violated: " << v->id << " matched " << matched
                     << ", floor " << v->lower_quota << "\n";
            }
        }
    };

    check_partition(G->getPartitionA());
    check_partition(G->getPartitionB());

    if (feasible) {
        stmp << "Lower quotas satisfied\n";
    } else {
        stmp << "Matching is infeasible (lower quota not met)\n";
    }

    out << stmp.str();
    return feasible;
}

// Helper print functions
inline void print_matching(const BipartiteGraph* G, const Matching& M, std::ostream& out) {
    std::stringstream stmp;
    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (M.isMatched(u)) {
            const auto& partners = M.getPartners(u);
            for (const auto& p : partners) {
                stmp << u->id << ','
                     << p.vertex->id << ','
                     << p.rank << '\n';
            }
        }
    }
    out << stmp.str();
}

inline void print_signature(const BipartiteGraph* G, const Matching& M, std::ostream& out) {
    std::stringstream stmp;
    int unmatched = 0;
    size_t num_ranks = 1;

    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (num_ranks < u->preferences.size() + 1) {
            num_ranks = u->preferences.size() + 1;
        }
    }

    std::vector<int> ranks(num_ranks, 0);

    for (const auto& it : G->getPartitionA()) {
        const Vertex* u = it.get();
        if (M.isMatched(u)) {
            for (const auto& p : M.getPartners(u)) {
                if (p.rank < static_cast<int>(num_ranks)) {
                    ranks[p.rank]++;
                }
            }
        } else {
            // Count unmatched vertices directly: a quota > 1 vertex matched to
            // several partners must not offset other vertices' unmatched status
            unmatched++;
        }
    }

    stmp << "Signature\n";
    stmp << "|A| = " << G->getPartitionA().size() << "\n";
    stmp << "|B| = " << G->getPartitionB().size() << "\n";
    stmp << "Max Pref Size of A = " << num_ranks - 1 << "\n";
    for (size_t i = 1; i < num_ranks; i++) {
        stmp << i << " " << ranks[i] << "\n";
    }
    stmp << "Unassigned " << unmatched << "\n";

    out << stmp.str();
}

#endif // N_PROPOSING_MATCHING_H
