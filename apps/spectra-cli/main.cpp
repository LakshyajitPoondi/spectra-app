// SPDX-License-Identifier: GPL-3.0-or-later
// spectra-cli: headless tools for the CPU reference pipeline.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include "parity.hpp"
#include "spectra/core/npy.hpp"
#include "spectra/core/parallel.hpp"
#include "spectra/pipeline/pipeline.hpp"

using namespace spectra;
using nlohmann::json;

namespace {

std::string default_pack_path(const char* argv0) {
    if (const char* env = std::getenv("SPECTRA_PACK")) return env;
    namespace fs = std::filesystem;
    for (fs::path base : {fs::current_path(), fs::absolute(argv0).parent_path()})
        for (int up = 0; up < 5; ++up) {
            fs::path p = base / "data" / "spektrafilm.sfpk";
            if (fs::exists(p)) return p.string();
            if (!base.has_parent_path()) break;
            base = base.parent_path();
        }
    return "data/spektrafilm.sfpk";
}

int usage() {
    std::puts(
        "spectra-cli <command> [options]\n"
        "  info                         show data-pack provenance and the profile list\n"
        "  pack-verify                  check every array of the data pack against its checksum\n"
        "  render  --in a.npy --out b.npy [--film F] [--print P] [--params p.json]\n"
        "          [--post p.json] [--options o.json] [--inject tap] [--collect tap]\n"
        "  digest  [--film F] [--print P] [--params p.json]     print the digested parameters\n"
        "  parity  cases.json [--filter s] [--report r.json] [--dump dir] [-v]\n"
        "common options: --pack path/to/spektrafilm.sfpk   --threads N");
    return 2;
}

json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return json::parse(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    std::map<std::string, std::string> opt;
    std::vector<std::string> positional;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v") verbose = true;
        else if (a.rfind("--", 0) == 0 && i + 1 < argc) opt[a.substr(2)] = argv[++i];
        else positional.push_back(a);
    }
    try {
        if (opt.count("threads")) thread_count() = std::max(1, std::stoi(opt["threads"]));
        std::string pack_path = opt.count("pack") ? opt["pack"] : default_pack_path(argv[0]);
        auto pack = Pack::load(pack_path);
        if (cmd == "pack-verify") {
            auto bad = pack->verify();
            for (auto& n : bad) std::printf("CORRUPT %s\n", n.c_str());
            std::printf("%s: %zu arrays, %zu corrupt\n", pack_path.c_str(), pack->array_names().size(), bad.size());
            return bad.empty() ? 0 : 1;
        }
        if (cmd == "info") {
            const json& prov = pack->meta().at("provenance");
            std::printf("data pack: %s\n", pack_path.c_str());
            for (auto it = prov.begin(); it != prov.end(); ++it)
                std::printf("  %-26s %s\n", it.key().c_str(), it.value().dump().c_str());
            std::printf("profiles:\n");
            for (auto& s : list_profiles(*pack)) {
                Profile p = load_profile(*pack, s);
                std::printf("  %-34s %-8s %-5s %-5s %s\n", s.c_str(), p.type.c_str(), p.support.c_str(),
                            p.channel_model.c_str(), p.stage.c_str());
            }
            return 0;
        }
        auto colour = std::make_shared<const ColourData>(pack);
        if (cmd == "parity") {
            if (positional.empty()) return usage();
            ParityOptions po;
            po.cases_path = positional[0];
            po.filter = opt.count("filter") ? opt["filter"] : "";
            po.report_path = opt.count("report") ? opt["report"] : "";
            po.dump_dir = opt.count("dump") ? opt["dump"] : "";
            po.verbose = verbose;
            return run_parity(colour, po) == 0 ? 0 : 1;
        }
        if (cmd == "render" || cmd == "digest") {
            Params p;
            if (opt.count("film")) p.film = opt["film"];
            if (opt.count("print")) p.print = opt["print"];
            if (opt.count("params")) apply_params_patch(p, read_json(opt["params"]));
            Params d = digest_params(*pack, p);
            if (opt.count("post")) apply_params_patch(d, read_json(opt["post"]));
            if (cmd == "digest") {
                std::cout << params_to_json(d).dump(1) << "\n";
                return 0;
            }
            RenderOptions ro;
            if (opt.count("options")) apply_render_options_patch(ro, read_json(opt["options"]));
            if (!opt.count("in") || !opt.count("out")) return usage();
            Image in = npy_load_image(opt["in"]);
            Pipeline pipe(colour, d, ro);
            Image out = pipe.process(in, tap_from_string(opt.count("inject") ? opt["inject"] : "rgb_in"),
                                     tap_from_string(opt.count("collect") ? opt["collect"] : "rgb_out"));
            npy_save_image(opt["out"], out);
            std::printf("rendered %dx%dx%d -> %s\n", out.w, out.h, out.c, opt["out"].c_str());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
