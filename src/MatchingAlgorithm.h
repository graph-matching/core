#ifndef MATCHING_ALGORITHM_H
#define MATCHING_ALGORITHM_H

#include "BipartiteGraph.h"
#include "Matching.h"
#include <ostream>

class MatchingAlgorithm {
public:
    virtual ~MatchingAlgorithm() = default;

    virtual Matching computeMatching(const BipartiteGraph* graph, bool isAProposing) = 0;

    // Returns true if the matching passed the check (stable / certificate issued)
    virtual bool checker(const BipartiteGraph* graph, const Matching& matching, bool isAProposing, std::ostream& out) = 0;
};

#endif // MATCHING_ALGORITHM_H
