/*
 * Copyright (C) 2026 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "../drm_hwmon.h"
#include "bdf.h"
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {

/**
 * @brief Reads the first line of a sysfs attribute
 *
 * @param [in] path Attribute file
 * @return The line without its trailing newline, or std::nullopt when the file cannot be read
 */
std::optional<std::string> readLine(const fs::path &path)
{
	std::ifstream file(path);
	std::string line;
	if (!std::getline(file, line)) {
		return std::nullopt;
	}
	return line;
}

/**
 * @brief Reads a sysfs attribute holding one signed decimal integer
 *
 * @param [in] path Attribute file
 * @return The value, or std::nullopt when the file cannot be read or does not hold an integer
 */
std::optional<int64_t> readInteger(const fs::path &path)
{
	const auto line = readLine(path);
	if (!line) {
		return std::nullopt;
	}
	const char *const first = line->data();
	const char *const last = std::to_address(line->end());
	int64_t value = 0;
	const auto [end, ec] = std::from_chars(first, last, value);
	if (ec != std::errc{} || end != last) {
		return std::nullopt;
	}
	return value;
}

/**
 * @brief Splits an hwmon label attribute name into its channel stem
 *
 * @param [in] fileName Directory entry name, e.g. "temp2_label"
 * @param [in] type     Channel type prefix, e.g. "temp"
 * @return The stem, e.g. "temp2", or std::nullopt when @p fileName is not a <type>N_label file
 */
std::optional<std::string_view> labelStem(std::string_view fileName, std::string_view type)
{
	constexpr std::string_view labelSuffix = "_label";
	if (!fileName.starts_with(type) || !fileName.ends_with(labelSuffix)) {
		return std::nullopt;
	}
	const std::string_view index = fileName.substr(type.size(), fileName.size() - type.size() - labelSuffix.size());
	if (index.empty()) {
		return std::nullopt;
	}
	for (const char c : index) {
		if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
			return std::nullopt;
		}
	}
	return fileName.substr(0, fileName.size() - labelSuffix.size());
}

} // namespace

namespace drm_hwmon {

std::optional<fs::path> findDir(std::string_view bdf, const fs::path &pciDevRoot)
{
	if (!isValidBdf(bdf)) {
		return std::nullopt;
	}
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(pciDevRoot / bdf / "hwmon", ec)) {
		if (entry.path().filename().string().starts_with("hwmon")) {
			return entry.path();
		}
	}
	return std::nullopt;
}

std::optional<int64_t> readLabeled(const fs::path &dir, std::string_view type, std::string_view label)
{
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(dir, ec)) {
		const std::string fileName = entry.path().filename().string();
		const auto stem = labelStem(fileName, type);
		if (!stem || readLine(entry.path()) != label) {
			continue;
		}
		return readInteger(dir / (std::string(*stem) + "_input"));
	}
	return std::nullopt;
}

std::map<std::string, int64_t> readAllLabeled(const fs::path &dir, std::string_view type)
{
	std::map<std::string, int64_t> values;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(dir, ec)) {
		const std::string fileName = entry.path().filename().string();
		const auto stem = labelStem(fileName, type);
		if (!stem) {
			continue;
		}
		const auto label = readLine(entry.path());
		const auto value = readInteger(dir / (std::string(*stem) + "_input"));
		if (label && value) {
			values[*label] = *value;
		}
	}
	return values;
}

} // namespace drm_hwmon
