#ifndef BIPARTITE_GRAPH_H
#define BIPARTITE_GRAPH_H

#include "Vertex.h"
#include <vector>
#include <memory>

class BipartiteGraph {
private:
    std::vector<std::unique_ptr<Vertex>> partitionA;
    std::vector<std::unique_ptr<Vertex>> partitionB;

public:
    BipartiteGraph() = default;

    // Disable copying
    BipartiteGraph(const BipartiteGraph&) = delete;
    BipartiteGraph& operator=(const BipartiteGraph&) = delete;

    // Enable move semantics
    BipartiteGraph(BipartiteGraph&& other) noexcept 
        : partitionA(std::move(other.partitionA)), partitionB(std::move(other.partitionB)) {}

    BipartiteGraph& operator=(BipartiteGraph&& other) noexcept {
        if (this != &other) {
            partitionA = std::move(other.partitionA);
            partitionB = std::move(other.partitionB);
        }
        return *this;
    }

    ~BipartiteGraph() = default;

    void addVertexA(std::unique_ptr<Vertex> v) {
        partitionA.push_back(std::move(v));
    }

    void addVertexB(std::unique_ptr<Vertex> v) {
        partitionB.push_back(std::move(v));
    }

    const std::vector<std::unique_ptr<Vertex>>& getPartitionA() const { return partitionA; }
    const std::vector<std::unique_ptr<Vertex>>& getPartitionB() const { return partitionB; }

    // Assigns contiguous indices and builds the O(1) rank lookups for all vertices
    void finalize() {
        size_t index = 0;
        for (auto& v : partitionA) {
            v->index = index++;
        }
        for (auto& v : partitionB) {
            v->index = index++;
        }

        size_t totalVertices = index;

        // A flat array costs 4*V bytes per vertex, a hash map roughly 48
        // bytes per preference entry; pick per vertex so total rank storage
        // stays O(E) on sparse graphs while dense vertices keep the flat array
        auto build_rank_lookup = [totalVertices](Vertex* v) {
            if (v->preferences.size() * 12 >= totalVertices) {
                v->use_rank_array = true;
                v->rank_array.assign(totalVertices, -1);
                for (size_t i = 0; i < v->preferences.size(); ++i) {
                    v->rank_array[v->preferences[i]->index] = static_cast<int>(i) + 1; // 1-indexed ranks
                }
            } else {
                v->rank_map.reserve(v->preferences.size());
                for (size_t i = 0; i < v->preferences.size(); ++i) {
                    v->rank_map[v->preferences[i]->index] = static_cast<int>(i) + 1; // 1-indexed ranks
                }
            }
        };

        for (auto& v : partitionA) build_rank_lookup(v.get());
        for (auto& v : partitionB) build_rank_lookup(v.get());
    }

    size_t size() const {
        return partitionA.size() + partitionB.size();
    }
};

#endif // BIPARTITE_GRAPH_H
