// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"

#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace defragger;

    const auto& registry = backend_registry();
    assert(registry.size() == 17U);

    const BackendInfo* ext = backend_by_fstype("EXT2");
    assert(ext != nullptr);
    assert(ext->id == "ext4");
    assert((ext->capabilities & CAP_DEFRAG) != 0U);
    assert(operation_for(*ext, "growth-defrag") != nullptr);

    const BackendInfo* vfat = backend_by_fstype("vfat");
    assert(vfat != nullptr);
    assert(vfat->id == "fat32");

    const BackendInfo* apfs = backend_by_fstype("apfs");
    assert(apfs != nullptr);
    assert(apfs->operations.empty());

    const std::vector<std::string> input{
        "--keep", "a", "--drop", "value", "--drop=other", "--tail"};
    const std::vector<std::string> blocked{"--drop"};
    const auto output = without_options(input, blocked);
    const std::vector<std::string> expected{"--keep", "a", "--tail"};
    assert(output == expected);

    const std::string manifest = registry_manifest_json();
    assert(manifest.find("\"schema\":3") != std::string::npos);
    assert(manifest.find("\"id\":\"ntfs\"") != std::string::npos);
    assert(manifest.find("\"id\":\"hfsplus\"") != std::string::npos);
    return 0;
}
