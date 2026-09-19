// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"
#include "map.hpp"
#include "runtime.hpp"

extern "C" {
#include "version.h"
}

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

int usage() {
    std::fputs(
        "usage: linux-defragger-mapper-cpp "
        "[PATH] [--fstype TYPE] [--cells N] [--probe] [--list-backends]\n",
        stderr);
    return 2;
}

defragger::Json probe_result(const defragger::BackendInfo& backend) {
    using defragger::Json;
    Json manifest = Json::parse(defragger::registry_manifest_json(2U));
    const Json& backends = manifest.at("backends");
    Json operations(Json::Array{});
    for (const auto& entry : backends.array()) {
        if (entry.at("id").string_or() == backend.id) {
            operations = entry.at("operations");
            break;
        }
    }
    Json::Object out;
    out["filesystem"] = Json(backend.id);
    out["capabilities"] = Json::unsigned_integer(backend.capabilities);
    out["map_accuracy"] = Json(backend.map_accuracy);
    out["operations"] = std::move(operations);
    return Json(std::move(out));
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string filesystem;
    std::size_t cells = 4096U;
    bool list = false;
    bool probe = false;

    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--version") {
            std::printf("linux-defragger-mapper-cpp %s\n", LD_VERSION);
            return 0;
        }
        if (token == "--list-backends") {
            list = true;
            continue;
        }
        if (token == "--probe") {
            probe = true;
            continue;
        }
        if (token == "--fstype") {
            if (++index >= argc) return usage();
            filesystem = argv[index];
            continue;
        }
        if (token == "--cells") {
            if (++index >= argc) return usage();
            try {
                const unsigned long long value = std::stoull(argv[index]);
                cells = static_cast<std::size_t>(
                    std::min<unsigned long long>(
                        value,
                        static_cast<unsigned long long>(
                            std::numeric_limits<std::size_t>::max())));
            } catch (...) {
                return usage();
            }
            continue;
        }
        if (!token.empty() && token[0] == '-') return usage();
        if (!path.empty()) return usage();
        path = token;
    }

    if (list) {
        std::puts(defragger::registry_manifest_json(2U).c_str());
        return 0;
    }
    if (path.empty()) return usage();

    try {
        const defragger::BackendInfo* backend = nullptr;
        if (!filesystem.empty()) {
            backend = defragger::backend_by_fstype(filesystem);
        } else {
            for (const auto& candidate : defragger::backend_registry()) {
                if (defragger::backend_probe(candidate, path)) {
                    backend = &candidate;
                    break;
                }
            }
        }
        if (backend == nullptr) {
            std::fputs(
                "No self-contained filesystem plugin recognised this volume.\n",
                stderr);
            return 2;
        }
        if (probe) {
            std::puts(probe_result(*backend).dump().c_str());
            return 0;
        }
        defragger::Json result =
            defragger::map_backend(*backend, path, std::max<std::size_t>(1U, cells));
        if (!result.is_object())
            throw std::runtime_error("allocation mapper produced non-object result");
        result.object()["capabilities"] =
            defragger::Json::unsigned_integer(backend->capabilities);
        result.object()["backend_id"] = defragger::Json(backend->id);
        std::puts(result.dump().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Defragmenter mapper: %s\n", error.what());
        return 1;
    }
}
