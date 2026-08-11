#ifndef VERTEX_H
#define VERTEX_H

#include <string>
#include <vector>
#include <unordered_map>

class Vertex {
public:
    std::string id;
    size_t index = 0;
    std::vector<Vertex*> preferences;
    // Rank lookup storage, chosen per vertex by BipartiteGraph::finalize():
    // dense vertices use a flat array indexed by vertex index (fast, 4*V
    // bytes), sparse vertices a hash map (O(deg) memory). Total rank storage
    // is O(E) on sparse graphs instead of O(V^2).
    std::vector<int> rank_array;
    std::unordered_map<size_t, int> rank_map;
    bool use_rank_array = false;
    unsigned int lower_quota = 0;
    unsigned int upper_quota = 1;

public:
    explicit Vertex(std::string id) : id(std::move(id)) {}

    explicit Vertex(std::string id, unsigned int upper_quota)
        : id(std::move(id)), upper_quota(upper_quota) {}

    explicit Vertex(std::string id, unsigned int lower_quota, unsigned int upper_quota)
        : id(std::move(id)), lower_quota(lower_quota), upper_quota(upper_quota) {}

    // Disable copying because vertex lifetime is owned uniquely by BipartiteGraph
    Vertex(const Vertex&) = delete;
    Vertex& operator=(const Vertex&) = delete;

    ~Vertex() = default;

    // O(1) rank lookup (array read or average-case hash lookup)
    int getRank(const Vertex* v) const {
        if (use_rank_array) {
            return v->index < rank_array.size() ? rank_array[v->index] : -1;
        }
        auto it = rank_map.find(v->index);
        return it == rank_map.end() ? -1 : it->second;
    }
};

#endif // VERTEX_H
