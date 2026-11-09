#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/GPXExport.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <iterator>
#include <map>
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

// How often the bytes of an x3g command occur in a file.
size_t count_of(const std::string &bytes, const std::string &command)
{
    size_t count = 0;
    for (size_t pos = bytes.find(command); pos != std::string::npos; pos = bytes.find(command, pos + 1))
        ++count;
    return count;
}

// The x3g command that runs (1) or stops (0) the belt of the Automated Build
// Platform: tool action 136, tool 0, action 27 with one payload byte. GPX
// writes it for M106 and M107 on the Cupcake and the Thing-O-Matic.
std::string abp_belt(bool on)
{
    return std::string{char(136), char(0), char(27), char(1), char(on ? 1 : 0)};
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
// "TOM" counts as a word of its own: a "Custom Cupcake" stays a Cupcake.
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
        {"MakerBot TOM", 1, "t7"},
        {"Custom Cupcake", 1, "c3"},
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

// The Thing-O-Matic's Automated Build Platform is a belt over the heated
// platform that carries a finished part off it. The end G-code of the shipped
// ABP preset cools the platform to 5 C below the print temperature, runs the
// belt for 14 s and stops it. Nothing else in the print touches the belt: the
// part fan of the legacy flavor is M126/M127, which GPX leaves out for these
// machines.
TEST_CASE("A Thing-O-Matic ABP print ends by running the belt once", "[GPXExport]")
{
    const std::string preset = std::string(PROFILES_DIR) +
                               "/MakerBot/machine/Legacy/MakerBot Thing-O-Matic ABP 0.4 nozzle.json";
    if (!boost::filesystem::exists(preset))
        SKIP("shipped profile not present in this checkout: " << preset);
    DynamicPrintConfig                 shipped;
    ConfigSubstitutionContext          substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    shipped.load_from_json(preset, substitutions, false, key_values, reason);
    INFO("profile: " << preset << (reason.empty() ? "" : ("  load reason: " + reason)));
    REQUIRE(shipped.has("machine_end_gcode"));
    REQUIRE(shipped.has("machine_start_gcode"));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", "makerbot_legacy"},
        {"printer_model", "MakerBot Thing-O-Matic ABP"},
        {"gpx_machine_type", "t7"},
        {"printable_area", "0x0,106x0,106x120,0x120"},
        {"printable_height", 106},
        {"nozzle_diameter", "0.4"},
        {"layer_height", 0.4},
        {"initial_layer_print_height", 0.4},
        {"nozzle_temperature_initial_layer", "230"},
        {"nozzle_temperature", "230"},
        {"curr_bed_type", "High Temp Plate"},
        {"hot_plate_temp_initial_layer", "100"},
        {"hot_plate_temp", "100"},
    });
    // Fans the Thing-O-Matic has not: with an auxiliary fan Orca writes
    // M106 P2 at the start, during the print and before the end G-code, with
    // air filtration M106 P3 after it. GPX would start the belt with each of
    // them, so the export comments them out.
    static constexpr int no_extra_fan = 0, auxiliary = 1, exhaust = 2;
    const int            fans = GENERATE(no_extra_fan, auxiliary, exhaust);
    CAPTURE(fans);
    config.set_deserialize_strict({
        {"auxiliary_fan", fans == auxiliary ? "1" : "0"},
        {"additional_cooling_fan_speed", "70"},
        {"support_air_filtration", fans == exhaust ? "1" : "0"},
        {"activate_air_filtration", fans == exhaust ? "1" : "0"},
        {"activate_air_filtration_on_completion", fans == exhaust ? "1" : "0"},
        {"complete_print_exhaust_fan_speed", "70"},
    });
    config.set_key_value("machine_start_gcode", new ConfigOptionString(shipped.opt_string("machine_start_gcode")));
    config.set_key_value("machine_end_gcode", new ConfigOptionString(shipped.opt_string("machine_end_gcode")));

    ScopedEnvVar no_ini("ORCA_GPX_INI", "");
    ScopedEnvVar no_kept_gcode("ORCA_GPX_KEEP_GCODE", "");
    ScopedTemporaryDir dir;
    const std::string  gcode_path = (dir.path() / "cube.gcode").string();
    TriangleMesh       cube       = Test::cube(20);
    cube.translate(43, 50, 0);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{cube}, print, model, config, nullptr, /*arrange=*/false);
    print.process();
    print.export_gcode(gcode_path, nullptr, nullptr);
    // The G-code carries the fan commands, or the case checks nothing.
    const std::string gcode = read_file(gcode_path);
    if (fans == auxiliary)
        REQUIRE(gcode.find("M106 P2") != std::string::npos);
    if (fans == exhaust)
        REQUIRE(gcode.find("M106 P3") != std::string::npos);
    const std::string x3g = GPXExport::pack_to_archive(gcode_path, print.config());
    REQUIRE_FALSE(x3g.empty());
    const std::string bytes = read_file(x3g);

    CHECK(count_of(bytes, abp_belt(true)) == 1);
    CHECK(count_of(bytes, abp_belt(false)) == 1);
    const size_t on  = bytes.find(abp_belt(true));
    const size_t off = bytes.find(abp_belt(false));
    REQUIRE(on != std::string::npos);
    REQUIRE(off != std::string::npos);
    // The platform target drops to 95 C before the belt runs: tool action 136,
    // tool 0, action 31 with the temperature in two bytes.
    const std::string platform_95{char(136), char(0), char(31), char(2), char(95), char(0)};
    const size_t      cooled = bytes.find(platform_95);
    REQUIRE(cooled != std::string::npos);
    CHECK(cooled < on);
    // Then the bot waits for the platform: command 141, tool 0, a query
    // every 100 ms and the longest timeout GPX writes (0xffff s).
    const std::string wait_platform{char(141), char(0), char(100), char(0), char(0xff), char(0xff)};
    const size_t      waits = bytes.find(wait_platform, cooled);
    REQUIRE(waits != std::string::npos);
    CHECK(waits < on);
    // Between the two the bot waits 14000 ms: command 133 with four bytes.
    const std::string wait_14s{char(133), char(0xb0), char(0x36), char(0), char(0)};
    const size_t      waited = bytes.find(wait_14s, on);
    REQUIRE(waited != std::string::npos);
    CHECK(waited < off);
}
