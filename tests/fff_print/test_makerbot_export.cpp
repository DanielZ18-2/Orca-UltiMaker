#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <boost/filesystem/operations.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Format/MakerBotExport.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A 20 mm cube at 0.4 mm per layer is 50 layers. The first layer prints at
// 220 C on a 65 C plate, the others at 225 C on 60 C. None of these is a value
// the export falls back to, so each one read back from the archive names the
// setting it came from.
DynamicPrintConfig makerbot_config(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"printable_area", "0x0,200x0,200x200,0x200"},
        {"layer_height", 0.4},
        {"initial_layer_print_height", 0.4},
        {"nozzle_temperature_initial_layer", "220"},
        {"nozzle_temperature", "225"},
        {"curr_bed_type", "High Temp Plate"},
        {"hot_plate_temp_initial_layer", "65"},
        {"hot_plate_temp", "60"},
    });
    config.set_deserialize_strict(items);
    return config;
}

// Slices the cube, in the middle of the 200 x 200 mm plate, into gcode_path
// and packs it the way the export does (BackgroundSlicingProcess): the
// archive takes the place of the G-code.
std::string pack_sliced_cube(const DynamicPrintConfig &config, const std::string &gcode_path)
{
    TriangleMesh cube = Test::cube(20);
    cube.translate(90, 90, 0);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{cube}, print, model, config, nullptr, /*arrange=*/false);
    print.process();
    print.export_gcode(gcode_path, nullptr, nullptr);
    return MakerBotExport::pack_to_archive(gcode_path, print.config());
}

nlohmann::json meta_json(const std::map<std::string, std::string> &entries)
{
    REQUIRE(entries.count("meta.json") == 1);
    return nlohmann::json::parse(entries.at("meta.json"));
}

} // namespace

TEST_CASE("The MakerBot flavors name the file type their export writes", "[MakerBotExport]")
{
    CHECK(MakerBotExport::get_archive_extension(gcfMakerBotBirdwing) == ".makerbot");
    CHECK(MakerBotExport::get_archive_extension(gcfMakerBotLava) == ".makerbot");
    // Legacy machines take .x3g, written by GPXExport from the plain G-code.
    CHECK(MakerBotExport::get_archive_extension(gcfMakerBotLegacy) == ".gcode");
}

TEST_CASE("A Birdwing print becomes a .makerbot archive with a toolpath and its metadata", "[MakerBotExport]")
{
    ScopedTemporaryDir dir;
    const std::string gcode_path = (dir.path() / "cube.gcode").string();
    const std::string archive    = pack_sliced_cube(makerbot_config({
        {"gcode_flavor", "makerbot_birdwing"},
        {"makerbot_bot_type", "z18_6"},
    }), gcode_path);

    REQUIRE(archive == (dir.path() / "cube.makerbot").string());
    CHECK_FALSE(boost::filesystem::exists(gcode_path));
    const std::map<std::string, std::string> entries = Test::zip_entries(archive);
    REQUIRE(entries.count("print.jsontoolpath") == 1);

    const nlohmann::json meta = meta_json(entries);
    CHECK(meta.at("version").get<std::string>() == "1.2.0");
    CHECK(meta.at("bot_type").get<std::string>() == "z18_6");
    // The print temperature, as MakerBot Print writes it, not the first layer's.
    CHECK(meta.at("extruder_temperature").get<int>() == 225);
    // The temperature of the plate curr_bed_type names, for the layers after the first.
    CHECK(meta.at("platform_temperature").get<int>() == 60);
    CHECK(meta.at("num_z_layers").get<int>() == 50);
    // The G-code has its origin in the corner of the plate, the printer in the
    // middle: the cube in the middle of the plate lies around 0, 0.
    const nlohmann::json &box = meta.at("bounding_box");
    CHECK_THAT(box.at("x_min").get<double>() + box.at("x_max").get<double>(), WithinAbs(0., 1.));
    CHECK_THAT(box.at("y_min").get<double>() + box.at("y_max").get<double>(), WithinAbs(0., 1.));

    const nlohmann::json toolpath = nlohmann::json::parse(entries.at("print.jsontoolpath"));
    REQUIRE(toolpath.is_array());
    CHECK(meta.at("total_commands").get<size_t>() == toolpath.size());
    size_t           moves = 0;
    std::vector<int> temperatures;
    for (const nlohmann::json &entry : toolpath) {
        const nlohmann::json &command  = entry.at("command");
        const std::string     function = command.value("function", std::string());
        if (function == "move")
            ++moves;
        else if (function == "set_toolhead_temperature")
            temperatures.push_back(command.at("parameters").at("temperature").get<int>());
    }
    CHECK(moves > 0);
    // The first layer heats to its temperature, the layers after it to theirs.
    REQUIRE_FALSE(temperatures.empty());
    CHECK(temperatures.front() == 220);
    CHECK(std::find(temperatures.begin(), temperatures.end(), 225) != temperatures.end());
}

// bot_type tells the printer which machine the file is for. replicator_b,
// not the fallback z18_6, shows that it comes from the profile. MakerBot
// Print 4 and later write metadata the Birdwing printers no longer show
// correctly on their own screen; the fields below are what makes the print
// show up there.
TEST_CASE("The Birdwing metadata names the printer type and keeps the fields its screen shows", "[MakerBotExport]")
{
    ScopedTemporaryDir dir;
    const std::string archive = pack_sliced_cube(makerbot_config({
        {"gcode_flavor", "makerbot_birdwing"},
        {"makerbot_bot_type", "replicator_b"},
        {"printer_model", "MakerBot Replicator+"},
    }), (dir.path() / "cube.gcode").string());

    const nlohmann::json meta = meta_json(Test::zip_entries(archive));
    CHECK(meta.at("bot_type").get<std::string>() == "replicator_b");
    for (const char *key : {"printer_settings", "name", "uses_raft", "num_tool_changes",
                            "extrusion_distance_a_mm", "extrusion_mass_a_grams"}) {
        INFO("meta.json key " << key);
        CHECK(meta.contains(key));
    }
    CHECK(meta.at("name").get<std::string>() == "cube");
}

TEST_CASE("The platform temperature in the metadata is the one of the plate the print uses", "[MakerBotExport]")
{
    const auto [plate, expected] = GENERATE(table<std::string, int>({
        {"Cool Plate", 35},
        {"Engineering Plate", 45},
        {"High Temp Plate", 60},
        {"Textured PEI Plate", 70},
        {"Textured Cool Plate", 40},
        {"Supertack Plate", 50},
    }));
    DYNAMIC_SECTION("curr_bed_type " << plate) {
        ScopedTemporaryDir dir;
        const std::string archive = pack_sliced_cube(makerbot_config({
            {"gcode_flavor", "makerbot_birdwing"},
            {"makerbot_bot_type", "z18_6"},
            {"cool_plate_temp", "35"},
            {"eng_plate_temp", "45"},
            {"hot_plate_temp", "60"},
            {"textured_plate_temp", "70"},
            {"textured_cool_plate_temp", "40"},
            {"supertack_plate_temp", "50"},
            {"curr_bed_type", plate},
        }), (dir.path() / "cube.gcode").string());

        CHECK(meta_json(Test::zip_entries(archive)).at("platform_temperature").get<int>() == expected);
    }
}

TEST_CASE("A Method print becomes a .makerbot archive with a toolpath", "[MakerBotExport]")
{
    ScopedTemporaryDir dir;
    const std::string archive = pack_sliced_cube(makerbot_config({
        {"gcode_flavor", "makerbot_lava"},
        {"makerbot_bot_type", "lava_f"},
        {"printer_model", "MakerBot Method X"},
    }), (dir.path() / "cube.gcode").string());

    REQUIRE(archive == (dir.path() / "cube.makerbot").string());
    const std::map<std::string, std::string> entries = Test::zip_entries(archive);
    CHECK(entries.count("print.jsontoolpath") == 1);
    CHECK(entries.count("print.gcode") == 0);
    const nlohmann::json meta = meta_json(entries);
    CHECK(meta.at("version").get<std::string>() == "3.0.0");
    CHECK(meta.at("bot_type").get<std::string>() == "lava_f");
    CHECK(meta.at("extruder_temperature").get<int>() == 225);
}

TEST_CASE("A Sketch print carries its G-code in the .makerbot archive", "[MakerBotExport]")
{
    ScopedTemporaryDir dir;
    const std::string archive = pack_sliced_cube(makerbot_config({
        {"gcode_flavor", "makerbot_lava"},
        {"makerbot_bot_type", "sketch"},
        {"printer_model", "MakerBot Sketch"},
    }), (dir.path() / "cube.gcode").string());

    const std::map<std::string, std::string> entries = Test::zip_entries(archive);
    REQUIRE(entries.count("print.gcode") == 1);
    CHECK(entries.count("print.jsontoolpath") == 0);
    const nlohmann::json meta = meta_json(entries);
    CHECK(meta.at("version").get<std::string>() == "3.0.0");
    CHECK(meta.at("bot_type").get<std::string>() == "sketch");

    // The Sketch runs print.gcode as it is, with its origin in the middle of
    // the plate: the cube in the middle of the plate lies around 0, 0.
    // Relative E: a positive E on a move in X or Y extrudes.
    double      min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    GCodeReader reader;
    reader.parse_buffer(entries.at("print.gcode"), [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.cmd_is("G1") && line.has_e() && line.e() > 0.f && (line.has_x() || line.has_y())) {
            min_x = std::min(min_x, double(line.new_X(self)));
            max_x = std::max(max_x, double(line.new_X(self)));
            min_y = std::min(min_y, double(line.new_Y(self)));
            max_y = std::max(max_y, double(line.new_Y(self)));
        }
    });
    REQUIRE(max_x > min_x);
    CHECK_THAT(min_x + max_x, WithinAbs(0., 1.));
    CHECK_THAT(min_y + max_y, WithinAbs(0., 1.));
}

// The export dialog saves the G-code under the name of the archive; packing
// it in place must not delete the archive it has just written.
TEST_CASE("A G-code saved under the .makerbot name becomes the archive in place", "[MakerBotExport]")
{
    ScopedTemporaryDir dir;
    const std::string path    = (dir.path() / "cube.makerbot").string();
    const std::string archive = pack_sliced_cube(makerbot_config({
        {"gcode_flavor", "makerbot_birdwing"},
        {"makerbot_bot_type", "z18_6"},
    }), path);

    REQUIRE(archive == path);
    const std::map<std::string, std::string> entries = Test::zip_entries(path);
    CHECK(entries.count("meta.json") == 1);
    CHECK(entries.count("print.jsontoolpath") == 1);
    CHECK_FALSE(boost::filesystem::exists(path + ".tmp_gcode_for_pack"));
}

// The G-code export pins its numeric locale to "C". Packing runs after it in
// the numeric locale of the calling thread -- in the GUI the user's, often
// with a decimal comma.
TEST_CASE("A .makerbot archive packed in a locale with a decimal comma is the same", "[MakerBotExport]")
{
    {
        CommaNumericLocale probe;
        CNumericLocalesSetter restore;
        if (! probe.apply())
            SKIP("no locale with a comma decimal separator is installed");
    }
    const DynamicPrintConfig config = makerbot_config({
        {"gcode_flavor", "makerbot_birdwing"},
        {"makerbot_bot_type", "z18_6"},
    });
    ScopedTemporaryDir dir;
    boost::filesystem::create_directories(dir.path() / "point");
    boost::filesystem::create_directories(dir.path() / "comma");
    const std::map<std::string, std::string> point =
        Test::zip_entries(pack_sliced_cube(config, (dir.path() / "point" / "cube.gcode").string()));

    CommaNumericLocale comma;
    CNumericLocalesSetter restore;
    REQUIRE(comma.apply());
    const std::map<std::string, std::string> in_comma_locale =
        Test::zip_entries(pack_sliced_cube(config, (dir.path() / "comma" / "cube.gcode").string()));

    for (const char *name : {"meta.json", "print.jsontoolpath"}) {
        INFO("archive entry " << name);
        REQUIRE(point.count(name) == 1);
        REQUIRE(in_comma_locale.count(name) == 1);
        const std::string &expected = point.at(name);
        CHECK(in_comma_locale.at(name).size() == expected.size());
        CHECK(Test::common_prefix_length(expected, in_comma_locale.at(name)) == expected.size());
    }
}
