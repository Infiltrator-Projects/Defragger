// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "helper_policy.hpp"
#include "json.hpp"
#include "process.hpp"

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

    const Json parsed = Json::parse(
        R"({"text":"A\nB","number":1234567890123,"truth":true,"array":[1,null]})");
    ok = check(parsed.at("text").string() == "A\nB", "JSON string escape") && ok;
    ok = check(parsed.at("number").unsigned_value() == 1234567890123ULL,
               "JSON exact integer") && ok;
    ok = check(Json::parse(parsed.dump()).at("truth").boolean(),
               "JSON round trip") && ok;

    const CommandResult command =
        run_capture({"/bin/sh", "-c", "printf native-cpp; printf warning >&2"});
    ok = check(command.return_code == 0, "process exit code") && ok;
    ok = check(command.standard_output == "native-cpp", "process stdout") && ok;
    ok = check(command.standard_error == "warning", "process stderr") && ok;

    const HelperCommand helper = helper_command(
        "operation-engine",
        {"defrag", "/dev/test", "--filesystem", "ext4",
         "--journal", "/var/lib/linux-defragger/state/1000/test.journal"},
        1000U);
    ok = check(
        helper.executable ==
            "/usr/lib/linux-defragger/linux-defragger-operation-engine",
        "privileged operation-engine allowlist") && ok;
    bool rejected_journal = false;
    try {
        (void)helper_command(
            "operation-engine",
            {"defrag", "/dev/test", "--filesystem", "ext4",
             "--journal", "/tmp/not-allowed.journal"},
            1000U);
    } catch (const std::exception&) {
        rejected_journal = true;
    }
    ok = check(rejected_journal, "privileged journal boundary") && ok;

    const std::string manifest = registry_manifest_json();
    ok = check(manifest.find("\"schema\":3") != std::string::npos,
               "manifest schema") && ok;
    ok = check(manifest.find("\"id\":\"ntfs\"") != std::string::npos,
               "manifest NTFS entry") && ok;
    ok = check(manifest.find("\"id\":\"hfsplus\"") != std::string::npos,
               "manifest HFS+ entry") && ok;

    return ok ? 0 : 1;
}
