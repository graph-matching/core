CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Isrc
HEADERS = src/Vertex.h src/BipartiteGraph.h src/Matching.h src/MatchingAlgorithm.h src/NProposingMatching.h src/GraphReader.h

# The solver ships as a shared library (loaded by graph_matcher.py via ctypes),
# so the same build serves the CLI and any in-process caller such as FastAPI.
LIB = build/libgraphmatch.so

all: build_dir $(LIB) build/generator

build_dir:
	mkdir -p build

$(LIB): src/api.cc $(HEADERS)
	$(CXX) $(CXXFLAGS) -fPIC -shared src/api.cc -o $(LIB)

build/generator: src/generator.cc
	$(CXX) $(CXXFLAGS) src/generator.cc -o build/generator

test: all
	uv run pytest test_solver.py -q

clean:
	rm -rf build

.PHONY: all clean build_dir test
