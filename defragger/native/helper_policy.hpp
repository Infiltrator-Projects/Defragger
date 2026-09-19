// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace defragger {

struct HelperCommand {
    std::string executable;
    std::vector<std::string> arguments;
};

std::uint32_t invoking_uid_from_environment();
HelperCommand helper_command(
    std::string_view program,
    const std::vector<std::string>& arguments,
    std::uint32_t invoking_uid);

} // namespace defragger
