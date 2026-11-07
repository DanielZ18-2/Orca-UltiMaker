#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Format/GPXExport.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// A 20 mm cube at 0.4 mm per layer for a Replicator 2. The first layer prints
// at 220 C, the others at 225 C. The Replicator 2 has no heated platform, so
// the plate stays cold.
DynamicPrintConfig replicator2_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", "makerbot_legacy"},
        {"printer_model", "MakerBot Replicator 2"},
        {"gpx_machine_type", "r2"},
        {"printable_area", "0x0,200x0,200x200,0x200"},
        {"nozzle_diameter", "0.4"},
        {"layer_height", 0.4},
        {"initial_layer_print_height", 0.4},
        {"nozzle_temperature_initial_layer", "220"},
        {"nozzle_temperature", "225"},
        {"curr_bed_type", "Cool Plate"},
        {"cool_plate_temp_initial_layer", "0"},
        {"cool_plate_temp", "0"},
    });
    return config;
}

// Slices the cube, in the middle of the 200 x 200 mm plate, into gcode_path
// and converts it the way the export does (BackgroundSlicingProcess): the
// .x3g takes the place of the G-code. A GPX ini file or kept intermediate
// files, both asked for through the environment, would change the outcome.
std::string export_x3g(const std::string &gcode_path)
{
    ScopedEnvVar no_ini("ORCA_GPX_INI", "");
    ScopedEnvVar no_kept_gcode("ORCA_GPX_KEEP_GCODE", "");
    TriangleMesh cube = Test::cube(20);
    cube.translate(90, 90, 0);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{cube}, print, model, replicator2_config(), nullptr, /*arrange=*/false);
    print.process();
    print.export_gcode(gcode_path, nullptr, nullptr);
    return GPXExport::pack_to_archive(gcode_path, print.config());
}

std::string read_file(const std::string &path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// The x3g command that sets the target temperature of tool 0: tool action
// 136, tool 0, action 3 with two payload bytes, the temperature in
// little-endian order. A file carries it without the serial framing.
std::string set_tool0_temperature(unsigned int celsius)
{
    return std::string{char(136), char(0), char(3), char(2), char(celsius & 0xff), char(celsius >> 8)};
}

} // namespace

TEST_CASE("The legacy MakerBot flavor names the .x3g file type", "[GPXExport]")
{
    CHECK(GPXExport::get_archive_extension(gcfMakerBotLegacy) == ".x3g");
    CHECK(GPXExport::get_archive_extension(gcfMarlinFirmware) == ".gcode");
}

TEST_CASE("A legacy MakerBot print becomes an .x3g file with the temperatures of its layers", "[GPXExport]")
{
    ScopedTemporaryDir dir;
    const std::string  gcode_path = (dir.path() / "cube.gcode").string();
    const std::string  x3g        = export_x3g(gcode_path);

    REQUIRE(x3g == (dir.path() / "cube.x3g").string());
    CHECK_FALSE(boost::filesystem::exists(gcode_path));
    CHECK_FALSE(boost::filesystem::exists(x3g + ".native.gcode"));
    const std::string bytes = read_file(x3g);
    // The first layer heats to its temperature, the layers after it to theirs.
    const size_t first_layer = bytes.find(set_tool0_temperature(220));
    REQUIRE(first_layer != std::string::npos);
    CHECK(bytes.find(set_tool0_temperature(225), first_layer) != std::string::npos);
}

// The export dialog saves the G-code under the name of the .x3g file;
// converting it in place must not delete the file it has just written.
TEST_CASE("A G-code saved under the .x3g name becomes the .x3g file in place", "[GPXExport]")
{
    ScopedTemporaryDir dir;
    const std::string  path = (dir.path() / "cube.x3g").string();

    REQUIRE(export_x3g(path) == path);
    // G-code is text, so the binary command shows the file was converted.
    CHECK(read_file(path).find(set_tool0_temperature(220)) != std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(path + ".tmp_gcode_for_gpx"));
    CHECK_FALSE(boost::filesystem::exists(path + ".native.gcode"));
}

TEST_CASE("Only the legacy MakerBot flavor is converted to .x3g", "[GPXExport]")
{
    ScopedTemporaryDir dir;
    const std::string  gcode_path = (dir.path() / "cube.gcode").string();
    {
        boost::nowide::ofstream f(gcode_path);
        f << "G1 X10 Y10 E1\n";
    }
    PrintConfig config;
    config.set_deserialize_strict("gcode_flavor", "marlin2");

    CHECK(GPXExport::pack_to_archive(gcode_path, config).empty());
    CHECK(boost::filesystem::exists(gcode_path));
    CHECK_FALSE(boost::filesystem::exists(dir.path() / "cube.x3g"));
}

// The G-code export pins its numeric locale to "C". The conversion runs after
// it in the numeric locale of the calling thread -- in the GUI the user's,
// often with a decimal comma.
TEST_CASE("An .x3g file converted in a locale with a decimal comma is the same", "[GPXExport]")
{
    {
        CommaNumericLocale probe;
        CNumericLocalesSetter restore;
        if (! probe.apply())
            SKIP("no locale with a comma decimal separator is installed");
    }
    ScopedTemporaryDir dir;
    boost::filesystem::create_directories(dir.path() / "point");
    boost::filesystem::create_directories(dir.path() / "comma");
    const std::string point = read_file(export_x3g((dir.path() / "point" / "cube.gcode").string()));
    REQUIRE_FALSE(point.empty());

    CommaNumericLocale comma;
    CNumericLocalesSetter restore;
    REQUIRE(comma.apply());
    const std::string in_comma_locale = read_file(export_x3g((dir.path() / "comma" / "cube.gcode").string()));
    CHECK(in_comma_locale.size() == point.size());
    // The binary file would make an unreadable report; where it differs is enough.
    CHECK(Test::common_prefix_length(point, in_comma_locale) == point.size());
}

// The printer profiles name their GPX machine. A printer without one, such as
// a user's own, gets it from the model name and the number of extruders: the
// models of the profiles land on the machine their profile names, and a
// second extruder makes a Replicator or a Thing-O-Matic the dual machine.
TEST_CASE("A legacy MakerBot without a GPX machine gets the one of its model and extruders", "[GPXExport]")
{
    const auto [model, extruders, machine] = GENERATE(table<std::string, int, std::string>({
        {"MakerBot Replicator Original", 1, "r1"},
        {"MakerBot Replicator Original Dual", 2, "r1d"},
        {"MakerBot Replicator 2", 1, "r2"},
        {"MakerBot Replicator 2X", 2, "r2x"},
        {"MakerBot Thing-O-Matic ABP", 1, "t7"},
        {"MakerBot Thing-O-Matic Acrylic", 1, "t7"},
        {"MakerBot Cupcake CNC", 1, "c3"},
        {"MakerBot Replicator Original", 2, "r1d"},
        {"MakerBot Thing-O-Matic ABP", 2, "t7d"},
    }));
    DYNAMIC_SECTION("printer_model " << model << ", " << extruders << " extruder(s)") {
        PrintConfig config;
        config.set_deserialize_strict({
            {"printer_model", model},
            {"nozzle_diameter", extruders == 2 ? "0.4,0.4" : "0.4"},
            {"gpx_machine_type", ""},
        });
        CHECK(GPXExport::gpx_machine_for_config(config) == machine);
    }
}

TEST_CASE("The GPX machine of the profile wins over the model name", "[GPXExport]")
{
    PrintConfig config;
    config.set_deserialize_strict({
        {"printer_model", "MakerBot Replicator 2X"},
        {"nozzle_diameter", "0.4,0.4"},
        {"gpx_machine_type", "r1d"},
    });
    CHECK(GPXExport::gpx_machine_for_config(config) == "r1d");
}

// GPX refuses to convert for a machine it does not know; the model name
// decides instead.
TEST_CASE("An unknown GPX machine falls back to the one of the model", "[GPXExport]")
{
    PrintConfig config;
    config.set_deserialize_strict({
        {"printer_model", "MakerBot Replicator 2X"},
        {"nozzle_diameter", "0.4,0.4"},
        {"gpx_machine_type", "r3"},
    });
    CHECK(GPXExport::gpx_machine_for_config(config) == "r2x");
}
