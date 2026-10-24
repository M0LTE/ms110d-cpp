#pragma once

// Loader for the transcriptions of the standard's tables in reference/spec-tables/. The
// directory comes from the compile definition MS110D_SPEC_TABLES_DIR (set by CMake). The
// files are committed, so a missing file is a test failure, not a skip.

#include <string>
#include <vector>

namespace ms110d_test {

struct CsvTable {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;  // fields as written, trimmed
};

// Full path of a file in reference/spec-tables/.
std::string spec_table_path(const std::string& file);

// Whole text of a transcription file; throws std::runtime_error if it cannot be read.
std::string read_spec_text(const std::string& file);

// A comma separated transcription: the first line is the header.
CsvTable read_spec_csv(const std::string& file);

// The integers inside the first {...} of a text transcription (the C arrays fixedPN, cntPN,
// widPN). Header prose before the brace is ignored.
std::vector<int> read_spec_brace_ints(const std::string& file);

// Parses a decimal text as a float exactly as the C++ compiler reads the same literal.
float parse_float(const std::string& text);

}  // namespace ms110d_test
