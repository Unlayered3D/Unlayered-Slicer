#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/MultiplanarAudit.hpp"
#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>

using namespace Slic3r;

TEST_CASE("Multiplanar audit detects rotary words in firmware motion formats", "[autoplanes][audit]")
{
    const std::string line = GENERATE(
        "G1 X10 A5", "  G1 X10 B-2", "G01 X10 C+0.5", "G1X10A5", "N42 G02X10Y10B.5*123",
        "\tg03x10y10c-.5", "N43G0A0", "G1 (move) X10 (ignore) B5");
    ScopedTemporaryFile file(".gcode");
    { boost::nowide::ofstream stream(file.string()); stream << line << '\n'; }
    GCodeProcessorResult result;
    const auto audit = audit_multiplanar_toolpaths(result, file.string());
    CHECK(audit.rotary_words == 1);
    CHECK(audit.first_rotary_line == line);
}

TEST_CASE("Multiplanar audit ignores rotary-looking words outside motion", "[autoplanes][audit]")
{
    const std::string line = GENERATE(
        "; G1 A5", "G1 X10 ; B5", "G1 X10 (C5)", "G1 X10 (nested (B5) comment)",
        "G10 A5", "G90 B5", "M104 C5", "G1 X10*123 A5", "G1 X10 B", "G1 X10 C+.");
    ScopedTemporaryFile file(".gcode");
    { boost::nowide::ofstream stream(file.string()); stream << line << '\n'; }
    GCodeProcessorResult result;
    const auto audit = audit_multiplanar_toolpaths(result, file.string());
    CHECK(audit.rotary_words == 0);
    CHECK(audit.first_rotary_line.empty());
}
