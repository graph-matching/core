#ifndef MATCHING_H
#define MATCHING_H

#include "Vertex.h"
#include <vector>
#include <algorithm>

struct Partner {
    const Vertex* vertex;
    int rank;
    int level;

    Partner(const Vertex* vertex, int rank, int level)
        : vertex(vertex), rank(rank), level(level) {}

    // Operator<: true if 'this' is LESS preferred than 'other'
    bool operator<(const Partner& other) const {
        if (level < other.level) {
            return true;
        } else if (level > other.level) {
            return false;
        } else {
            return rank > other.rank; // higher rank value means less preferred (smaller in ordering)
        }
    }
};

class Matching {
private:
    // Flat vector where matches[v->index] holds partners of v
    std::vector<std::vector<Partner>> matches;

public:
    Matching() = default;
    explicit Matching(size_t num_vertices) : matches(num_vertices) {}

    // Add match between u and v
    void addMatch(const Vertex* u, const Vertex* v, int u_rank, int v_rank, int level) {
        if (u->index >= matches.size() || v->index >= matches.size()) {
            size_t max_idx = std::max(u->index, v->index);
            matches.resize(max_idx + 1);
        }
        matches[u->index].emplace_back(v, u_rank, level);
        matches[v->index].emplace_back(u, v_rank, level);
    }

    // Remove match between u and v
    void removeMatch(const Vertex* u, const Vertex* v) {
        if (u->index < matches.size()) {
            auto& list = matches[u->index];
            list.erase(std::remove_if(list.begin(), list.end(), 
                [v](const Partner& p) { return p.vertex == v; }), list.end());
        }
        if (v->index < matches.size()) {
            auto& list = matches[v->index];
            list.erase(std::remove_if(list.begin(), list.end(), 
                [u](const Partner& p) { return p.vertex == u; }), list.end());
        }
    }

    // Remove the least preferred partner of a vertex.
    // Returns the Partner that was removed.
    Partner removeLeastPreferred(const Vertex* u) {
        auto& list = matches[u->index];
        auto worst_it = std::min_element(list.begin(), list.end()); // min element is the least preferred
        Partner removed = *worst_it;
        const Vertex* v = removed.vertex;

        // Erase from u's list
        list.erase(worst_it);

        // Erase u from v's list
        if (v->index < matches.size()) {
            auto& v_list = matches[v->index];
            v_list.erase(std::remove_if(v_list.begin(), v_list.end(), 
                [u](const Partner& p) { return p.vertex == u; }), v_list.end());
        }

        return removed;
    }

    const std::vector<Partner>& getPartners(const Vertex* v) const {
        return matches[v->index];
    }

    // True if u and v are already matched to each other
    bool hasPartner(const Vertex* u, const Vertex* v) const {
        if (u->index >= matches.size()) return false;
        const auto& list = matches[u->index];
        return std::any_of(list.begin(), list.end(),
            [v](const Partner& p) { return p.vertex == v; });
    }

    bool isMatched(const Vertex* v) const {
        if (v->index >= matches.size()) return false;
        return !matches[v->index].empty();
    }

    Partner getLeastPreferred(const Vertex* u) const {
        const auto& list = matches[u->index];
        auto worst_it = std::min_element(list.begin(), list.end());
        return *worst_it;
    }

    size_t getNumPartners(const Vertex* v) const {
        if (v->index >= matches.size()) return 0;
        return matches[v->index].size();
    }

    const std::vector<std::vector<Partner>>& getAllMatches() const {
        return matches;
    }
};

#endif // MATCHING_H
