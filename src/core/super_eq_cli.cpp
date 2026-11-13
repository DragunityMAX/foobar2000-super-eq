#include "super_eq/dsp_engine.h"
#include "super_eq/preset_io.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static std::string read_all(const char* path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "super_eq_cli - foo_super_eq Core Inspection & Preset Converter\n"
                  << "Usage:\n"
                  << "  super_eq_cli inspect <preset.json|apo.txt> [sample_rate]\n"
                  << "  super_eq_cli apo2json <in.txt> <out.json>\n"
                  << "  super_eq_cli json2apo <in.json> <out.txt>\n";
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "inspect" && argc >= 3) {
        double sr = (argc >= 4) ? std::stod(argv[3]) : 44100.0;
        std::string text = read_all(argv[2]);
        if (text.empty()) {
            std::cerr << "Failed to read file: " << argv[2] << "\n";
            return 1;
        }
        super_eq::EQPresetState st;
        if (!super_eq::PresetIO::from_json(text, st) &&
            !super_eq::PresetIO::import_apo_text(text, st)) {
            std::cerr << "Unrecognized preset format.\n";
            return 1;
        }
        std::cout << "Preset: " << st.name << " (" << st.bands.size() << " bands)\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Freq(Hz)    Left(dB)   Right(dB)   Mid(dB)    Side(dB)\n";
        const double freqs[] = {31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
        for (double f : freqs) {
            double l = super_eq::SuperEQEngine::eval_channel_magnitude_db(st, super_eq::ChannelRoute::Left, f, sr, true);
            double r = super_eq::SuperEQEngine::eval_channel_magnitude_db(st, super_eq::ChannelRoute::Right, f, sr, true);
            double m = super_eq::SuperEQEngine::eval_channel_magnitude_db(st, super_eq::ChannelRoute::Mid, f, sr, false);
            double s = super_eq::SuperEQEngine::eval_channel_magnitude_db(st, super_eq::ChannelRoute::Side, f, sr, false);
            std::cout << std::setw(8) << f << "   "
                      << std::setw(8) << l << "   "
                      << std::setw(8) << r << "   "
                      << std::setw(8) << m << "   "
                      << std::setw(8) << s << "\n";
        }
        return 0;
    }

    if (cmd == "apo2json" && argc >= 4) {
        super_eq::EQPresetState st;
        if (!super_eq::PresetIO::import_apo_text(read_all(argv[2]), st)) {
            std::cerr << "Failed to parse APO file.\n";
            return 1;
        }
        std::ofstream ofs(argv[3]);
        ofs << super_eq::PresetIO::to_json(st);
        return ofs ? 0 : 1;
    }

    if (cmd == "json2apo" && argc >= 4) {
        super_eq::EQPresetState st;
        if (!super_eq::PresetIO::from_json(read_all(argv[2]), st)) {
            std::cerr << "Failed to parse JSON file.\n";
            return 1;
        }
        std::ofstream ofs(argv[3]);
        ofs << super_eq::PresetIO::export_apo_text(st);
        return ofs ? 0 : 1;
    }

    std::cerr << "Invalid arguments.\n";
    return 1;
}
