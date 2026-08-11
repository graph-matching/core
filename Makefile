CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Isrc
HEADERS = src/Vertex.h src/BipartiteGraph.h src/Matching.h src/MatchingAlgorithm.h src/NProposingMatching.h src/GraphReader.h

all: build_dir build/graph_matcher build/generator

build_dir:
	mkdir -p build

build/graph_matcher: src/main.cc $(HEADERS)
	$(CXX) $(CXXFLAGS) src/main.cc -o build/graph_matcher

build/generator: src/generator.cc
	$(CXX) $(CXXFLAGS) src/generator.cc -o build/generator

test: all
	uv run pytest test_solver.py -q

clean:
	rm -rf build

.PHONY: all clean build_dir test
