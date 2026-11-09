#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Format/UltimakerUFPExport.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <iterator>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A 20 mm cube at 0.4 mm per layer through a 0.6 mm nozzle. The first layer
// prints at 220 C on a 65 C plate, the others at 225 C on 60 C. The values
// read back from the files differ from the ones the export falls back to, so
// each of them names the setting it came from.
DynamicPrintConfig ultimaker_config(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"printable_area", "0x0,200x0,200x200,0x200"},
        {"nozzle_diameter", "0.6"},
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

// Slices the cube, in the middle of the 200 x 200 mm plate, into
// gcode_path. The export step gets the PrintConfig of the print, as in the
// export (BackgroundSlicingProcess).
void slice_cube(Print &print, Model &model, const DynamicPrintConfig &config, const std::string &gcode_path)
{
    TriangleMesh cube = Test::cube(20);
    cube.translate(90, 90, 0);
    Test::init_print(std::vector<TriangleMesh>{cube}, print, model, config, nullptr, /*arrange=*/false);
    print.process();
    print.export_gcode(gcode_path, nullptr, nullptr);
}

std::string read_file(const std::string &path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// The value of a ";KEY:value" line after the first line, empty when the key
// is missing.
std::string header_value(const std::string &header, const std::string &key)
{
    const std::string token = "\n;" + key + ":";
    const size_t      pos   = header.find(token);
    if (pos == std::string::npos)
        return {};
    const size_t begin = pos + token.size();
    return header.substr(begin, header.find('\n', begin) - begin);
}

// The number of a ";KEY:value" line. The printer reads it as a number, so
// "220" and "220.0" are the same value.
double header_number(const std::string &header, const std::string &key)
{
    const std::string value = header_value(header, key);
    INFO("header key " << key);
    REQUIRE_FALSE(value.empty());
    std::istringstream in(value);
    in.imbue(std::locale::classic());
    double number = 0.;
    in >> number;
    REQUIRE_FALSE(in.fail());
    return number;
}

// The Griffin header at the start of a .ufp package's G-code, without its
// end line.
std::string griffin_header(const std::map<std::string, std::string> &entries)
{
    REQUIRE(entries.count("3D/model.gcode") == 1);
    const std::string &model_gcode = entries.at("3D/model.gcode");
    REQUIRE(model_gcode.rfind(";START_OF_HEADER\n", 0) == 0);
    const size_t header_end = model_gcode.find(";END_OF_HEADER\n");
    REQUIRE(header_end != std::string::npos);
    return model_gcode.substr(0, header_end);
}

// What a printer of the flavor reads besides the G-code moves: for a .ufp
// package the Griffin header but its build date, and the slice metadata; for
// UltiGCode the head.
std::string export_step_result(const std::string &flavor, const std::string &gcode_path)
{
    Print print;
    Model model;
    if (flavor == "ultigcode") {
        slice_cube(print, model, ultimaker_config({{"gcode_flavor", flavor}, {"printer_model", "UltiMaker 2+"}}), gcode_path);
        REQUIRE(UltimakerUFPExport::add_ultigcode_header(gcode_path, print.config()));
        const std::string gcode = read_file(gcode_path);
        return gcode.substr(0, gcode.find("; HEADER_BLOCK_START"));
    }
    slice_cube(print, model, ultimaker_config({{"gcode_flavor", flavor}, {"printer_model", "UltiMaker S5"}}), gcode_path);
    const std::map<std::string, std::string> entries =
        Test::zip_entries(UltimakerUFPExport::pack_to_archive(gcode_path, print.config()));
    std::istringstream lines(griffin_header(entries));
    std::string        result;
    for (std::string line; std::getline(lines, line);)
        if (line.rfind(";GENERATOR.BUILD_DATE:", 0) != 0)
            result += line + "\n";
    REQUIRE(entries.count("Cura/slicemetadata.json") == 1);
    return result + entries.at("Cura/slicemetadata.json");
}

} // namespace

TEST_CASE("The UltiMaker flavors name the file type their export writes", "[UltimakerUFPExport]")
{
    CHECK(UltimakerUFPExport::get_archive_extension(gcfGriffin) == ".ufp");
    CHECK(UltimakerUFPExport::get_archive_extension(gcfCheetah) == ".ufp");
    // The UltiMaker 2 family reads plain G-code from its SD card.
    CHECK(UltimakerUFPExport::get_archive_extension(gcfUltiGCode) == ".gcode");
}

TEST_CASE("An S-line print becomes a .ufp package that starts with the Griffin header", "[UltimakerUFPExport]")
{
    const auto [flavor, machine] = GENERATE(table<std::string, std::string>({
        {"griffin", "UltiMaker S5"},
        {"cheetah", "UltiMaker S8"},
    }));
    DYNAMIC_SECTION("gcode_flavor " << flavor) {
        ScopedTemporaryDir dir;
        const std::string  gcode_path = (dir.path() / "cube.gcode").string();
        Print              print;
        Model              model;
        slice_cube(print, model, ultimaker_config({{"gcode_flavor", flavor}, {"printer_model", machine}}), gcode_path);
        const std::string archive = UltimakerUFPExport::pack_to_archive(gcode_path, print.config());

        REQUIRE(archive == (dir.path() / "cube.ufp").string());
        CHECK_FALSE(boost::filesystem::exists(gcode_path));
        const std::map<std::string, std::string> entries = Test::zip_entries(archive);
        for (const char *name : {"[Content_Types].xml", "_rels/.rels", "3D/model.gcode", "Cura/slicemetadata.json"}) {
            INFO("package part " << name);
            CHECK(entries.count(name) == 1);
        }
        REQUIRE(entries.count("Cura/slicemetadata.json") == 1);

        // The printer reads the header from the start of the G-code.
        const std::string header = griffin_header(entries);
        CHECK(header.size() < entries.at("3D/model.gcode").find("; HEADER_BLOCK_START"));
        CHECK(header_value(header, "HEADER_VERSION") == "0.1");
        // Cura's name for the Griffin dialect; the one for Cheetah is not
        // confirmed against a file of Cura's.
        if (flavor == "griffin")
            CHECK(header_value(header, "FLAVOR") == "Griffin");
        CHECK(header_value(header, "TARGET_MACHINE.NAME") == machine);
        CHECK(header_number(header, "PRINT.TIME") > 0.);
        // The plate's first layer temperature, the one the G-code heats it to.
        CHECK_THAT(header_number(header, "BUILD_PLATE.INITIAL_TEMPERATURE"), WithinAbs(65., 1e-6));
        CHECK_THAT(header_number(header, "EXTRUDER_TRAIN.0.INITIAL_TEMPERATURE"), WithinAbs(220., 1e-6));
        CHECK(header_number(header, "EXTRUDER_TRAIN.0.MATERIAL.VOLUME_USED") > 0.);
        CHECK_THAT(header_number(header, "EXTRUDER_TRAIN.0.NOZZLE.DIAMETER"), WithinAbs(0.6, 1e-6));

        const nlohmann::json slicemetadata = nlohmann::json::parse(entries.at("Cura/slicemetadata.json"));
        CHECK_THAT(slicemetadata.at("layer_height").get<double>(), WithinAbs(0.4, 1e-6));
    }
}

// More than one colon in a line makes UltiMaker printers crash, and the time
// of day in the generator line has two. The upload rewrites that line only in
// a plain G-code; a package goes out as the packer wrote it.
TEST_CASE("The generator line in a .ufp package carries no colon", "[UltimakerUFPExport]")
{
    ScopedTemporaryDir dir;
    const std::string  gcode_path = (dir.path() / "cube.gcode").string();
    Print              print;
    Model              model;
    slice_cube(print, model, ultimaker_config({{"gcode_flavor", "griffin"}, {"printer_model", "UltiMaker S5"}}), gcode_path);
    const std::string sliced = read_file(gcode_path);
    const std::map<std::string, std::string> entries =
        Test::zip_entries(UltimakerUFPExport::pack_to_archive(gcode_path, print.config()));
    REQUIRE(entries.count("3D/model.gcode") == 1);
    const std::string &model_gcode = entries.at("3D/model.gcode");

    const std::string prefix = "; generated by OrcaSlicer";
    const size_t      before = sliced.find(prefix);
    REQUIRE(before != std::string::npos);
    // The sliced G-code names the time with its colons; the package without.
    CHECK(sliced.find(':', before) < sliced.find('\n', before));
    const size_t start = model_gcode.find("\n" + prefix);
    REQUIRE(start != std::string::npos);
    const std::string line = model_gcode.substr(start + 1, model_gcode.find('\n', start + 1) - start - 1);
    INFO("generator line: " << line);
    CHECK(line.find(':') == std::string::npos);
    // The Griffin header keeps its own "KEY:value" lines.
    CHECK(header_number(griffin_header(entries), "PRINT.TIME") > 0.);
}

// The export dialog saves the G-code under the name of the package; packing
// it in place must not delete the package it has just written.
TEST_CASE("A G-code saved under the .ufp name becomes the package in place", "[UltimakerUFPExport]")
{
    ScopedTemporaryDir dir;
    const std::string  path = (dir.path() / "cube.ufp").string();
    Print              print;
    Model              model;
    slice_cube(print, model, ultimaker_config({{"gcode_flavor", "griffin"}, {"printer_model", "UltiMaker S5"}}), path);

    REQUIRE(UltimakerUFPExport::pack_to_archive(path, print.config()) == path);
    const std::map<std::string, std::string> entries = Test::zip_entries(path);
    REQUIRE(entries.count("3D/model.gcode") == 1);
    CHECK(entries.at("3D/model.gcode").rfind(";START_OF_HEADER\n", 0) == 0);
    CHECK_FALSE(boost::filesystem::exists(path + ".tmp_gcode_for_ufp"));
}

TEST_CASE("An UltiGCode print starts with the head the UltiMaker 2 firmware reads", "[UltimakerUFPExport]")
{
    ScopedTemporaryDir dir;
    const std::string  path = (dir.path() / "cube.gcode").string();
    Print              print;
    Model              model;
    slice_cube(print, model, ultimaker_config({{"gcode_flavor", "ultigcode"}, {"printer_model", "UltiMaker 2+"}}), path);

    REQUIRE(UltimakerUFPExport::add_ultigcode_header(path, print.config()));
    const std::string gcode = read_file(path);
    // The firmware takes the dialect from the first line.
    REQUIRE(gcode.rfind(";FLAVOR:UltiGCode\n", 0) == 0);
    const std::string head = gcode.substr(0, gcode.find("; HEADER_BLOCK_START"));
    // The firmware heats, primes and converts mm3 to mm only for an extruder
    // with at least 1 mm3 of material.
    CHECK(header_number(head, "MATERIAL") >= 1.);
    CHECK_THAT(header_number(head, "NOZZLE_DIAMETER"), WithinAbs(0.6, 1e-6));
    // The print screen shows this time.
    CHECK(header_number(head, "TIME") > 0.);

    // A second export step leaves the file as it is: one head only.
    CHECK(UltimakerUFPExport::add_ultigcode_header(path, print.config()));
    CHECK(read_file(path) == gcode);
}

TEST_CASE("Only the UltiGCode flavor gets the UltiGCode head", "[UltimakerUFPExport]")
{
    ScopedTemporaryDir dir;
    const std::string  path = (dir.path() / "cube.gcode").string();
    // Enough material for a head, were the flavor UltiGCode.
    const std::string gcode = "G1 X10 Y10 E1\n; filament used [cm3] = 2.00\n";
    {
        boost::nowide::ofstream f(path, std::ios::binary);
        f << gcode;
    }
    PrintConfig config;
    config.set_deserialize_strict("gcode_flavor", "griffin");

    CHECK_FALSE(UltimakerUFPExport::add_ultigcode_header(path, config));
    CHECK(read_file(path) == gcode);
}

// The G-code export pins its numeric locale to "C". The export step runs
// after it in the numeric locale of the calling thread -- in the GUI the
// user's, often with a decimal comma.
TEST_CASE("The UltiMaker export step in a locale with a decimal comma gives the same header", "[UltimakerUFPExport]")
{
    {
        CommaNumericLocale probe;
        CNumericLocalesSetter restore;
        if (! probe.apply())
            SKIP("no locale with a comma decimal separator is installed");
    }
    const std::string flavor = GENERATE("griffin", "ultigcode");
    DYNAMIC_SECTION("gcode_flavor " << flavor) {
        ScopedTemporaryDir dir;
        const std::string point = export_step_result(flavor, (dir.path() / "point.gcode").string());

        CommaNumericLocale comma;
        CNumericLocalesSetter restore;
        REQUIRE(comma.apply());
        const std::string in_comma_locale = export_step_result(flavor, (dir.path() / "comma.gcode").string());
        CHECK(in_comma_locale.size() == point.size());
        CHECK(Test::common_prefix_length(point, in_comma_locale) == point.size());
    }
}
