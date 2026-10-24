#include "../native/manifest.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Supply an isolated test directory");
        auto root = ark::fs::absolute(argv[1]);
        if (ark::fs::exists(root)) throw std::runtime_error("Test directory must be new");
        auto game = root / L"game", output = root / L"output";
        ark::fs::create_directories(game / L"Arknights_Data/skinpack"); ark::fs::create_directories(output);
        std::ofstream(game / L"Arknights.exe") << "synthetic fixture, never executed";
        auto original = game / L"Arknights_Data/skinpack/test.ab", replacement = output / L"test.ab";
        std::ofstream(original) << "original"; std::ofstream(replacement) << "replacement";
        auto file = output / L"manifest.tsv";
        auto write = [&](const std::string& destination, const std::string& hash) { std::ofstream(file, std::ios::binary | std::ios::trunc) << "QAQR-ARKNIGHTS-BUNDLES\t1\nGAME\t" << ark::utf8((game / L"Arknights.exe").wstring()) << "\nBUNDLE\tArknights_Data/skinpack/test.ab\t" << destination << "\t" << ark::sha256(original) << "\t" << hash << "\n"; };
        write("test.ab", ark::sha256(replacement));
        auto manifest = ark::readManifest(file, true);
        if (manifest.entries.size() != 1 || !ark::within(manifest.entries[0].replacement, output)) throw std::runtime_error("Valid manifest rejected");
        auto refuses = [&]() { try { ark::readManifest(file, true); return false; } catch (...) { return true; } };
        write("../output/test.ab", ark::sha256(replacement)); if (!refuses()) throw std::runtime_error("Traversal was accepted");
        write("test.ab", std::string(64, '0')); if (!refuses()) throw std::runtime_error("Changed output was accepted");
        write("test.ab", ark::sha256(replacement)); std::ofstream(original) << "game update"; if (!refuses()) throw std::runtime_error("Changed game base was accepted");
        if (ark::within(root / L"output-other/file.ab", output)) throw std::runtime_error("Prefix sibling accepted");
        {
            std::ofstream bulk(file, std::ios::binary | std::ios::trunc);
            bulk << "QAQR-ARKNIGHTS-BUNDLES\t1\nGAME\t" << ark::utf8((game / L"Arknights.exe").wstring()) << "\n";
            for (int index = 0; index < 201; ++index) {
                auto name = "bulk-" + std::to_string(index) + ".ab";
                auto input = game / L"Arknights_Data/skinpack" / name;
                std::ofstream(input) << "fixture";
                bulk << "BUNDLE\tArknights_Data/skinpack/" << name << "\ttest.ab\t" << ark::sha256(input) << "\t" << ark::sha256(replacement) << "\n";
            }
        }
        if (ark::readManifest(file, true).entries.size() != 201) throw std::runtime_error("Large valid manifest rejected");
        std::cout << "Manifest boundaries, source/replacement hashes, and relative paths verified; no game executed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what(); return 1; }
}
