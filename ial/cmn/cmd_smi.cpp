/*
 * Copyright (C) 2026 Intel Corporation
 * SPDX-License-Identifier: MIT
 *
 */

#include "cmd_smi.h"
#include "cmd_ps.h"
#include "debug.h"
#include "fan.h"
#include "memory.h"
#include "metrics_registry.h"
#include "os.h"
#include "power.h"
#include "temperature.h"
#include <enginegroup.h>
#include "table_builder.h"
#include <chrono>
#include "utility/compat/format.h"
#include <ranges>
#include <thread>
#include <vector>

// Version fallbacks (XPUM_VERSION_MAJOR/MINOR are set via -D compiler flags)
#ifndef XPUM_VERSION_MAJOR
#define XPUM_VERSION_MAJOR 0
#endif
#ifndef XPUM_VERSION_MINOR
#define XPUM_VERSION_MINOR 0
#endif

// ── Utility formatting helpers ─────────────────────────────────────────────

static std::string fmtPower(double currentW, double tdpW)
{
	const std::string cur = currentW > 0.0 ? xpum::compat::format("{:.0f}W", currentW) : "N/A";
	if (tdpW > 0.0) {
		return xpum::compat::format("{} / {:.0f}W", cur, tdpW);
	}
	if (currentW <= 0.0) {
		return "N/A";
	}
	return cur;
}

static std::string fmtMem(double usedMiB, double totalMiB)
{
	if (totalMiB <= 0.0) {
		return "N/A";
	}
	return xpum::compat::format("{:.0f}MiB / {:.0f}MiB", usedMiB, totalMiB);
}

/**
 * @brief Format fan telemetry string.
 *
 * @param[in] speed Fan speed, or negative when unavailable.
 * @param[in] units Units of @p speed: a percentage, or RPM when the fan reports no maximum.
 * @return std::string Formatted fan string for the SMI table.
 */
static std::string fmtFan(int32_t speed, zes_fan_speed_units_t units)
{
	if (speed < 0) {
		return "N/A";
	}
	if (units == ZES_FAN_SPEED_UNITS_RPM) {
		return xpum::compat::format("{}RPM", speed);
	}
	return xpum::compat::format("{}%", speed);
}

static std::string processTypeFromEngines(uint64_t engines, uint64_t memSizeKiB, uint64_t sharedSizeKiB)
{
	if (engines & ZES_ENGINE_TYPE_FLAG_COMPUTE) {
		return "C";
	}
	if (engines & ZES_ENGINE_TYPE_FLAG_RENDER || engines & ZES_ENGINE_TYPE_FLAG_3D) {
		return "G";
	}
	if (engines & ZES_ENGINE_TYPE_FLAG_MEDIA) {
		return "M";
	}
	if (engines & ZES_ENGINE_TYPE_FLAG_DMA) {
		return "DMA";
	}
	if (engines & ZES_ENGINE_TYPE_FLAG_OTHER) {
		return "Other";
	}
	if (memSizeKiB > 0 || sharedSizeKiB > 0) {
		return "C";
	}
	return "N/A";
}

// ── Per-device stat collection ─────────────────────────────────────────────

/**
 * @brief Collect device name, BDF address, ECC state, and driver version
 */
void cmdSmi::collectStaticProps(SmiDeviceStats &stats, devInfo *di)
{
	TRACING();
	device *dev = di->dev;

	// Device name — prefer the compute path; fall back to sysman modelName when
	// running in sysman-only mode (null compute handle, e.g. DEPENDENCY_UNAVAILABLE).
	ze_device_properties_t zeDevProp = {};
	if (dev->getDevProps(di->deviceHdl, &zeDevProp) == ZE_RESULT_SUCCESS) {
		stats.name = zeDevProp.name;
		stats.eccEnabled = (zeDevProp.flags & ZE_DEVICE_PROPERTY_FLAG_ECC) != 0;
	} else {
		zes_device_properties_t zesDevPropFallback = {};
		if (dev->zesGetDevProps(di->zesDeviceHdl, &zesDevPropFallback) == ZE_RESULT_SUCCESS) {
			stats.name = zesDevPropFallback.modelName;
		}
	}

	// Driver version + device index
	stats.devIndex = di->index;

	if (dev->getDriverVersionString(stats.driverVersion) != ZE_RESULT_SUCCESS) {
		stats.driverVersion.clear();
	}

	zes_device_properties_t zesDevProp = {};
	zesDevProp.stype = ZES_STRUCTURE_TYPE_DEVICE_PROPERTIES;
	if (dev->zesGetDevProps(di->zesDeviceHdl, &zesDevProp) == ZE_RESULT_SUCCESS) {
		// The core query above is unavailable where only sysman is initialized — a container
		// without a render node, or the paths that skip zeInit — and leaves the name blank.
		// zes_device_properties_t embeds the same core properties, so use them as a fallback.
		if (stats.name.empty()) {
			stats.name = zesDevProp.core.name;
			stats.eccEnabled = (zesDevProp.core.flags & ZE_DEVICE_PROPERTY_FLAG_ECC) != 0;
		}
	}

	// PCI BDF address
	stats.pciBdf = dev->getBDFStr();

	stats.kmdVersion = GETKERNELDRIVERVERSION(stats.pciBdf);
}

/**
 * @brief Read current fan speed (%, or RPM when no percentage can be derived) if the device has fans.
 */
void cmdSmi::collectFan(SmiDeviceStats &stats, devInfo *di)
{
	TRACING();
	auto *fanHandler = reinterpret_cast<::fan *>(di->dev->getFan());
	if (fanHandler == nullptr) {
		return;
	}

	int32_t speed = -1;
	zes_fan_speed_units_t units = ZES_FAN_SPEED_UNITS_PERCENT;
	if (fanHandler->getSpeed(speed, units) == ZE_RESULT_SUCCESS) {
		stats.fanSpeed = speed;
		stats.fanSpeedUnits = units;
		stats.fanValid = true;
	}
}

/**
 * @brief Sample GPU core temperature (uses average across tiles)
 */
void cmdSmi::collectTemperature(SmiDeviceStats &stats, devInfo *di)
{
	TRACING();
	auto *tempHandler = reinterpret_cast<::temperature *>(di->dev->getTemperature());
	if (tempHandler == nullptr) {
		return;
	}

	std::map<uint32_t, double> temps;
	if (tempHandler->getTempPerTile(ZES_TEMP_SENSORS_GPU, temps) == ZE_RESULT_SUCCESS && !temps.empty()) {
		double sum = 0.0;
		int count = 0;
		for (const auto &[tid, t] : temps) {
			if (t > 0.0 && t < MAX_REASONABLE_TEMP_CELSIUS) {
				sum += t;
				++count;
			}
		}
		if (count > 0) {
			stats.gpuTempC = sum / static_cast<double>(count);
			stats.tempValid = true;
		}
	}
}

/**
 * @brief Read device memory total and current usage
 */
void cmdSmi::collectMemory(SmiDeviceStats &stats, devInfo *di)
{
	TRACING();
	auto *memHandler = reinterpret_cast<::memory *>(di->dev->getMemory());
	if (memHandler == nullptr) {
		return;
	}

	uint64_t sizeBytes = 0;
	if (memHandler->getMemorySize(&sizeBytes) == ZE_RESULT_SUCCESS && sizeBytes > 0) {
		stats.memTotalMiB = static_cast<double>(sizeBytes) / (1024.0 * 1024.0);
	}

	// An iGPU's memory is the host's RAM, so the module's own usage is the whole system's. Show what
	// GPU clients have allocated instead: the sum of the process table's GPU memory.
	if (di->dev->isIGPU()) {
		std::vector<psInfo> procs;
		if (cmdPs::getProcessList(di, procs) == ZE_RESULT_SUCCESS) {
			uint64_t usedKiB = 0;
			for (const auto &p : procs) {
				usedKiB += p.memSize;
			}
			stats.memUsedMiB = static_cast<double>(usedKiB) / 1024.0;
			stats.memValid = (stats.memTotalMiB > 0.0);
		}
		return;
	}

	uint64_t usedBytes = 0;
	double utilPct = 0.0;
	if (memHandler->getMemoryUsed(&usedBytes, &utilPct) == ZE_RESULT_SUCCESS) {
		stats.memUsedMiB = static_cast<double>(usedBytes) / (1024.0 * 1024.0);
		stats.memValid = (stats.memTotalMiB > 0.0);
	}
}

/**
 * @brief Read the sustained power limit (TDP) for the device.
 *
 * Priority: SUSTAINED → BURST → PEAK → UNKNOWN.
 * Arc/Intel drivers often report the TDP as UNKNOWN level; accept it as a
 * fallback so the display isn't always blank on those devices.
 */
void cmdSmi::collectPowerTdp(SmiDeviceStats &stats, devInfo *di)
{
	TRACING();
	auto *powerHandler = reinterpret_cast<::power *>(di->dev->getPower());
	if (powerHandler == nullptr) {
		return;
	}

	std::vector<PowerLimitExt> limits;
	if (powerHandler->getLimitsExt(limits) != ZE_RESULT_SUCCESS || limits.empty()) {
		return;
	}

	// Assign scores: lower = higher priority
	auto levelPriority = [](zes_power_level_t lvl) -> int {
		switch (lvl) {
		case ZES_POWER_LEVEL_SUSTAINED:
			return 0;
		case ZES_POWER_LEVEL_BURST:
			return 1;
		case ZES_POWER_LEVEL_PEAK:
			return 2;
		case ZES_POWER_LEVEL_UNKNOWN:
			return 3;
		default:
			return 4;
		}
	};

	const PowerLimitExt *best = nullptr;
	for (const auto &lim : limits) {
		if (lim.limitMw == 0) {
			continue;
		}
		if (best == nullptr || levelPriority(lim.level) < levelPriority(best->level)) {
			best = &lim;
		}
	}

	if (best != nullptr) {
		stats.powerTdpW = static_cast<double>(best->limitMw) / 1000.0;
	}
}

/**
 * @brief Capture baseline energy counters and engine activity snapshots for delta calculation
 */
void cmdSmi::captureBaseline(SmiBaseline &baseline, devInfo *di)
{
	TRACING();
	device *dev = di->dev;

	// Energy baseline
	auto *powerHandler = reinterpret_cast<::power *>(dev->getPower());
	if (powerHandler != nullptr) {
		std::map<uint32_t, std::pair<uint64_t, uint64_t>> tileEnergy;
		if (powerHandler->getEnergyPerTile(tileEnergy) == ZE_RESULT_SUCCESS) {
			baseline.tileEnergy = std::move(tileEnergy);
		}
	}

	// Engine activity baseline: from fdinfo where the platform does not allow the sysman
	// engine counters (sysmanEngineCountersAllowed), otherwise from every engine group, so
	// utilization can be taken from the busiest engine rather than from the device-wide
	// average (see computeFromBaseline).
	if (!sysmanEngineCountersAllowed()) {
		baseline.fdinfoSnap = fdinfo::capture(devPciAddr(*di));
		baseline.useFdinfo = true;
		return;
	}
	auto *engGroup = reinterpret_cast<enginegroup *>(dev->getEngineGroup());
	if (engGroup != nullptr) {
		std::vector<EngineActivitySample> engineActivity;
		if (engGroup->getAllEngineActivity(engineActivity) == ZE_RESULT_SUCCESS) {
			baseline.engineActivity = std::move(engineActivity);
		}
	}
}

/**
 * @brief Compute power (W) and GPU utilization (%) from baseline vs. current readings
 */
void cmdSmi::computeFromBaseline(SmiDeviceStats &stats, const SmiBaseline &baseline, devInfo *di)
{
	TRACING();
	device *dev = di->dev;

	// Power: deltaEnergy (µJ) / deltaTime (µs) = Watts
	auto *powerHandler = reinterpret_cast<::power *>(dev->getPower());
	if (powerHandler != nullptr && !baseline.tileEnergy.empty()) {
		std::map<uint32_t, std::pair<uint64_t, uint64_t>> tileEnergy;
		if (powerHandler->getEnergyPerTile(tileEnergy) == ZE_RESULT_SUCCESS) {
			double totalPowerW = 0.0;
			int tileCount = 0;
			for (const auto &[tid, cur] : tileEnergy) {
				auto it = baseline.tileEnergy.find(tid);
				if (it == baseline.tileEnergy.end()) {
					continue;
				}
				uint64_t prevEnergy = it->second.first;
				uint64_t prevTs = it->second.second;
				uint64_t curEnergy = cur.first;
				uint64_t curTs = cur.second;
				if (curTs > prevTs && curEnergy >= prevEnergy) {
					double w = static_cast<double>(curEnergy - prevEnergy) / static_cast<double>(curTs - prevTs);
					totalPowerW += w;
					++tileCount;
				}
			}
			if (tileCount > 0) {
				stats.powerCurrentW = totalPowerW;
				stats.powerValid = true;
			}
		}
	}

	// GPU utilization: busiest engine per tile, averaged across tiles. Sysman's
	// ZES_ENGINE_GROUP_ALL cannot be used here: it averages the busyness of every engine on
	// the device, so a compute-only workload saturating one of nine engines reads as 11%.
	if (baseline.useFdinfo) {
		const auto util = fdinfo::aggregateDeviceUtil(baseline.fdinfoSnap, fdinfo::capture(devPciAddr(*di)));
		if (const auto busiest = metrics::busiestEngineClass(util.compute, util.render, util.media, util.copy)) {
			stats.gpuUtilPercent = static_cast<double>(*busiest);
			stats.utilValid = true;
		}
		return;
	}
	auto *engGroup = reinterpret_cast<enginegroup *>(dev->getEngineGroup());
	if (engGroup != nullptr && !baseline.engineActivity.empty()) {
		std::vector<EngineActivitySample> engineActivity;
		if (engGroup->getAllEngineActivity(engineActivity) == ZE_RESULT_SUCCESS) {
			const auto utilPerTile = enginegroup::computeGpuUtilPerTile(baseline.engineActivity, engineActivity);
			if (const auto deviceUtil = enginegroup::deviceUtilFromTiles(utilPerTile)) {
				stats.gpuUtilPercent = *deviceUtil;
				stats.utilValid = true;
			}
		}
	}
}

// ── Table rendering ──────────────────────────────────────────────────────────

/**
 * @brief Split a device name into lines of at most @p width characters, breaking at spaces.
 *
 * Words longer than @p width are hard-broken.
 *
 * @param[in] name  Device name.
 * @param[in] width Maximum line width.
 * @return The lines, at least one.
 */
static std::vector<std::string> wrapName(const std::string &name, size_t width)
{
	std::vector<std::string> lines;
	std::string line;
	size_t pos = 0;
	while (pos < name.size()) {
		size_t end = name.find(' ', pos);
		if (end == std::string::npos) {
			end = name.size();
		}
		std::string word = name.substr(pos, end - pos);
		pos = end + 1;
		while (word.size() > width) {
			if (!line.empty()) {
				lines.push_back(line);
				line.clear();
			}
			lines.push_back(word.substr(0, width));
			word.erase(0, width);
		}
		if (word.empty()) {
			continue;
		}
		if (line.empty()) {
			line = word;
		} else if (line.size() + 1 + word.size() <= width) {
			line += ' ';
			line += word;
		} else {
			lines.push_back(line);
			line = word;
		}
	}
	if (!line.empty() || lines.empty()) {
		lines.push_back(line);
	}
	return lines;
}

// Column content widths of the summary tables. They match nvidia-smi's, so the two tools'
// output lines up at 91 columns: each cell renders as " <content> |".
constexpr int GPU_COL0_WIDTH = 39; // GPU  Name  Persistence-M  /  Fan  Temp  Pwr:Usage/Cap
constexpr int GPU_COL1_WIDTH = 22; // Bus-Id  Disp.A  /  Memory-Usage
constexpr int GPU_COL2_WIDTH = 20; // Volatile Uncorr. ECC  /  GPU-Util  Compute M.
// The process table's columns share the same 87-character content area: 3 + 7 + 4 + 51 + 10
// plus " | " between each pair.
constexpr int PROC_GPU_WIDTH = 3;
constexpr int PROC_PID_WIDTH = 7;
constexpr int PROC_TYPE_WIDTH = 4;
constexpr int PROC_NAME_WIDTH = 51;
constexpr int PROC_MEM_WIDTH = 10;

/**
 * @brief Build and return the GPU summary table.
 *
 * Three fixed-width columns with multi-line column headers (via setColumnExtraHeaders),
 * matching the nvidia-smi layout:
 *   Col 1: GPU#  Name  Persistence-M  /  Fan  Temp  Pwr:Usage/Cap
 *   Col 2: Bus-Id  Disp.A  /  Memory-Usage
 *   Col 3: Volatile Uncorr. ECC  /  GPU-Util  Compute M.
 * Every cell is pre-formatted to its column's full width, so the strings below fix the layout.
 */
TableBuilder cmdSmi::buildGpuTable(const std::vector<SmiDeviceStats> &devStats, const std::string &lzVersion)
{
	// Col 1 line 1: "{:>3}  " index, then the name, then "Persistence-M" right-aligned.
	constexpr int persistenceWidth = static_cast<int>(sizeof("Persistence-M") - 1);
	constexpr int nameHeaderWidth = GPU_COL0_WIDTH - 5 - persistenceWidth;
	// The persistence value only needs " Off", leaving the name more room than its header.
	constexpr size_t nameWidth = GPU_COL0_WIDTH - 5 - 4;
	// Col 1 line 2: fan wide enough for "20000RPM", temperature for "100C", power right-aligned.
	constexpr int fanWidth = 8;
	constexpr int tempWidth = 4;
	constexpr int pwrWidth = GPU_COL0_WIDTH - fanWidth - 1 - tempWidth;
	// Col 2 line 1: "Disp.A" right-aligned after the bus id.
	constexpr int dispWidth = static_cast<int>(sizeof("Disp.A") - 1);
	// Col 3 line 2: "GPU-Util" and "Compute M." each right-aligned.
	constexpr int utilWidth = static_cast<int>(sizeof("GPU-Util") - 1);
	constexpr int computeWidth = GPU_COL2_WIDTH - utilWidth - 2;

	TableBuilder table = TableBuilder::bordered();
	table.disableAutoSizing();
	table
		.addColumn(xpum::compat::format("GPU  {:<{}}{}", "Name", nameHeaderWidth, "Persistence-M"), GPU_COL0_WIDTH,
				   Align::Left)
		.addColumn(xpum::compat::format("{:<{}}{}", "Bus-Id", GPU_COL1_WIDTH - dispWidth, "Disp.A"), GPU_COL1_WIDTH,
				   Align::Left)
		.addColumn("Volatile Uncorr. ECC", GPU_COL2_WIDTH, Align::Right);

	// Multi-line column headers (second header line per column)
	table.setColumnExtraHeaders(0, {xpum::compat::format("{:<{}} {:>{}}{:>{}}", "Fan", fanWidth, "Temp", tempWidth,
														 "Pwr:Usage/Cap", pwrWidth)});
	table.setColumnExtraHeaders(1, {xpum::compat::format("{:>{}}", "Memory-Usage", GPU_COL1_WIDTH)});
	table.setColumnExtraHeaders(
		2, {xpum::compat::format("{:>{}}  {:>{}}", "GPU-Util", utilWidth, "Compute M.", computeWidth)});

	// Banner rows: inside the table border, above column headers. One left-aligned line, as
	// nvidia-smi prints it, when the versions fit; otherwise two centred lines, since the table
	// would otherwise truncate the banner.
	const std::string shortVer = xpum::compat::format("v{}.{}", XPUM_VERSION_MAJOR, XPUM_VERSION_MINOR);

	// Take the first device that reports a version rather than device 0, whose value
	// may be absent: a mixed host can pair an iGPU on the in-tree module, which names
	// no release, with a dGPU on the DKMS one that does. The banner summarises, so it
	// names one version; discovery reports each device's own.
	const auto firstReported = [&devStats](std::string SmiDeviceStats::*field) -> std::string {
		for (const auto &s : devStats) {
			if (!(s.*field).empty()) {
				return s.*field;
			}
		}
		return {};
	};

	const std::string umd = firstReported(&SmiDeviceStats::driverVersion);
	const std::string umdVersion = umd.empty() ? "N/A" : umd;

	const std::string kmd = firstReported(&SmiDeviceStats::kmdVersion);
	const std::string kmdSegment = kmd.empty() ? "" : xpum::compat::format("   KMD Version: {}", kmd);

	const std::string title = xpum::compat::format("Intel XPU-SMI {}{}", shortVer, kmdSegment);
	const std::string versions = xpum::compat::format("UMD Version: {}   Level Zero: {}", umdVersion, lzVersion);
	const std::string oneLine = xpum::compat::format("{}   {}", title, versions);
	constexpr size_t bannerWidth = GPU_COL0_WIDTH + 3 + GPU_COL1_WIDTH + 3 + GPU_COL2_WIDTH;
	if (oneLine.size() <= bannerWidth) {
		table.addPreHeaderSpanRow(oneLine, BorderStyle::Normal, Align::Left);
	} else {
		table.addPreHeaderSpanRow(title, BorderStyle::None);
		table.addPreHeaderSpanRow(versions);
	}

	for (const auto &s : devStats) {
		// Build col-0 lines: the name wraps at word boundaries rather than truncating.
		const std::vector<std::string> nameLines = wrapName(s.name, nameWidth);
		std::vector<std::string> col0Lines;
		col0Lines.push_back(xpum::compat::format("{:>3}  {:<{}}{:>4}", s.devIndex, nameLines[0], nameWidth, "Off"));
		for (size_t i = 1; i < nameLines.size(); ++i) {
			col0Lines.push_back(xpum::compat::format("     {}", nameLines[i]));
		}

		std::string bdfLine =
			xpum::compat::format("{:>{}} {:>{}}", s.pciBdf, GPU_COL1_WIDTH - dispWidth - 1, "Off", dispWidth);
		std::string eccLine = xpum::compat::format("{:>{}}", s.eccEnabled ? "Enabled" : "Disabled", GPU_COL2_WIDTH);

		// Row 2: live telemetry per column
		std::string tempStr = s.tempValid ? xpum::compat::format("{:.0f}C", s.gpuTempC) : "N/A";
		std::string fanStr = s.fanValid ? fmtFan(s.fanSpeed, s.fanSpeedUnits) : "N/A";
		std::string pwrStr = fmtPower(s.powerValid ? s.powerCurrentW : 0.0, s.powerTdpW);
		std::string fanTempPwrLine =
			xpum::compat::format("{:<{}} {:>{}}{:>{}}", fanStr, fanWidth, tempStr, tempWidth, pwrStr, pwrWidth);
		col0Lines.push_back(fanTempPwrLine);

		// Align memory/util on the same line as fan/temp/pwr by padding cols 1 & 2
		// with empty strings for each name-overflow line.
		const size_t numWraps = col0Lines.size() - 2;
		std::string memLine =
			xpum::compat::format("{:>{}}", s.memValid ? fmtMem(s.memUsedMiB, s.memTotalMiB) : "N/A", GPU_COL1_WIDTH);
		std::string utilStr = s.utilValid ? xpum::compat::format("{:.0f}%", s.gpuUtilPercent) : "N/A";
		std::string utilComputeLine =
			xpum::compat::format("{:>{}}  {:>{}}", utilStr, utilWidth, "Default", computeWidth);
		std::vector<std::string> col1Lines = {bdfLine};
		col1Lines.insert(col1Lines.end(), numWraps, "");
		col1Lines.push_back(memLine);
		std::vector<std::string> col2Lines = {eccLine};
		col2Lines.insert(col2Lines.end(), numWraps, "");
		col2Lines.push_back(utilComputeLine);

		table.addMultiLineRow({
			col0Lines,
			col1Lines,
			col2Lines,
		});

		if (&s != &devStats.back()) {
			table.addSeparator(BorderStyle::Normal);
		}
	}

	return table;
}

/**
 * @brief Build and return the process listing table.
 *
 * Fixed-width columns, the same overall width as the GPU table; long process names are
 * truncated with "...".
 */
TableBuilder cmdSmi::buildProcessTable(const std::vector<devInfo> &deviceList)
{
	TableBuilder procTable = TableBuilder::bordered();
	procTable.disableAutoSizing();
	procTable.addColumn("GPU", PROC_GPU_WIDTH, Align::Right)
		.addColumn("PID", PROC_PID_WIDTH, Align::Right)
		.addColumn("Type", PROC_TYPE_WIDTH, Align::Center)
		.addColumn("Process Name", PROC_NAME_WIDTH, Align::Left)
		.addColumn("GPU Memory", PROC_MEM_WIDTH, Align::Right);
	procTable.setColumnExtraHeaders(4, {"Usage"});

	// "Processes:" banner: left-aligned, no separator before column headers.
	procTable.addPreHeaderSpanRow("Processes:", BorderStyle::None, Align::Left);
	procTable.suppressHeaderColumnSeparators();
	procTable.suppressDataColumnSeparators();

	cmdPs ps;
	bool anyProcess = false;
	for (const auto &di : deviceList) {
		std::vector<psInfo> procs;
		if (ps.getProcessList(&di, procs) != ZE_RESULT_SUCCESS) {
			continue;
		}
		for (const auto &p : procs) {
			// memSize is in KiB (already divided by 1024 in getProcessList)
			double memMiB = static_cast<double>(p.memSize) / 1024.0;
			std::string memStr = xpum::compat::format("{:.0f} MiB", memMiB);
			std::string procType = processTypeFromEngines(p.engines, p.memSize, p.sharedSize);
			procTable.addRow(di.index, p.processId, procType, p.commandName, memStr);
			anyProcess = true;
		}
	}

	if (!anyProcess) {
		// No running processes found
		procTable.addSpanRow("No running GPU processes found.", BorderStyle::None);
	}

	return procTable;
}

// ── cmds interface ───────────────────────────────────────────────────────────

void cmdSmi::help(HELP helpType)
{
	// cmdSmi is not listed as a subcommand — it runs automatically when no args are given.
	// Provide a minimal entry in case of future introspection.
	std::vector<helpCmd> helpList;
	helpList.push_back(helpCmd(HEADING, "Show GPU status summary (default, no-args behavior)"));
	printHelp(helpList, helpType);
}

/**
 * @brief Collect GPU telemetry and render it to a string.
 *
 * Enumerates all available GPU devices, gathers static properties,
 * temperature, memory, power, and utilization metrics (via a 200 ms
 * delta window), then formats the result as a printable table string.
 *
 * @param args  CLI arguments used for device enumeration and formatting.
 *
 * @return A pair of the Level Zero result code and the rendered output
 *         string.  The string is empty when the result is not
 *         ZE_RESULT_SUCCESS.
 */
std::pair<ze_result_t, std::string> cmdSmi::collectAndRender(arg_struct *args)
{
	// ── 1. Enumerate all devices ────────────────────────────────────────
	std::vector<devInfo> deviceList;
	ze_result_t result = args->sm.findDevice("", &deviceList);
	if (result != ZE_RESULT_SUCCESS) {
		ERR("Failed to enumerate GPU devices (error 0x{:x}).\n", result);
		return {result, {}};
	}
	if (deviceList.empty()) {
		return {ZE_RESULT_ERROR_DEVICE_LOST, {}};
	}

	// ── 2. Collect static properties + temperature + memory + TDP ───────
	std::vector<SmiDeviceStats> devStats(deviceList.size());
	std::vector<SmiBaseline> baselines(deviceList.size());

	for (auto i : std::views::iota(size_t{0}, deviceList.size())) {
		collectStaticProps(devStats[i], &deviceList[i]);
		collectFan(devStats[i], &deviceList[i]);
		collectTemperature(devStats[i], &deviceList[i]);
		collectMemory(devStats[i], &deviceList[i]);
		collectPowerTdp(devStats[i], &deviceList[i]);
		captureBaseline(baselines[i], &deviceList[i]);
	}

	// ── 3. Wait for delta window (200 ms) ──────────────────────────────
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	// ── 4. Compute delta-based metrics (power, utilization) ─────────────
	for (auto i : std::views::iota(size_t{0}, deviceList.size())) {
		computeFromBaseline(devStats[i], baselines[i], &deviceList[i]);
	}

	// ── 5. Get Level Zero version ───────────────────────────────────────
	std::string lzVersion;
	args->sm.getLoaderVersion(&lzVersion);

	// ── 6. Render to string ─────────────────────────────────────────────
	TableBuilder gpuTable = buildGpuTable(devStats, lzVersion);
	TableBuilder procTable = buildProcessTable(deviceList);
	procTable.padToWidth(gpuTable.getTotalWidth());
	return {result, gpuTable.toString() + "\n" + procTable.toString() + "\n"};
}

/**
 * @brief Run a single-shot SMI summary and print it to stdout.
 *
 * Calls collectAndRender() once, prints the result, and returns the
 * Level Zero result code.  Prints "No GPU devices found." when no
 * devices are enumerated.
 *
 * @param args  CLI arguments used for device enumeration and formatting.
 *
 * @retval 0                          Success.
 * @retval ZE_RESULT_ERROR_DEVICE_LOST No GPU devices found.
 * @retval ZE_RESULT_ERROR_*          Propagated from collectAndRender.
 */
int cmdSmi::run(arg_struct *args)
{
	TRACING();
	auto [result, output] = collectAndRender(args);
	if (result != ZE_RESULT_SUCCESS) {
		if (result == ZE_RESULT_ERROR_DEVICE_LOST) {
			PRINT("No GPU devices found.\n");
		}
		return result;
	}
	PRINT("{}", output.c_str());
	return result;
}
