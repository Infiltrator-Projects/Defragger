// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "cpp runtime test failed: %s\n", message);
    return false;
}

} // namespace

int main() {
    using namespace defragger;
    bool ok = true;

    const auto& registry = backend_registry();
    ok = check(registry.size() == 17U, "registry size") && ok;

    const BackendInfo* ext = backend_by_fstype("EXT2");
    ok = check(ext != nullptr, "EXT2 alias lookup") && ok;
    if (ext != nullptr) {
        ok = check(ext->id == "ext4", "EXT2 maps to ext4") && ok;
        ok = check((ext->capabilities & CAP_DEFRAG) != 0U,
                   "ext4 advertises defrag") && ok;
        ok = check(operation_for(*ext, "growth-defrag") != nullptr,
                   "ext4 growth-defrag operation") && ok;
    }

    const BackendInfo* vfat = backend_by_fstype("vfat");
    ok = check(vfat != nullptr, "vfat alias lookup") && ok;
    if (vfat != nullptr)
        ok = check(vfat->id == "fat32", "vfat maps to fat32") && ok;

    const BackendInfo* apfs = backend_by_fstype("apfs");
    ok = check(apfs != nullptr, "APFS lookup") && ok;
    if (apfs != nullptr)
        ok = check(apfs->operations.empty(), "APFS remains read-only") && ok;

    const std::vector<std::string> input{
        "--keep", "a", "--drop", "value", "--drop=other", "--tail"};
    const std::vector<std::string> blocked{"--drop"};
    const auto output = without_options(input, blocked);
    const std::vector<std::string> expected{"--keep", "a", "--tail"};
    ok = check(output == expected, "unsupported option filtering") && ok;

    const std::string manifest = registry_manifest_json();
    ok = check(manifest.find("\"schema\":3") != std::string::npos,
               "manifest schema") && ok;
    ok = check(manifest.find("\"id\":\"ntfs\"") != std::string::npos,
               "manifest NTFS entry") && ok;
    ok = check(manifest.find("\"id\":\"hfsplus\"") != std::string::npos,
               "manifest HFS+ entry") && ok;

    return ok ? 0 : 1;
}
