/*
 * Copyright (C) 2026 Intel Corporation
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for drm_hwmon. Each test builds a fake sysfs PCI device tree with
 * an hwmon directory under a scratch directory, so no GPU is required.
 */

// Empty in clang-tidy passes run without -Dwith_tests, where doctest is not on the include path.
#if __has_include(<doctest/doctest.h>)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../../drm_hwmon.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr const char *BDF = "0000:03:00.0";

/**
 * @brief A fake /sys/bus/pci/devices tree that is removed when the test case ends
 */
class FakePciDevices
{
public:
	fs::path root;

	/**
	 * @brief Creates an empty device directory unique to this test case
	 *
	 * @param [in] tag Test-specific name component, so test cases never share a tree
	 */
	explicit FakePciDevices(const std::string &tag)
		: root(fs::temp_directory_path() / ("xpum_drm_hwmon_" + tag + "_" + std::to_string(::getpid())))
	{
		std::error_code ec;
		fs::remove_all(root, ec);
		fs::create_directories(root);
	}
	/**
	 * @brief Removes the tree and everything the test case wrote into it
	 */
	~FakePciDevices()
	{
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	FakePciDevices(const FakePciDevices &) = delete;
	FakePciDevices &operator=(const FakePciDevices &) = delete;
	FakePciDevices(FakePciDevices &&) = delete;
	FakePciDevices &operator=(FakePciDevices &&) = delete;

	/**
	 * @brief Creates <root>/<bdf>/hwmon/<name> the way the kernel registers it
	 *
	 * @param [in] name hwmon directory name, e.g. "hwmon5"
	 * @return The created directory
	 */
	fs::path addHwmon(const std::string &name = "hwmon5") const
	{
		const fs::path dir = root / BDF / "hwmon" / name;
		fs::create_directories(dir);
		return dir;
	}

	/**
	 * @brief Writes one hwmon attribute, trailing newline included as sysfs writes it
	 *
	 * @param [in] dir      hwmon directory
	 * @param [in] attr     Attribute name, e.g. "temp2_label"
	 * @param [in] contents Attribute contents without the newline
	 */
	static void setAttr(const fs::path &dir, const std::string &attr, const std::string &contents)
	{
		std::ofstream(dir / attr) << contents << '\n';
	}
};

} // namespace

TEST_CASE("drm_hwmon::findDir: finds the hwmon directory under the device")
{
	const FakePciDevices sysfs("find");
	const fs::path dir = sysfs.addHwmon();
	CHECK(drm_hwmon::findDir(BDF, sysfs.root) == dir);
}

TEST_CASE("drm_hwmon::findDir: nullopt when the device has no hwmon directory")
{
	const FakePciDevices sysfs("none");
	CHECK_FALSE(drm_hwmon::findDir(BDF, sysfs.root).has_value());
	fs::create_directories(sysfs.root / BDF / "hwmon");
	CHECK_FALSE(drm_hwmon::findDir(BDF, sysfs.root).has_value());
}

TEST_CASE("drm_hwmon::findDir: a malformed BDF cannot reach files outside its device directory")
{
	const FakePciDevices sysfs("escape");
	fs::create_directories(sysfs.root / "hwmon" / "hwmon0");
	fs::create_directories(sysfs.root / "03:00.0" / "hwmon" / "hwmon0");
	for (const char *bdf : {"", "..", "03:00.0", "03:00.0/.."}) {
		CAPTURE(bdf);
		CHECK_FALSE(drm_hwmon::findDir(bdf, sysfs.root).has_value());
	}
}

TEST_CASE("drm_hwmon::readLabeled: matches channels by label, not by index")
{
	const FakePciDevices sysfs("labeled");
	const fs::path dir = sysfs.addHwmon();
	FakePciDevices::setAttr(dir, "temp2_label", "pkg");
	FakePciDevices::setAttr(dir, "temp2_input", "68000");
	FakePciDevices::setAttr(dir, "temp3_label", "vram");
	FakePciDevices::setAttr(dir, "temp3_input", "76000");
	FakePciDevices::setAttr(dir, "energy1_label", "card");
	FakePciDevices::setAttr(dir, "energy1_input", "4863009279907");

	CHECK(drm_hwmon::readLabeled(dir, "temp", "pkg") == int64_t{68000});
	CHECK(drm_hwmon::readLabeled(dir, "temp", "vram") == int64_t{76000});
	CHECK(drm_hwmon::readLabeled(dir, "energy", "card") == int64_t{4863009279907});
	// The label belongs to another channel type, so it must not match.
	CHECK_FALSE(drm_hwmon::readLabeled(dir, "temp", "card").has_value());
	CHECK_FALSE(drm_hwmon::readLabeled(dir, "temp", "mctrl").has_value());
}

TEST_CASE("drm_hwmon::readLabeled: nullopt when the input is missing or malformed")
{
	const FakePciDevices sysfs("malformed");
	const fs::path dir = sysfs.addHwmon();
	FakePciDevices::setAttr(dir, "temp2_label", "pkg");
	CHECK_FALSE(drm_hwmon::readLabeled(dir, "temp", "pkg").has_value());
	for (const char *contents : {"", "abc", "68000 ", "6.8"}) {
		CAPTURE(contents);
		FakePciDevices::setAttr(dir, "temp2_input", contents);
		CHECK_FALSE(drm_hwmon::readLabeled(dir, "temp", "pkg").has_value());
	}
}

TEST_CASE("drm_hwmon::readAllLabeled: returns every readable channel of the type")
{
	const FakePciDevices sysfs("all");
	const fs::path dir = sysfs.addHwmon();
	FakePciDevices::setAttr(dir, "temp3_label", "vram");
	FakePciDevices::setAttr(dir, "temp3_input", "76000");
	FakePciDevices::setAttr(dir, "temp6_label", "vram_ch_0");
	FakePciDevices::setAttr(dir, "temp6_input", "74000");
	// Unreadable input: left out rather than reported as zero.
	FakePciDevices::setAttr(dir, "temp7_label", "vram_ch_1");
	// Not temperature channels.
	FakePciDevices::setAttr(dir, "energy1_label", "card");
	FakePciDevices::setAttr(dir, "energy1_input", "1");
	FakePciDevices::setAttr(dir, "temp_label", "no index");

	const std::map<std::string, int64_t> expected = {{"vram", 76000}, {"vram_ch_0", 74000}};
	CHECK(drm_hwmon::readAllLabeled(dir, "temp") == expected);
}

#endif
