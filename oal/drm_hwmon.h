/*
 * Copyright (C) 2026 Intel Corporation
 * SPDX-License-Identifier: MIT
 *
 * Read-only access to the hwmon sensors a GPU's DRM driver registers — Linux only.
 *
 * The xe driver exposes temperatures, energy counters and fan speed under
 * /sys/bus/pci/devices/<bdf>/hwmon/hwmonN, readable by any user. Sysman reads
 * several of the same quantities from PMT telemetry instead, which is root-only,
 * so these reads let an unprivileged xpu-smi still report them.
 *
 * Channels are looked up by their <type>N_label (e.g. temp2_label = "pkg"),
 * never by index: the kernel numbers channels per platform.
 *
 * On non-Linux platforms all functions are stubs that return std::nullopt / empty.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace drm_hwmon {

/**
 * @brief Locates the hwmon directory registered under a GPU's PCI device
 *
 * @p bdf is validated as a canonical address before being used as a path
 * component, so a malformed value cannot escape @p pciDevRoot.
 *
 * @param [in] bdf        PCI BDF address of the device, in canonical DDDD:BB:DD.F form
 * @param [in] pciDevRoot sysfs PCI device directory (defaults to live sysfs; override in tests)
 * @return The hwmonN directory, or std::nullopt when @p bdf is malformed or the device has none
 */
[[nodiscard]] std::optional<std::filesystem::path>
findDir(std::string_view bdf, const std::filesystem::path &pciDevRoot = "/sys/bus/pci/devices");

/**
 * @brief Reads the <type>N_input of the channel whose <type>N_label is @p label
 *
 * @param [in] dir   hwmon directory, as returned by findDir()
 * @param [in] type  Channel type prefix, e.g. "temp" or "energy"
 * @param [in] label Channel label, e.g. "pkg"
 * @return The raw value in hwmon units (millidegrees Celsius, microjoules, ...), or
 *         std::nullopt when no channel has that label or its input cannot be read
 */
[[nodiscard]] std::optional<int64_t> readLabeled(const std::filesystem::path &dir, std::string_view type,
												 std::string_view label);

/**
 * @brief Reads every labelled <type>N_input channel in @p dir
 *
 * Channels whose label or input cannot be read are left out.
 *
 * @param [in] dir  hwmon directory, as returned by findDir()
 * @param [in] type Channel type prefix, e.g. "temp"
 * @return Map of label -> raw value in hwmon units
 */
[[nodiscard]] std::map<std::string, int64_t> readAllLabeled(const std::filesystem::path &dir, std::string_view type);

} // namespace drm_hwmon
