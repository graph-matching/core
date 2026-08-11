#ifndef GRAPH_READER_H
#define GRAPH_READER_H

#include "BipartiteGraph.h"
#include <string>
#include <string_view>
#include <iostream>
#include <sstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cctype>
#include <algorithm>
#include <limits>

enum Token {
    TOK_AT = 0,
    TOK_PARTITION_A,
    TOK_PARTITION_B,
    TOK_PREF_LISTS_A,
    TOK_PREF_LISTS_B,
    TOK_END,
    TOK_STRING,
    TOK_COLON,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_LEFT_BRACE,
    TOK_RIGHT_BRACE,
    TOK_EOF,
    TOK_ERROR
};

inline const char* token_to_string(Token tok) {
    switch (tok) {
        case TOK_AT: return "@";
        case TOK_PARTITION_A: return "PartitionA";
        case TOK_PARTITION_B: return "PartitionB";
        case TOK_PREF_LISTS_A: return "PreferenceListsA";
        case TOK_PREF_LISTS_B: return "PreferenceListsB";
        case TOK_END: return "End";
        case TOK_STRING: return "string";
        case TOK_COLON: return ":";
        case TOK_COMMA: return ",";
        case TOK_SEMICOLON: return ";";
        case TOK_LEFT_BRACE: return "(";
        case TOK_RIGHT_BRACE: return ")";
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

class Lexer {
private:
    std::string buffer;
    size_t cursor = 0;
    int lineno = 1;
    std::string_view lexeme;

    char peek() const {
        if (cursor >= buffer.size()) return '\0';
        return buffer[cursor];
    }

    char nextChar() {
        if (cursor >= buffer.size()) return '\0';
        char ch = buffer[cursor++];
        if (ch == '\n') {
            lineno++;
        }
        return ch;
    }

public:
    explicit Lexer(std::istream& in) {
        std::stringstream ss;
        ss << in.rdbuf();
        buffer = ss.str();
    }

    int line_number() const {
        return lineno;
    }

    std::string_view get_lexeme() const {
        return lexeme;
    }

    Token next_token() {
        char ch = peek();
        
        while (true) {
            while (ch != '\0' && isspace(static_cast<unsigned char>(ch))) {
                nextChar();
                ch = peek();
            }
            
            if (ch == '#') {
                while (ch != '\0' && ch != '\n') {
                    nextChar();
                    ch = peek();
                }
                if (ch == '\n') {
                    nextChar();
                }
                ch = peek();
            } else {
                break;
            }
        }

        if (ch == '\0') return TOK_EOF;
        if (ch == ':') { nextChar(); lexeme = ":"; return TOK_COLON; }
        if (ch == '@') { nextChar(); lexeme = "@"; return TOK_AT; }
        if (ch == ',') { nextChar(); lexeme = ","; return TOK_COMMA; }
        if (ch == ';') { nextChar(); lexeme = ";"; return TOK_SEMICOLON; }
        if (ch == '(') { nextChar(); lexeme = "("; return TOK_LEFT_BRACE; }
        if (ch == ')') { nextChar(); lexeme = ")"; return TOK_RIGHT_BRACE; }

        if (isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
            size_t start = cursor;
            while (ch != '\0' && (isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '+')) {
                nextChar();
                ch = peek();
            }
            lexeme = std::string_view(buffer.data() + start, cursor - start);

            if (lexeme == "End") return TOK_END;
            if (lexeme == "PartitionA") return TOK_PARTITION_A;
            if (lexeme == "PartitionB") return TOK_PARTITION_B;
            if (lexeme == "PreferenceListsA") return TOK_PREF_LISTS_A;
            if (lexeme == "PreferenceListsB") return TOK_PREF_LISTS_B;
            return TOK_STRING;
        }

        size_t start = cursor;
        nextChar();
        lexeme = std::string_view(buffer.data() + start, cursor - start);
        return TOK_ERROR;
    }
};

// Parses a quota token; false on empty, non-digit, or unsigned int overflow
// (std::stoi would throw an uncaught exception on those and abort)
inline bool parse_quota(std::string_view s, unsigned int& out) {
    if (s.empty()) return false;
    unsigned long long val = 0;
    for (char ch : s) {
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
        val = val * 10 + static_cast<unsigned long long>(ch - '0');
        if (val > std::numeric_limits<unsigned int>::max()) return false;
    }
    out = static_cast<unsigned int>(val);
    return true;
}

class GraphReader {
private:
    std::unique_ptr<Lexer> lexer;
    Token curtok;
    bool error_occurred = false;

    void consume() {
        curtok = lexer->next_token();
    }

    void match(Token expected) {
        if (curtok != expected) {
            error_occurred = true;
            std::cerr << "Line " << lexer->line_number() 
                      << ": Expected token '" << token_to_string(expected) 
                      << "', but got '" << token_to_string(curtok) 
                      << "' (lexeme: '" << lexer->get_lexeme() << "')\n";
            exit(1);
        }
        consume();
    }

    void read_partition(std::vector<std::unique_ptr<Vertex>>& partition,
                        std::unordered_map<std::string_view, Vertex*>& lookup,
                        const std::unordered_map<std::string_view, Vertex*>& other_lookup) {
        while (curtok != TOK_SEMICOLON) {
            std::string_view v_id = lexer->get_lexeme();
            match(TOK_STRING);

            if (lookup.find(v_id) != lookup.end()) {
                std::cerr << "Line " << lexer->line_number()
                          << ": Duplicate vertex: " << v_id << "\n";
                error_occurred = true;
            }
            // Vertex ids must be unique across the whole graph: the matching
            // output and claimed-matching formats identify vertices by id alone
            if (other_lookup.find(v_id) != other_lookup.end()) {
                std::cerr << "Line " << lexer->line_number()
                          << ": Duplicate vertex: " << v_id
                          << " (already declared in the other partition)\n";
                error_occurred = true;
            }

            unsigned int lower_quota = 0;
            unsigned int upper_quota = 1;

            if (curtok == TOK_LEFT_BRACE) {
                match(TOK_LEFT_BRACE);

                std::string_view quota_str1 = lexer->get_lexeme();
                match(TOK_STRING);
                unsigned int q1 = 0;
                if (!parse_quota(quota_str1, q1)) {
                    std::cerr << "Line " << lexer->line_number()
                              << ": Invalid quota value '" << quota_str1
                              << "' for vertex " << v_id << "\n";
                    error_occurred = true;
                }

                if (curtok == TOK_COMMA) {
                    match(TOK_COMMA);
                    lower_quota = q1;

                    std::string_view quota_str2 = lexer->get_lexeme();
                    match(TOK_STRING);
                    if (!parse_quota(quota_str2, upper_quota)) {
                        std::cerr << "Line " << lexer->line_number()
                                  << ": Invalid quota value '" << quota_str2
                                  << "' for vertex " << v_id << "\n";
                        error_occurred = true;
                    }
                } else {
                    upper_quota = q1;
                }
                match(TOK_RIGHT_BRACE);
            }

            if (lower_quota > upper_quota) {
                std::cerr << "Line " << lexer->line_number() 
                          << ": Lower quota cannot be greater than Upper quota for vertex " << v_id << "\n";
                error_occurred = true;
            }

            auto vertex = std::make_unique<Vertex>(std::string(v_id), lower_quota, upper_quota);
            lookup[vertex->id] = vertex.get();
            partition.push_back(std::move(vertex));

            if (curtok != TOK_SEMICOLON) {
                match(TOK_COMMA);
            }
        }
        match(TOK_SEMICOLON);
        match(TOK_AT);
        match(TOK_END);
    }

    void read_preference_lists(std::unordered_map<std::string_view, Vertex*>& self_lookup,
                               std::unordered_map<std::string_view, Vertex*>& other_lookup) {
        while (curtok != TOK_AT) {
            std::string_view a_id = lexer->get_lexeme();
            
            auto it = self_lookup.find(a_id);
            if (it == self_lookup.end()) {
                std::cerr << "Line " << lexer->line_number() 
                          << ": Vertex not found: " << a_id << "\n";
                error_occurred = true;
                
                match(TOK_STRING);
                match(TOK_COLON);
                while (curtok != TOK_SEMICOLON && curtok != TOK_EOF) {
                    consume();
                }
                if (curtok == TOK_EOF) return;
                match(TOK_SEMICOLON);
                continue;
            }

            Vertex* a = it->second;
            match(TOK_STRING);
            match(TOK_COLON);

            std::unordered_set<const Vertex*> seen_prefs;
            while (curtok != TOK_SEMICOLON) {
                std::string_view b_id = lexer->get_lexeme();
                match(TOK_STRING);

                auto it_b = other_lookup.find(b_id);
                if (it_b == other_lookup.end()) {
                    std::cerr << "Line " << lexer->line_number() 
                              << ": Vertex not found: " << b_id << "\n";
                    error_occurred = true;
                    if (curtok != TOK_SEMICOLON) {
                        match(TOK_COMMA);
                    }
                    continue;
                }

                Vertex* b = it_b->second;
                
                if (seen_prefs.find(b) != seen_prefs.end()) {
                    std::cerr << "Line " << lexer->line_number() 
                              << ": Vertex " << b_id << " is inserted multiple times in " << a_id << "'s preference list\n";
                    error_occurred = true;
                } else {
                    seen_prefs.insert(b);
                    a->preferences.push_back(b);
                }

                if (curtok != TOK_SEMICOLON) {
                    match(TOK_COMMA);
                }
            }
            match(TOK_SEMICOLON);
        }
        match(TOK_AT);
        match(TOK_END);
    }

    void handle_partition(std::vector<std::unique_ptr<Vertex>>& partitionA,
                          std::vector<std::unique_ptr<Vertex>>& partitionB,
                          std::unordered_map<std::string_view, Vertex*>& lookupA,
                          std::unordered_map<std::string_view, Vertex*>& lookupB) {
        if (curtok == TOK_PARTITION_A) {
            match(TOK_PARTITION_A);
            read_partition(partitionA, lookupA, lookupB);
        } else if (curtok == TOK_PARTITION_B) {
            match(TOK_PARTITION_B);
            read_partition(partitionB, lookupB, lookupA);
        } else {
            std::cerr << "Line " << lexer->line_number() << ": Wrong syntax for partition\n";
            exit(1);
        }
    }

    void handle_preference_lists(std::unordered_map<std::string_view, Vertex*>& lookupA,
                                 std::unordered_map<std::string_view, Vertex*>& lookupB) {
        if (curtok == TOK_PREF_LISTS_A) {
            match(TOK_PREF_LISTS_A);
            read_preference_lists(lookupA, lookupB);
        } else if (curtok == TOK_PREF_LISTS_B) {
            match(TOK_PREF_LISTS_B);
            read_preference_lists(lookupB, lookupA);
        } else {
            std::cerr << "Line " << lexer->line_number() << ": Wrong syntax for preference list\n";
            exit(1);
        }
    }

    void compatibility_checker(const std::vector<std::unique_ptr<Vertex>>& self_part,
                               const std::vector<std::unique_ptr<Vertex>>& /*other_part*/) {
        bool flag = true;
        for (const auto& v_ptr : self_part) {
            Vertex* v = v_ptr.get();
            for (Vertex* u : v->preferences) {
                if (u->getRank(v) == -1) {
                    if (flag) {
                        std::cout << "\nIncompatible preference lists \n";
                        flag = false;
                    }
                    std::cout << u->id << " doesn't have " << v->id 
                              << " in its preferences but vice-versa is true.\n";
                }
            }
        }
        if (!flag) {
            exit(1);
        }
    }

public:
    explicit GraphReader(std::istream& in) {
        lexer = std::make_unique<Lexer>(in);
        consume();
    }

    std::unique_ptr<BipartiteGraph> readGraph() {
        auto graph = std::make_unique<BipartiteGraph>();
        std::unordered_map<std::string_view, Vertex*> lookupA;
        std::unordered_map<std::string_view, Vertex*> lookupB;

        std::vector<std::unique_ptr<Vertex>> partitionA;
        std::vector<std::unique_ptr<Vertex>> partitionB;

        match(TOK_AT);
        Token partition = curtok;
        handle_partition(partitionA, partitionB, lookupA, lookupB);

        match(TOK_AT);
        if (curtok != partition) {
            handle_partition(partitionA, partitionB, lookupA, lookupB);
        } else {
            std::cerr << "Line " << lexer->line_number() << ": duplicate partition listing\n";
            exit(1);
        }

        for (auto& v : partitionA) graph->addVertexA(std::move(v));
        for (auto& v : partitionB) graph->addVertexB(std::move(v));

        match(TOK_AT);
        Token pref_lists = curtok;
        handle_preference_lists(lookupA, lookupB);

        match(TOK_AT);
        if (curtok != pref_lists) {
            handle_preference_lists(lookupA, lookupB);
        } else {
            std::cerr << "Line " << lexer->line_number() << ": duplicate preference listing\n";
            exit(1);
        }

        graph->finalize();

        compatibility_checker(graph->getPartitionA(), graph->getPartitionB());
        compatibility_checker(graph->getPartitionB(), graph->getPartitionA());

        if (error_occurred) {
            return nullptr;
        }

        return graph;
    }
};

#endif // GRAPH_READER_H
