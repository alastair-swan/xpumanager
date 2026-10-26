/*
 * Copyright (C) 2026 Intel Corporation
 * SPDX-License-Identifier: MIT
 *
 * Windows stub for drm_hwmon — there is no hwmon sysfs on Windows.
 * All functions return std::nullopt / empty so callers degrade gracefully
 * without any platform guards.
 */

#include "../drm_hwmon.h"

namespace drm_hwmon {

std::optional<std::filesystem::path> findDir(std::string_view /*bdf*/, const std::filesystem::path & /*pciDevRoot*/)
{
	return std::nullopt;
}

std::optional<int64_t> readLabeled(const std::filesystem::path & /*dir*/, std::string_view /*type*/,
								   std::string_view /*label*/)
{
	return std::nullopt;
}

std::map<std::string, int64_t> readAllLabeled(const std::filesystem::path & /*dir*/, std::string_view /*type*/)
{
	return {};
}

} // namespace drm_hwmon
