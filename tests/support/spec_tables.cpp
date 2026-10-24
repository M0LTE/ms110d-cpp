#include "spec_tables.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifndef MS110D_SPEC_TABLES_DIR
#error "MS110D_SPEC_TABLES_DIR must be defined by the build"
#endif

namespace ms110d_test {

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in, field, ',')) out.push_back(trim(field));
    if (!line.empty() && line.back() == ',') out.emplace_back();
    return out;
}

}  // namespace

std::string spec_table_path(const std::string& file) {
    return std::string(MS110D_SPEC_TABLES_DIR) + "/" + file;
}

std::string read_spec_text(const std::string& file) {
    std::ifstream in(spec_table_path(file), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read spec table " + spec_table_path(file));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

CsvTable read_spec_csv(const std::string& file) {
    std::istringstream in(read_spec_text(file));
    CsvTable t;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        if (first) {
            t.header = split_csv(line);
            first = false;
        } else {
            t.rows.push_back(split_csv(line));
        }
    }
    return t;
}

std::vector<int> read_spec_brace_ints(const std::string& file) {
    const std::string text = read_spec_text(file);
    const auto open = text.find('{');
    const auto close = text.find('}', open);
    if (open == std::string::npos || close == std::string::npos) {
        throw std::runtime_error("no {...} in " + file);
    }
    std::vector<int> out;
    std::string body = text.substr(open + 1, close - open - 1);
    for (char& c : body) {
        if (c == ',' || c == '\n' || c == '\r') c = ' ';
    }
    std::istringstream in(body);
    std::string tok;
    while (in >> tok) out.push_back(std::atoi(tok.c_str()));
    return out;
}

float parse_float(const std::string& text) {
    return std::strtof(text.c_str(), nullptr);
}

}  // namespace ms110d_test
