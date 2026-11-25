#include "./third_party/httplib.h"
#include "./third_party/json.hpp"
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <limits>
#include <csignal>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <system_error>
#include <mutex>
#include <memory>
#include <cstdlib>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

// main.cpp prepares, releases, and stops browser experiments.
// tester.cpp owns tc/IFB settings and the browsing simulators.
// Keep bandwidth, delay, jitter, and loss settings in tester.cpp.
// Workload switches and ABR tuning share tester/experiment-parameters.js.

// ==================== 1. SHARED EXPERIMENT STATE ====================

// exchange(true) reserves the experiment slot in one atomic operation.
std::atomic_bool isExperimentRunning{false};
// The mutex protects the tester command pipe and experiment protocol flags.
std::mutex testerControlMutex;
// Parent's writing end: sends START and CANCEL to Tester's standard input.
int testerStopWriteFd = -1;
bool stopCommandSent = false;
bool startCommandSent = false;
bool testerPrepared = false;
// These fields are protected by testerControlMutex, alongside the pipe state.
std::uint64_t nextRunId = 0;
std::uint64_t activeRunId = 0;
std::uint64_t lastFinishedRunId = 0;
int lastTesterExitCode = -1;
bool lastTesterExitSuccessful = false;
bool resultSavedForActiveRun = false;
// Snapshot of the configuration actually used by the active experiment.
nlohmann::json activeExperimentConfig = nullptr;
namespace fs = std::filesystem;

// Telemetry is best-effort: QoE saving must never depend on telemetry success.
struct CapturedTelemetry {
  std::mutex mutex;
  std::atomic_bool testerExited{false};
  nlohmann::json network = nlohmann::json::array();
  nlohmann::json lifecycle = nlohmann::json::array();
  nlohmann::json player = nullptr;
  std::string rawLog;
  bool logTruncated = false;
  fs::path outputPath;
};
std::shared_ptr<CapturedTelemetry> activeTelemetry;

void recordTesterLine(const std::shared_ptr<CapturedTelemetry> &trace, const std::string &line) {
  static const std::string netMarker = "TELEMETRY_NETWORK ";
  static const std::string eventMarker = "TELEMETRY_EVENT ";
  const auto n = line.find(netMarker);
  const auto e = line.find(eventMarker);
  nlohmann::json parsed = nullptr;
  if (n != std::string::npos)
    parsed = nlohmann::json::parse(line.substr(n + netMarker.size()), nullptr, false);
  else if (e != std::string::npos)
    parsed = nlohmann::json::parse(line.substr(e + eventMarker.size()), nullptr, false);
  {
    std::lock_guard<std::mutex> lock(trace->mutex);
    if (!parsed.is_discarded() && parsed.is_object()) {
      if (n != std::string::npos && trace->network.size() < 5000)
        trace->network.push_back(parsed);
      else if (e != std::string::npos && trace->lifecycle.size() < 2000)
        trace->lifecycle.push_back(parsed);
    }
    if (trace->rawLog.size() + line.size() < 5 * 1024 * 1024)
      trace->rawLog += line + "\n";
    else
      trace->logTruncated = true;
  }
  if (n == std::string::npos && e == std::string::npos)
    std::cerr << line << '\n';
}

void readTesterStderr(int fd, const std::shared_ptr<CapturedTelemetry> &trace) {
  std::string remaining;
  char buf[4096];
  int quietAfterExit = 0;
  while (true) {
    pollfd p{fd, POLLIN | POLLHUP | POLLERR, 0};
    int pollResult = poll(&p, 1, 200);
    if (pollResult < 0 && errno == EINTR) continue;
    if (pollResult < 0) break;
    if (pollResult == 0) {
      if (trace->testerExited.load() && ++quietAfterExit >= 5) break;
      continue;
    }
    if (p.revents & POLLIN) {
      const ssize_t size = read(fd, buf, sizeof(buf));
      if (size <= 0) break;
      quietAfterExit = 0;
      remaining.append(buf, static_cast<std::size_t>(size));
      std::size_t newline;
      while ((newline = remaining.find('\n')) != std::string::npos) {
        recordTesterLine(trace, remaining.substr(0, newline));
        remaining.erase(0, newline+1);
      }
      if (remaining.size() > 16384) remaining.clear(); // Bound unterminated output.
    } else if (p.revents & (POLLHUP | POLLERR)) break;
  }
  if (!remaining.empty()) recordTesterLine(trace, remaining);
  close(fd);
}

void logMain(const std::string &message); // Defined below.
void persistTelemetry(const std::shared_ptr<CapturedTelemetry> &trace) {
  fs::path path;
  nlohmann::json data;
  std::string log;
  {
    std::lock_guard<std::mutex> lock(trace->mutex);
    path = trace->outputPath;
    data = {
      {"schemaVersion", 1},
      {"clockAlignment", "network/lifecycle epochMs is system clock; frontend events include epochMs and monotonic tMs; compare epochMs with clock uncertainty"},
      {"trafficAttribution", "8080 groups custom ABR and dash.js; 80/443 groups all WPR browser traffic; cannot distinguish players or individual browsers from tc filters"},
      {"networkSamples", trace->network},
      {"workloadEvents", trace->lifecycle},
      {"player", trace->player},
      {"testerLogTruncated", trace->logTruncated}
    };
    log = trace->rawLog;
  }
  if (path.empty()) return; // No QoE result saved for this run.
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output || !(output << data.dump(2) << "\n")) {
    logMain("WARNING: could not persist telemetry: " + path.generic_string());
    return;
  }
  const fs::path logPath = path.parent_path() / (path.stem().string() + "-tester.log");
  std::ofstream logOutput(logPath, std::ios::out | std::ios::trunc);
  if (logOutput) logOutput << log;
}


// Per-batch association remains in backend memory. A configuration snapshot is
// saved alongside QoE results for reproducibility, but the backend still does
// not use it to recover an unfinished batch after restarting.
struct BatchDirectoryState {
  fs::path directory;
  nlohmann::json configuration;
  std::string label;
  std::string seed;
  int total = 0;
};
std::unordered_map<std::string, BatchDirectoryState> batchDirectories;

// One human-editable assignment file controls both workload switches and ABR.
// Values are parsed, validated, and frozen once per experiment. This is a
// deliberately small "export const name = value;" subset of JavaScript.
constexpr const char *kExperimentConfigFile = "tester/experiment-parameters.js";
constexpr int kMaxBrowsers = 16;

std::string trimParameter(std::string s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

struct ExperimentConfig {
  bool customAbr = true;
  bool dashJs = true;
  bool browsersEnabled = true;
  int browserCount = 2;
  int browserLaunchIntervalMs = 2000;
  int browserStartDelayMs = 0;
  int browserStartIntervalMs = 5000;
  int browserStartJitterMs = 0;
  int repeatRuns = 5;
  std::string trafficSeed = "baseline";
  std::string resultLabel = "baseline";
  int throughputAveragingSegments = 4;
  double qualitySafetyFactor = 0.8;
  double lowBufferThresholdSec = 4;
  double bufferTargetSec = 30;
  double timeoutEtaFactor = 1.3;
  double timeoutBufferFactor = 0.25;
  int lowestQualityTimeoutMs = 15000;
  int initialDownloadTimeoutMs = 10000;
  int downloadRetryDelayMs = 3000;

  int effectiveBrowserCount() const { return browsersEnabled ? browserCount : 0; }
  nlohmann::json toJson() const {
    return {
      {"workloads", {
        {"customAbr", customAbr}, {"dashJs", dashJs},
        {"browsers", {{"enabled", browsersEnabled}, {"count", browserCount}}}
      }},
      {"browserScheduling", {
        {"launchIntervalMs", browserLaunchIntervalMs},
        {"startDelayMs", browserStartDelayMs},
        {"startIntervalMs", browserStartIntervalMs},
        {"startJitterMs", browserStartJitterMs},
        {"trafficSeed", trafficSeed}
      }},
      {"runSettings", {
        {"repeatRuns", repeatRuns}, {"trafficSeed", trafficSeed},
        {"resultLabel", resultLabel}
      }},
      {"abrParameters", {
        {"throughputAveragingSegments", throughputAveragingSegments},
        {"qualitySafetyFactor", qualitySafetyFactor},
        {"lowBufferThresholdSec", lowBufferThresholdSec},
        {"bufferTargetSec", bufferTargetSec},
        {"timeoutEtaFactor", timeoutEtaFactor},
        {"timeoutBufferFactor", timeoutBufferFactor},
        {"lowestQualityTimeoutMs", lowestQualityTimeoutMs},
        {"initialDownloadTimeoutMs", initialDownloadTimeoutMs},
        {"downloadRetryDelayMs", downloadRetryDelayMs}
      }}
    };
  }
};

bool loadExperimentConfig(ExperimentConfig &out, std::string &error) {
  std::ifstream input(kExperimentConfigFile);
  if (!input) {
    error = std::string("Cannot open ") + kExperimentConfigFile +
            " (start the backend from Main_Project root)";
    return false;
  }

  const std::unordered_set<std::string> allowed = {
    "customAbr", "dashJs", "browsersEnabled", "browserCount",
    "browserLaunchIntervalMs", "browserStartDelayMs", "browserStartIntervalMs", "browserStartJitterMs",
    "repeatRuns", "trafficSeed", "resultLabel",
    "throughputAveragingSegments", "qualitySafetyFactor", "lowBufferThresholdSec",
    "bufferTargetSec", "timeoutEtaFactor", "timeoutBufferFactor",
    "lowestQualityTimeoutMs", "initialDownloadTimeoutMs", "downloadRetryDelayMs"
  };
  std::unordered_map<std::string, std::string> values;
  std::string line;
  std::size_t lineNo = 0;
  while (std::getline(input, line)) {
    ++lineNo;
    const std::size_t slash = line.find("//");
    if (slash != std::string::npos) line.erase(slash);
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    line = trimParameter(line);
    if (line.empty()) continue;
    if (line.rfind("export const ", 0) != 0) {
      error = "Line " + std::to_string(lineNo) + ": expected export const name = value;";
      return false;
    }
    line.erase(0, 13);
    if (!line.empty() && line.back() == ';') line.pop_back();
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos || line.find('=', eq+1) != std::string::npos) {
      error = "Line " + std::to_string(lineNo) + ": expected one = assignment";
      return false;
    }
    std::string key = trimParameter(line.substr(0, eq));
    std::string val = trimParameter(line.substr(eq+1));
    if (!allowed.count(key) || val.empty() || values.count(key)) {
      error = "Line " + std::to_string(lineNo) + ": unknown, duplicate, or empty setting: " + key;
      return false;
    }
    values.emplace(std::move(key), std::move(val));
  }
  if (!input.eof() || values.size() != allowed.size()) {
    error = "Incomplete/unreadable parameter file (expected all 20 named settings)";
    return false;
  }

  auto boolValue = [&](const char *key, bool &destination) {
    const auto &raw = values.at(key);
    if (raw == "true") { destination = true; return true; }
    if (raw == "false") { destination = false; return true; }
    error = std::string(key) + " must be true or false";
    return false;
  };
  auto intValue = [&](const char *key, int &destination, int minimum, int maximum) {
    const auto &raw = values.at(key);
    try {
      std::size_t pos = 0;
      const long long parsed = std::stoll(raw, &pos, 10);
      if (pos == raw.size() && parsed >= minimum && parsed <= maximum) {
        destination = static_cast<int>(parsed); return true;
      }
    } catch (const std::exception &) {}
    error = std::string(key) + " must be an integer in [" +
            std::to_string(minimum) + "," + std::to_string(maximum) + "]";
    return false;
  };
  auto doubleValue = [&](const char *key, double &destination, double min, double max) {
    const auto &raw = values.at(key);
    try {
      std::size_t pos = 0;
      const double parsed = std::stod(raw, &pos);
      if (pos == raw.size() && std::isfinite(parsed) && parsed >= min && parsed <= max) {
        destination = parsed; return true;
      }
    } catch (const std::exception &) {}
    error = std::string(key) + " must be a finite number in [" +
            std::to_string(min) + "," + std::to_string(max) + "]";
    return false;
  };
  auto stringValue = [&](const char *key, std::string &destination) {
    const auto &raw = values.at(key);
    if (raw.size() < 3 || raw.front() != '"' || raw.back() != '"') {
      error = std::string(key) + " must be a quoted string";
      return false;
    }
    const std::string value = raw.substr(1, raw.size() - 2);
    if (value.empty() || value.size() > 64 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        })) {
      error = std::string(key) + " must contain 1..64 characters: letters, digits, underscore or hyphen";
      return false;
    }
    destination = value;
    return true;
  };
  if (!boolValue("customAbr", out.customAbr) ||
      !boolValue("dashJs", out.dashJs) ||
      !boolValue("browsersEnabled", out.browsersEnabled) ||
      !intValue("browserCount", out.browserCount, 0, kMaxBrowsers) ||
      !intValue("browserLaunchIntervalMs", out.browserLaunchIntervalMs, 0, 600000) ||
      !intValue("browserStartDelayMs", out.browserStartDelayMs, 0, 3600000) ||
      !intValue("browserStartIntervalMs", out.browserStartIntervalMs, 0, 3600000) ||
      !intValue("browserStartJitterMs", out.browserStartJitterMs, 0, 3600000) ||
      !intValue("repeatRuns", out.repeatRuns, 1, 20) ||
      !stringValue("trafficSeed", out.trafficSeed) ||
      !stringValue("resultLabel", out.resultLabel) ||
      !intValue("throughputAveragingSegments", out.throughputAveragingSegments, 1, 1000) ||
      !doubleValue("qualitySafetyFactor", out.qualitySafetyFactor, 0.01, 10.0) ||
      !doubleValue("lowBufferThresholdSec", out.lowBufferThresholdSec, 0, 120) ||
      !doubleValue("bufferTargetSec", out.bufferTargetSec, 0.01, 120) ||
      !doubleValue("timeoutEtaFactor", out.timeoutEtaFactor, 0, 100) ||
      !doubleValue("timeoutBufferFactor", out.timeoutBufferFactor, 0, 100) ||
      !intValue("lowestQualityTimeoutMs", out.lowestQualityTimeoutMs, 1, 300000) ||
      !intValue("initialDownloadTimeoutMs", out.initialDownloadTimeoutMs, 1, 300000) ||
      !intValue("downloadRetryDelayMs", out.downloadRetryDelayMs, 0, 300000))
    return false;
  if (out.lowBufferThresholdSec > out.bufferTargetSec) {
    error = "lowBufferThresholdSec cannot exceed bufferTargetSec";
    return false;
  }
  if (!out.customAbr && out.repeatRuns > 1) {
    error = "repeatRuns > 1 requires customAbr=true (no custom-player QoE otherwise)";
    return false;
  }
  if (!out.customAbr && !out.dashJs && out.effectiveBrowserCount() == 0) {
    error = "All workloads disabled; enable at least one";
    return false;
  }
  return true;
}

// Preserve the immutable parameter snapshot from /experiment/start.
// "enabled" is a requested workload setting, not proof that an individual
// player or container remained online throughout the experiment.
nlohmann::json savedConfiguration(const nlohmann::json &config,
                                  bool testerPrepared, bool goSent) {
  const auto &workloads = config.at("workloads");
  const auto &browsers = workloads.at("browsers");
  const bool browserGroupEnabled = browsers.at("enabled").get<bool>();
  const int configuredCount = browsers.at("count").get<int>();
  const int effectiveCount = browserGroupEnabled ? configuredCount : 0;
  const bool browsersActive = effectiveCount > 0;
  const bool abr = workloads.at("customAbr").get<bool>();
  const bool dash = workloads.at("dashJs").get<bool>();

  nlohmann::json instances = nlohmann::json::array();
  for (int slot = 1; slot <= configuredCount; ++slot) {
    instances.push_back({{"slot", slot},
                         {"enabled", browserGroupEnabled},
                         {"status", browserGroupEnabled ? "enabled" : "disabled"}});
  }

  return {
    {"snapshotVersion", 1},
    {"note", "Captured from the frozen experiment configuration. Enabled/disabled is a configured setting, not independent proof of live connectivity or successful playback."},
    {"workloadStatus", {
      {"customAbr", {{"enabled", abr}, {"status", abr ? "enabled" : "disabled"}}},
      {"dashJs", {{"enabled", dash}, {"status", dash ? "enabled" : "disabled"}}},
      {"browserSimulators", {
        {"enabled", browsersActive},
        {"groupSwitchEnabled", browserGroupEnabled},
        {"configuredCount", configuredCount},
        {"effectiveCount", effectiveCount},
        {"status", browsersActive ? "enabled" : "disabled"},
        {"instances", std::move(instances)}
      }},
      {"webPageReplay", {{"enabled", browsersActive},
                         {"status", browsersActive ? "enabled" : "disabled"}}}
    }},
    {"startupSignalsAtSave", {
      {"testerPrepared", testerPrepared},
      {"goCommandSent", goSent},
      {"individualWorkloadHealthVerified", false}
    }},
    {"abrParameters", config.at("abrParameters")},
    {"browserScheduling", config.at("browserScheduling")},
    {"runSettings", config.at("runSettings")},
    {"configuredWorkloads", config.at("workloads")}
  };
}

// Diagnostic-only logger. Does not change the experiment protocol or traffic shaping.
std::mutex diagnosticLogMutex;
void logMain(const std::string &message) {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
  std::tm local{};
  localtime_r(&time, &local);
  std::lock_guard<std::mutex> lock(diagnosticLogMutex);
  std::cerr << "[" << std::put_time(&local, "%H:%M:%S") << "."
            << std::setfill('0') << std::setw(3) << ms
            << "] [main] " << message << '\n';
}

std::string runLabel(std::uint64_t runId) {
  return "run=" + std::to_string(runId) + " ";
}


bool safeIdentifier(const std::string &value, std::size_t maxLength = 64) {
  if (value.empty() || value.size() > maxLength) return false;
  for (unsigned char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
  }
  return true;
}

// Folder sequence comes from existing directories on disk, never browser storage.
// Numeric-prefix parsing also recognizes historical folders such as 008_2026-11-21_14-45-03.
std::uint64_t folderNumber(const std::string &name) {
  const auto separator = name.find('_');
  if (separator == std::string::npos || separator == 0) return 0;
  std::uint64_t number = 0;
  for (std::size_t i = 0; i < separator; ++i) {
    const char digit = name[i];
    if (digit < '0' || digit > '9') return 0;
    const auto value = static_cast<unsigned>(digit - '0');
    if (number > (std::numeric_limits<std::uint64_t>::max() - value) / 10)
      return 0;
    number = number * 10 + value;
  }
  return number;
}

// Called while testerControlMutex is held. Filesystem directory creation is
// atomic, so even an unexpected collision cannot overwrite prior results.
fs::path createNumberedDirectory(const fs::path &root,
                                 const std::string &suffix,
                                 std::error_code &error) {
  std::uint64_t highest = 0;
  fs::directory_iterator it(root, error), end;
  if (error) return {};
  for (; it != end; it.increment(error)) {
    if (error) return {};
    if (!it->is_directory(error)) {
      if (error) return {};
      continue;
    }
    highest = std::max(highest, folderNumber(it->path().filename().string()));
  }
  if (highest == std::numeric_limits<std::uint64_t>::max()) {
    error = std::make_error_code(std::errc::value_too_large);
    return {};
  }
  for (std::uint64_t next = highest + 1; next > highest; ++next) {
    std::ostringstream folder;
    folder << std::setw(3) << std::setfill('0') << next << '_' << suffix;
    const fs::path directory = root / folder.str();
    if (fs::create_directory(directory, error)) return directory;
    if (error != std::errc::file_exists) return {};
    error.clear();
    if (next == std::numeric_limits<std::uint64_t>::max()) break;
  }
  error = std::make_error_code(std::errc::value_too_large);
  return {};
}

// Create-once file writing: never silently overwrite an existing measurement.
// The backend is local research infrastructure, but recording should still be durable.
bool writeNewResult(const fs::path &path, const std::string &body, std::string &error) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd == -1) {
    error = std::strerror(errno);
    return false;
  }
  std::size_t written = 0;
  bool ok = true;
  while (written < body.size()) {
    const ssize_t n = write(fd, body.data() + written, body.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { error = n < 0 ? std::strerror(errno) : "Short write"; ok = false; break; }
    written += static_cast<std::size_t>(n);
  }
  if (ok && fsync(fd) != 0) { error = std::strerror(errno); ok = false; }
  if (close(fd) != 0) { error = std::strerror(errno); ok = false; }
  if (!ok) unlink(path.c_str());
  return ok;
}

bool validQoe(const nlohmann::json &qoe) {
  if (!qoe.is_object()) return false;
  for (const char *field : {"playbackDurationTotal480Sec", "playbackDurationTotal720Sec",
                            "playbackDurationTotal1080Sec", "startupDelayMs",
                            "stallCountTotal", "stallDurationTotalMs", "qualitySwitchCount",
                            "abortedDownloadCount"}) {
    if (!qoe.contains(field) || !qoe[field].is_number()) return false;
    const double value = qoe[field].get<double>();
    if (!std::isfinite(value) || value < 0) return false;
  }
  return qoe.contains("qualitySwitchHistoryArr") && qoe["qualitySwitchHistoryArr"].is_array() &&
         qoe.contains("abortHistoryArr") && qoe["abortHistoryArr"].is_array();
}

// Preserve the requested resolution order in JSON files and API responses.
// JSON object order has no semantic meaning, but ordered_json retains this
// presentation order instead of nlohmann::json's sorted-map order.
nlohmann::ordered_json orderedQoe(const nlohmann::json &qoe) {
  nlohmann::ordered_json ordered = nlohmann::ordered_json::object();
  for (const char *key : {
        "playbackDurationTotal1080Sec",
        "playbackDurationTotal720Sec",
        "playbackDurationTotal480Sec"}) {
    if (qoe.contains(key)) ordered[key] = qoe.at(key);
  }
  for (auto it = qoe.begin(); it != qoe.end(); ++it) {
    if (!ordered.contains(it.key())) ordered[it.key()] = it.value();
  }
  return ordered;
}

// Human-readable companion report. The original JSON is kept unchanged so
// scripts/averages can continue consuming numeric values without parsing text.
std::string formatQoeReport(const nlohmann::json &qoe, const std::string &title,
                            bool isAverage) {
  auto num = [&](const char *key) -> double {
    return qoe.contains(key) && qoe[key].is_number() ? qoe[key].get<double>() : 0.0;
  };
  auto fmt = [](double value, int decimals = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
  };
  auto cell = [](std::string value) {
    for (char &ch : value)
      if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
    if (value.size() > 160) value.resize(160);
    return value;
  };
  auto quality = [](int id) -> std::string {
    switch (id) {
      case 0: return "480p";
      case 1: return "720p";
      case 2: return "1080p";
      default: return "unknown";
    }
  };
  auto getInteger = [](const nlohmann::json &event, const char *field) -> int {
    return event.is_object() && event.contains(field) && event[field].is_number_integer()
      ? event[field].get<int>() : -1;
  };
  auto getString = [](const nlohmann::json &event, const char *field) -> std::string {
    return event.is_object() && event.contains(field) && event[field].is_string()
      ? event[field].get<std::string>() : "unknown";
  };
  auto getDouble = [](const nlohmann::json &event, const char *field) -> double {
    return event.is_object() && event.contains(field) && event[field].is_number()
      ? event[field].get<double>() : 0.0;
  };

  const double seconds1080 = num("playbackDurationTotal1080Sec");
  const double seconds720 = num("playbackDurationTotal720Sec");
  const double seconds480 = num("playbackDurationTotal480Sec");
  const double total = seconds1080 + seconds720 + seconds480;
  std::ostringstream out;
  out << "# " << cell(title) << " — ABR experiment result\n\n";
  out << "**" << (num("stallCountTotal") == 0 ? "Zero stalls" : "Stalls recorded")
      << "** · " << fmt(total) << " s measured playback · "
      << (total > 0 ? fmt(seconds1080 * 100.0 / total, 1) : "0.0")
      << "% at 1080p\n\n";
  out << "## Playback by resolution\n\n";
  out << "| Resolution | Duration | Share |\n|---|---:|---:|\n";
  for (const auto &item : {std::pair<const char*, double>{"1080p", seconds1080},
                           {"720p", seconds720}, {"480p", seconds480}}) {
    out << "| " << item.first << " | " << fmt(item.second) << " s | "
        << (total > 0 ? fmt(item.second * 100.0 / total, 1) : "0.0") << "% |\n";
  }
  out << "\n## Playback performance\n\n";
  out << "| Metric | " << (isAverage ? "Mean per run" : "Value") << " |\n|---|---:|\n";
  out << "| Startup delay | " << fmt(num("startupDelayMs") / 1000.0) << " s |\n";
  out << "| Stall count | " << (isAverage ? fmt(num("stallCountTotal")) : fmt(num("stallCountTotal"), 0)) << " |\n";
  out << "| Total stall duration | " << fmt(num("stallDurationTotalMs") / 1000.0) << " s |\n";
  out << "| Quality switches | " << (isAverage ? fmt(num("qualitySwitchCount")) : fmt(num("qualitySwitchCount"), 0)) << " |\n";
  out << "| Aborted downloads | " << (isAverage ? fmt(num("abortedDownloadCount")) : fmt(num("abortedDownloadCount"), 0)) << " |\n\n";
  if (!isAverage) {
    out << "## Quality switch history\n\n";
    if (!qoe.contains("qualitySwitchHistoryArr") || qoe["qualitySwitchHistoryArr"].empty()) {
      out << "No quality switches.\n\n";
    } else {
      out << "| Time | From | To | Reason |\n|---:|---|---|---|\n";
      for (const auto &event : qoe["qualitySwitchHistoryArr"]) {
        out << "| " << fmt(getDouble(event, "videoTimeSec")) << " s | "
            << quality(getInteger(event, "from")) << " | "
            << quality(getInteger(event, "to")) << " | "
            << cell(getString(event, "reason")) << " |\n";
      }
      out << "\n";
    }
    out << "## Aborted download history\n\n";
    if (!qoe.contains("abortHistoryArr") || qoe["abortHistoryArr"].empty()) {
      out << "No aborted downloads.\n\n";
    } else {
      out << "| Segment | Quality | Elapsed | Reason |\n|---:|---|---:|---|\n";
      for (const auto &event : qoe["abortHistoryArr"]) {
        out << "| " << getInteger(event, "segmentNumber") << " | "
            << quality(getInteger(event, "quality")) << " | "
            << fmt(getDouble(event, "elapsedMs") / 1000.0) << " s | "
            << cell(getString(event, "abortReason")) << " |\n";
      }
      out << "\n";
    }
  } else {
    out << "_This report uses arithmetic means across completed runs; histories are not averaged._\n\n";
  }
  out << "_Resolution duration counters follow representation IDs: 0 = 480p, 1 = 720p, 2 = 1080p._\n";
  return out.str();
}

extern char **environ;

// ==================== 2. WAIT FOR TESTER STARTUP ====================

// Tester sends P for preparation progress, R when all journeys report READY,
// or E on failure. 180s without any progress times out; no fixed total limit.
bool waitForTesterReady(int startupReadFd, bool &startupTimedOut, std::uint64_t runId) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
  startupTimedOut = false;
  logMain(runLabel(runId) + "waiting for tester readiness (P=progress, R=ready, E=error)");

  while (true) {
    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now()).count();
    if (remainingMs <= 0) {
      startupTimedOut = true;
      logMain(runLabel(runId) + "ERROR: 180s without a tester readiness/progress byte");
      return false;
    }

    pollfd startupEvent{startupReadFd, POLLIN, 0};
    // Five-second diagnostic heartbeats; original 180-second inactivity deadline remains.
    const int timeoutMs = static_cast<int>(std::min<long long>(remainingMs, 5000));
    const int pollResult = poll(&startupEvent, 1, timeoutMs);
    if (pollResult == -1 && errno == EINTR)
      continue;
    if (pollResult == -1) {
      logMain(runLabel(runId) + "ERROR: readiness poll failed: " + std::strerror(errno));
      return false;
    }
    if (pollResult == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        startupTimedOut = true;
        logMain(runLabel(runId) + "ERROR: tester readiness inactivity timeout");
        return false;
      }
      logMain(runLabel(runId) + "still waiting for tester; no readiness byte in last 5s");
      continue;
    }

    if (startupEvent.revents & (POLLIN | POLLHUP)) {
      char startupByte = 0;
      ssize_t bytesRead;
      do {
        bytesRead = read(startupReadFd, &startupByte, 1);
      } while (bytesRead == -1 && errno == EINTR);
      if (bytesRead != 1) {
        logMain(runLabel(runId) + (bytesRead == 0
          ? "ERROR: tester closed readiness pipe (EOF) before READY"
          : "ERROR: reading tester readiness pipe failed: " + std::string(std::strerror(errno))));
        return false;
      }
      if (startupByte == 'R') {
        logMain(runLabel(runId) + "tester reported READY (R)");
        return true;
      }
      if (startupByte == 'E') {
        logMain(runLabel(runId) + "ERROR: tester reported startup failure (E)");
        return false;
      }
      if (startupByte != 'P') {
        logMain(runLabel(runId) + "ERROR: unexpected tester status byte " +
                std::to_string(static_cast<unsigned char>(startupByte)));
        return false;
      }
      logMain(runLabel(runId) + "tester reported preparation progress (P)");
      // Original protocol: progress resets the 180-second inactivity deadline.
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
      continue;
    }
    if (startupEvent.revents & (POLLERR | POLLNVAL)) {
      logMain(runLabel(runId) + "ERROR: readiness pipe poll events=" +
              std::to_string(startupEvent.revents));
      return false;
    }
  }
}

// ==================== 3. BACKEND AND MEDIA FILES ====================

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  httplib::Server server;
  // Left path is the browser URL; right path is relative to backend's working directory.
  bool segmentsMounted = server.set_mount_point("/segments", "./segments");
  // Do NOT expose tester/: it now contains private research measurements.
  logMain(std::string("backend starting; cwd=") + fs::current_path().string());
  logMain(std::string("media mount /segments: ") + (segmentsMounted ? "OK" : "FAILED"));
  if (segmentsMounted) {
    server.set_file_extension_and_mimetype_mapping("mpd", "application/dash+xml");
    server.set_file_extension_and_mimetype_mapping("mp4", "video/mp4");
    server.set_file_extension_and_mimetype_mapping("m4s", "video/iso.segment");
  }

  // ==================== 4. BROWSER ACCESS (CORS) ====================

  server.set_post_routing_handler(
      [](const httplib::Request &request, httplib::Response &httpResponse) {
        const std::string origin = request.get_header_value("Origin");
        if (origin == "http://172.19.176.1:5173" || origin == "http://localhost:5173" ||
            origin == "http://127.0.0.1:5173") {
          httpResponse.set_header("Access-Control-Allow-Origin", origin);
          httpResponse.set_header("Vary", "Origin");
        }
        if (request.path.rfind("/experiment/", 0) == 0)
          httpResponse.set_header("Cache-Control", "no-store");
      });

  // Frontend reads this before starting any player. A run gets its own
  // immutable snapshot at /experiment/start (see configuration comparison).
  server.Options("/experiment/config", [](const httplib::Request &, httplib::Response &response) {
    response.set_header("Access-Control-Allow-Headers", "Content-Type, Cache-Control, Pragma");
    response.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
    response.status = 204;
  });
  server.Get("/experiment/config", [](const httplib::Request &, httplib::Response &response) {
    ExperimentConfig config;
    std::string error;
    if (!loadExperimentConfig(config, error)) {
      response.status = 500;
      response.set_content(nlohmann::json{{"error", error}}.dump(), "application/json");
      return;
    }
    response.set_header("Cache-Control", "no-store");
    response.set_content(config.toJson().dump(), "application/json");
  });

  // ==================== 5. START EXPERIMENT ====================

  // OPTIONS answers the browser's permission check before its POST request.
  server.Options("/experiment/start",
                 [](const httplib::Request &request, httplib::Response &httpResponse) {
                   httpResponse.set_header("Access-Control-Allow-Headers", "Content-Type");
                   httpResponse.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                   httpResponse.status = 200;
                 });
  // POST starts tester and replies only after receiving its startup byte.
  server.Post(
      "/experiment/start", [&](const httplib::Request &request, httplib::Response &httpResponse) {
        // Validate before claiming the experiment slot; reject stale browser config.
        logMain("POST /experiment/start received");
        ExperimentConfig experimentConfig;
        std::string configError;
        if (!loadExperimentConfig(experimentConfig, configError)) {
          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", configError}}.dump(), "application/json");
          return;
        }
        const auto requestJson = nlohmann::json::parse(request.body, nullptr, false);
        if (!requestJson.is_object() || !requestJson.contains("config") ||
            requestJson["config"] != experimentConfig.toJson()) {
          httpResponse.status = 409;
          httpResponse.set_content(R"({"error":"Experiment configuration changed; reload the frontend"})",
                                   "application/json");
          return;
        }
        if (isExperimentRunning.exchange(true)) {
          logMain("POST /experiment/start REJECTED: experiment already running");
          httpResponse.status = 409;
          httpResponse.set_content(R"({"error": "An Experiment is already running"})",
                                   "application/json");
          return;
        }

        std::uint64_t thisRunId;
        {
          std::lock_guard<std::mutex> lock(testerControlMutex);
          thisRunId = ++nextRunId;
          activeRunId = thisRunId;
          resultSavedForActiveRun = false;
          activeExperimentConfig = experimentConfig.toJson();
          activeTelemetry = std::make_shared<CapturedTelemetry>();
        }
        logMain(runLabel(thisRunId) + "experiment slot reserved; creating control/status pipes");

        // Step 2: create two one-way pipes. Index 0 reads; index 1 writes.
        // stopCommandPipe: main writes cancellation; tester reads it on stdin.
        // startupStatusPipe: tester writes R/E on stdout; main reads it.
        int stopCommandPipe[2];

        if (pipe(stopCommandPipe) == -1) {
          int operationError = errno;
          logMain(runLabel(thisRunId) + "ERROR: control pipe creation: " + std::strerror(operationError));
          isExperimentRunning.store(false);

          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(operationError)}}.dump(),
                                   "application/json");
          return;
        }
        int startupStatusPipe[2];
        if (pipe(startupStatusPipe) == -1) {
          int operationError = errno;
          logMain(runLabel(thisRunId) + "ERROR: readiness pipe creation: " + std::strerror(operationError));
          close(stopCommandPipe[0]);
          close(stopCommandPipe[1]);
          isExperimentRunning.store(false);

          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(operationError)}}.dump(),
                                   "application/json");
          return;
        }

        // A third one-way pipe captures tester stderr without changing its
        // byte-oriented readiness protocol on stdout.
        int stderrPipe[2];
        if (pipe(stderrPipe) == -1) {
          const int code = errno;
          close(stopCommandPipe[0]); close(stopCommandPipe[1]);
          close(startupStatusPipe[0]); close(startupStatusPipe[1]);
          isExperimentRunning.store(false);
          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(code)}}.dump(), "application/json");
          return;
        }

        // Step 3: build instructions for the child's descriptors, applied during spawn.
        // These addclose calls schedule closes in tester, not in main.cpp.
        posix_spawn_file_actions_t testerIoActions;

        int ioSetupError = posix_spawn_file_actions_init(&testerIoActions);

        if (ioSetupError != 0) {
          logMain(runLabel(thisRunId) + "ERROR: spawn file actions init: " + std::strerror(ioSetupError));
          close(stopCommandPipe[0]);
          close(stopCommandPipe[1]);
          close(startupStatusPipe[0]);
          close(startupStatusPipe[1]);
          close(stderrPipe[0]); close(stderrPipe[1]);
          isExperimentRunning.store(false);

          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(ioSetupError)}}.dump(),
                                   "application/json");
          return;
        }

        // Make tester's stdin another descriptor for the stop pipe's reading end.
        ioSetupError =
            posix_spawn_file_actions_adddup2(&testerIoActions, stopCommandPipe[0], STDIN_FILENO);

        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_addclose(&testerIoActions, stopCommandPipe[0]);
        }

        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_addclose(&testerIoActions, stopCommandPipe[1]);
        }

        if (ioSetupError == 0) {
          // Make tester's stdout another descriptor for the startup pipe's writing end.
          ioSetupError = posix_spawn_file_actions_adddup2(
              &testerIoActions, startupStatusPipe[1], STDOUT_FILENO);
        }

        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_addclose(&testerIoActions, startupStatusPipe[0]);
        }

        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_addclose(&testerIoActions, startupStatusPipe[1]);
        }
        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_adddup2(&testerIoActions, stderrPipe[1], STDERR_FILENO);
        }
        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_addclose(&testerIoActions, stderrPipe[0]);
        }
        if (ioSetupError == 0) {
          ioSetupError = posix_spawn_file_actions_addclose(&testerIoActions, stderrPipe[1]);
        }

        if (ioSetupError != 0) {
          logMain(runLabel(thisRunId) + "ERROR: spawn file actions setup: " + std::strerror(ioSetupError));
          posix_spawn_file_actions_destroy(&testerIoActions);
          close(stopCommandPipe[0]);
          close(stopCommandPipe[1]);
          close(startupStatusPipe[0]);
          close(startupStatusPipe[1]);
          close(stderrPipe[0]); close(stderrPipe[1]);
          isExperimentRunning.store(false);

          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(ioSetupError)}}.dump(),
                                   "application/json");
          return;
        }

        // Step 4: launch sudo -n /usr/local/bin/main-project-tester.
        // -n makes sudo fail instead of waiting for a password in the backend.
        pid_t testerPid;

        char sudoArg[] = "sudo";
        char nonInteractiveArg[] = "-n";
        char testerPathArg[] = "/usr/local/bin/main-project-tester";
        // Positional CLI arguments are validated server-side and frozen per run.
        // Tester never rereads a file that could be edited during startup.
        const std::string browserCountArg = std::to_string(experimentConfig.effectiveBrowserCount());
        const std::string customAbrArg = experimentConfig.customAbr ? "1" : "0";
        const std::string dashArg = experimentConfig.dashJs ? "1" : "0";
        const std::string launchArg = std::to_string(experimentConfig.browserLaunchIntervalMs);
        const std::string delayArg = std::to_string(experimentConfig.browserStartDelayMs);
        const std::string intervalArg = std::to_string(experimentConfig.browserStartIntervalMs);
        const std::string jitterArg = std::to_string(experimentConfig.browserStartJitterMs);
        const std::string seedArg = experimentConfig.trafficSeed;
        char *testerCommandArgs[] = {
          sudoArg, nonInteractiveArg, testerPathArg,
          const_cast<char *>(browserCountArg.c_str()),
          const_cast<char *>(customAbrArg.c_str()),
          const_cast<char *>(dashArg.c_str()),
          const_cast<char *>(launchArg.c_str()),
          const_cast<char *>(delayArg.c_str()),
          const_cast<char *>(intervalArg.c_str()),
          const_cast<char *>(jitterArg.c_str()),
          const_cast<char *>(seedArg.c_str()), nullptr
        };

        logMain(runLabel(thisRunId) + "spawning sudo -n /usr/local/bin/main-project-tester");
        int operationError = posix_spawn(
            &testerPid, "/usr/bin/sudo", &testerIoActions, nullptr, testerCommandArgs, environ);
        // Spawn has used the instructions; destroying the list does not stop tester.
        posix_spawn_file_actions_destroy(&testerIoActions);

        // Parent keeps only its stop writer and startup reader.
        close(stopCommandPipe[0]);
        close(startupStatusPipe[1]);
        close(stderrPipe[1]);

        if (operationError != 0) {
          logMain(runLabel(thisRunId) + "ERROR: posix_spawn: " + std::strerror(operationError));
          close(stopCommandPipe[1]);
          close(startupStatusPipe[0]);
          close(stderrPipe[0]);
          isExperimentRunning.store(false);

          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(operationError)}}.dump(),
                                   "application/json");
          return;
        }

        logMain(runLabel(thisRunId) + "spawned sudo PID=" + std::to_string(testerPid));
        // Step 5: publish the parent's stop writer for the /experiment/stop route.
        {
          std::lock_guard<std::mutex> lock(testerControlMutex);

          testerStopWriteFd = stopCommandPipe[1];
          stopCommandSent = false;
          startCommandSent = false;
          testerPrepared = false;
        }
        logMain(runLabel(thisRunId) + "control pipe published; starting tester exit watcher");
        const auto telemetryForRun = activeTelemetry;
        std::thread stderrReader(readTesterStderr, stderrPipe[0], telemetryForRun);

        // Step 6: a background thread waits for tester to finish and collects its status.
        // waitpid blocks this thread, leaving the HTTP request threads available.
        std::thread([testerPid, thisRunId, telemetryForRun,
                     stderrReader = std::move(stderrReader)]() mutable {
          int exitStatus;
          pid_t waitResult;

          // EINTR interrupts the wait call, so retry it for the same child.
          do {
            waitResult = waitpid(testerPid, &exitStatus, 0);
          } while (waitResult == -1 && errno == EINTR);

          int exitCode = -1;
          bool exitedSuccessfully = false;
          if (waitResult == -1) {
            logMain(runLabel(thisRunId) + "ERROR: waitpid: " + std::strerror(errno));
          } else if (WIFEXITED(exitStatus)) {
            exitCode = WEXITSTATUS(exitStatus);
            exitedSuccessfully = exitCode == 0;
            logMain(runLabel(thisRunId) + "tester PID=" + std::to_string(testerPid) +
                    " exited with code " + std::to_string(exitCode));
          } else if (WIFSIGNALED(exitStatus)) {
            logMain(runLabel(thisRunId) + "tester terminated by signal " +
                    std::to_string(WTERMSIG(exitStatus)));
          }
          telemetryForRun->testerExited.store(true);
          if (stderrReader.joinable()) stderrReader.join();
          // The QoE response already recorded the result path and player trace.
          // Save only when the tester is actually finished, including cleanup events.
          persistTelemetry(telemetryForRun);
          {
            std::lock_guard<std::mutex> lock(testerControlMutex);
            if (testerStopWriteFd != -1) {
              close(testerStopWriteFd);
              testerStopWriteFd = -1;
            }
            stopCommandSent = false;
            startCommandSent = false;
            testerPrepared = false;
            lastFinishedRunId = thisRunId;
            lastTesterExitCode = exitCode;
            lastTesterExitSuccessful = exitedSuccessfully;
            activeRunId = 0;
            activeExperimentConfig = nullptr;
            activeTelemetry.reset();
            isExperimentRunning.store(false);
          }
          logMain(runLabel(thisRunId) + "tester exit watcher cleared running state");
        }).detach();

        // Step 7: wait for network, WPR, and all selected browsers to be ready.
        bool startupTimedOut = false;
        const bool testerReady = waitForTesterReady(startupStatusPipe[0], startupTimedOut, thisRunId);
        close(startupStatusPipe[0]);

        if (!testerReady) {
          logMain(runLabel(thisRunId) + "startup failed; closing control pipe; HTTP " +
                  std::to_string(startupTimedOut ? 504 : 500));
          // Closing our stop writer tells tester to cancel when it reads EOF.
          {
            std::lock_guard<std::mutex> lock(testerControlMutex);
            if (testerStopWriteFd != -1) {
              close(testerStopWriteFd);
              testerStopWriteFd = -1;
            }
            stopCommandSent = false;
            startCommandSent = false;
            testerPrepared = false;
          }

          httpResponse.status = startupTimedOut ? 504 : 500;
          httpResponse.set_content(R"({"error":"Tester did not confirm network readiness"})",
                                   "application/json");
          return;
        }

        // Step 8: signal PREPARED; actual browsing awaits /experiment/go.
        {
          std::lock_guard<std::mutex> lock(testerControlMutex);
          if (!isExperimentRunning.load() || testerStopWriteFd == -1 || stopCommandSent) {
            logMain(runLabel(thisRunId) + "ERROR: tester exited/cancelled after READY; HTTP 409");
            httpResponse.status = 409;
            httpResponse.set_content(R"({"error":"Tester exited or experiment was cancelled"})",
                                     "application/json");
            return;
          }
          testerPrepared = true;
        }
        nlohmann::json startupResponse = {{"status", "ready"}, {"success", true},
                                           {"runId", thisRunId},
                                           {"config", experimentConfig.toJson()}};

        logMain(runLabel(thisRunId) + "PREPARED; POST /experiment/start -> 200");
        httpResponse.status = 200;
        httpResponse.set_content(startupResponse.dump(), "application/json");
      });

  // ==================== 5B. RELEASE PREPARED BROWSERS ====================

  server.Options("/experiment/go",
                 [](const httplib::Request &request, httplib::Response &httpResponse) {
                   httpResponse.set_header("Access-Control-Allow-Headers", "Content-Type");
                   httpResponse.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                   httpResponse.status = 204;
                 });

  server.Post(
      "/experiment/go", [](const httplib::Request &request, httplib::Response &httpResponse) {
        std::lock_guard<std::mutex> lock(testerControlMutex);
        logMain(runLabel(activeRunId) + "POST /experiment/go received; prepared=" +
                std::to_string(testerPrepared) + " running=" +
                std::to_string(isExperimentRunning.load()));
        if (!isExperimentRunning.load() || testerStopWriteFd == -1 || !testerPrepared) {
          httpResponse.status = 409;
          httpResponse.set_content(R"({"error":"Experiment is not prepared"})", "application/json");
          return;
        }
        if (stopCommandSent) {
          httpResponse.status = 409;
          httpResponse.set_content(R"({"error":"Experiment is stopping"})", "application/json");
          return;
        }
        if (startCommandSent) {
          httpResponse.status = 200;
          httpResponse.set_content(R"({"status":"already_started"})", "application/json");
          return;
        }

        // Tester reads 's' from its existing stdin pipe and broadcasts START.
        const char startCommand = 's';
        ssize_t written;
        do {
          written = write(testerStopWriteFd, &startCommand, 1);
        } while (written == -1 && errno == EINTR);
        if (written != 1) {
          logMain(runLabel(activeRunId) + "ERROR: unable to send GO byte to tester");
          httpResponse.status = 500;
          httpResponse.set_content(R"({"error":"Could not signal Tester"})", "application/json");
          return;
        }
        startCommandSent = true;
        logMain(runLabel(activeRunId) + "GO byte sent; /experiment/go -> 200");
        httpResponse.status = 200;
        httpResponse.set_content(R"({"status":"started"})", "application/json");
      });

  // ==================== 5C. STATUS AND DURABLE RESULTS ====================

  // Return only once tester has actually exited (not merely acknowledged stop).
  server.Options("/experiment/status", [](const httplib::Request &, httplib::Response &response) {
    response.set_header("Access-Control-Allow-Headers", "Content-Type, Cache-Control, Pragma");
    response.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
    response.status = 204;
  });
  server.Get("/experiment/status", [](const httplib::Request &, httplib::Response &response) {
    std::lock_guard<std::mutex> lock(testerControlMutex);
    response.set_content(nlohmann::json{
      {"running", isExperimentRunning.load()},
      {"prepared", testerPrepared},
      {"started", startCommandSent},
      {"stopRequested", stopCommandSent},
      {"activeRunId", activeRunId},
      {"lastFinishedRunId", lastFinishedRunId},
      {"lastTesterExitCode", lastTesterExitCode},
      {"lastTesterExitSuccessful", lastTesterExitSuccessful},
      {"resultSaved", resultSavedForActiveRun}
    }.dump(), "application/json");
  });

  server.Options("/experiment/result", [](const httplib::Request &, httplib::Response &response) {
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.status = 204;
  });

  server.Post("/experiment/result", [](const httplib::Request &request, httplib::Response &response) {
    if (request.body.size() > 2 * 1024 * 1024) {
      response.status = 413;
      response.set_content(R"({"error":"Result payload is too large"})", "application/json");
      return;
    }
    const auto payload = nlohmann::json::parse(request.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object() ||
        !payload.contains("runId") || !payload["runId"].is_number_unsigned() ||
        !payload.contains("qoe") || !validQoe(payload["qoe"]) ||
        !payload.contains("parameters") || !payload["parameters"].is_object()) {
      response.status = 400;
      response.set_content(R"({"error":"Invalid result JSON"})", "application/json");
      return;
    }
    // First identify an ended/failed run. A child exit clears activeExperimentConfig;
    // that must not be mislabeled as a player-parameter mismatch.
    if (payload.contains("telemetry") && !payload["telemetry"].is_null() &&
        (!payload["telemetry"].is_object() ||
         !payload["telemetry"].contains("events") ||
         !payload["telemetry"]["events"].is_array() ||
         payload["telemetry"]["events"].size() > 1500)) {
      response.status = 400;
      response.set_content(R"({"error":"Invalid player telemetry"})", "application/json");
      return;
    }
    const std::uint64_t requestedRunId = payload["runId"].get<std::uint64_t>();
    std::lock_guard<std::mutex> lock(testerControlMutex);
    if (!isExperimentRunning.load() || activeRunId != requestedRunId ||
        !activeExperimentConfig.is_object()) {
      response.status = 409;
      response.set_content(R"({"error":"Experiment ended before QoE save; check tester process exit and backend logs"})",
                           "application/json");
      return;
    }
    // Compare the actual ABR settings with the run's immutable snapshot.
    if (payload["parameters"] != activeExperimentConfig["abrParameters"]) {
      response.status = 409;
      response.set_content(R"({"error":"Player parameters differ from experiment snapshot"})",
                           "application/json");
      return;
    }
    const bool isBatch = payload.contains("batch") && !payload["batch"].is_null();
    if (isBatch != (activeExperimentConfig["runSettings"]["repeatRuns"].get<int>() > 1)) {
      response.status = 400;
      response.set_content(R"({"error":"Batch mode does not match configured repeatRuns"})",
                           "application/json");
      return;
    }
    std::string batchId;
    int runIndex = 0;
    if (isBatch) {
      const auto &batch = payload["batch"];
      if (!batch.is_object() || !batch.contains("label") || !batch["label"].is_string() ||
          !batch.contains("id") || !batch["id"].is_string() ||
          !batch.contains("index") || !batch["index"].is_number_integer() ||
          !batch.contains("total") || !batch["total"].is_number_integer()) {
        response.status = 400;
        response.set_content(R"({"error":"Invalid batch metadata"})", "application/json");
        return;
      }
      batchId = batch["id"].get<std::string>();
      runIndex = batch["index"].get<int>();
      const auto &settings = activeExperimentConfig["runSettings"];
      if (!safeIdentifier(batch["label"].get<std::string>()) || !safeIdentifier(batchId) ||
          batch["label"] != settings["resultLabel"] ||
          batch.value("seed", std::string()) != settings["trafficSeed"].get<std::string>() ||
          batch["total"].get<int>() != settings["repeatRuns"].get<int>() ||
          runIndex < 1 || runIndex > settings["repeatRuns"].get<int>()) {
        response.status = 400;
        response.set_content(R"({"error":"Invalid batch identifier or index"})", "application/json");
        return;
      }
    }

    if (!isExperimentRunning.load() || !startCommandSent || stopCommandSent ||
        activeRunId != requestedRunId || resultSavedForActiveRun) {
      response.status = 409;
      response.set_content(R"({"error":"No active unsaved matching experiment"})", "application/json");
      return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&nowTime, &localTime);
    std::ostringstream dateTime;
    dateTime << std::put_time(&localTime, "%H-%M-%S_%Y-%m-%d");
    const fs::path resultRoot = "tester/experiments";
    std::error_code ec;
    fs::create_directories(resultRoot, ec);
    fs::path directory;
    if (!ec && isBatch) {
      auto found = batchDirectories.find(batchId);
      if (found != batchDirectories.end()) {
        const BatchDirectoryState &previous = found->second;
        if (previous.configuration != activeExperimentConfig ||
            previous.label != payload["batch"]["label"].get<std::string>() ||
            previous.seed != payload["batch"].value("seed", std::string()) ||
            previous.total != payload["batch"]["total"].get<int>()) {
          response.status = 409;
          response.set_content(R"({"error":"Batch configuration changed"})",
                               "application/json");
          return;
        }
        directory = previous.directory;
        if (!fs::is_directory(directory, ec) || ec) {
          response.status = 409;
          response.set_content(R"({"error":"Batch result folder is missing"})",
                               "application/json");
          return;
        }
      } else if (runIndex != 1) {
        response.status = 409;
        response.set_content(R"({"error":"Batch cannot resume after backend restart without batch metadata; start a fresh batch"})",
                             "application/json");
        return;
      }
    }
    if (!ec && directory.empty()) {
      const std::string suffix = dateTime.str() + "_" +
          activeExperimentConfig["runSettings"]["resultLabel"].get<std::string>();
      directory = createNumberedDirectory(resultRoot, suffix, ec);
    }
    if (ec || directory.empty()) {
      response.status = 500;
      response.set_content(nlohmann::json{{"error", ec ? ec.message() : "Cannot allocate result directory"}}.dump(),
                           "application/json");
      return;
    }
    // One immutable configuration snapshot per result folder. For a repeated
    // batch, later runs must match the first run; never rewrite historical data
    // with values from a later edit of experiment-parameters.js.
    const fs::path configurationPath = directory / "configuration.json";
    const nlohmann::json snapshot =
        savedConfiguration(activeExperimentConfig, testerPrepared, startCommandSent);
    if (fs::exists(configurationPath)) {
      std::ifstream stream(configurationPath);
      const auto previous = nlohmann::json::parse(stream, nullptr, false);
      if (previous.is_discarded() || previous != snapshot) {
        response.status = 409;
        response.set_content(R"({"error":"Saved configuration is missing, invalid, or differs from this experiment; refusing to mix settings in one result folder"})",
                             "application/json");
        return;
      }
    } else {
      // A missing snapshot for an existing batch is not silently recreated.
      if (isBatch && runIndex > 1) {
        response.status = 409;
        response.set_content(R"({"error":"Configuration snapshot missing for an existing batch"})",
                             "application/json");
        return;
      }
      std::string snapshotError;
      if (!writeNewResult(configurationPath, snapshot.dump(2) + "\n", snapshotError)) {
        response.status = 500;
        response.set_content(nlohmann::json{{"error", "Could not save experiment configuration: " + snapshotError}}.dump(),
                             "application/json");
        return;
      }
    }

    // Every folder numbers its runs locally, starting with run-1.json.
    const std::string filename = "run-" + std::to_string(isBatch ? runIndex : 1) + ".json";
    const fs::path destination = directory / filename;
    std::string error;
    // The file is exactly the QoE object; no IDs, timing metadata, or segment data.
    if (!writeNewResult(destination, orderedQoe(payload["qoe"]).dump(2) + "\n", error)) {
      response.status = 500;
      response.set_content(nlohmann::json{{"error", "Could not save result: " + error}}.dump(),
                           "application/json");
      return;
    }
    if (isBatch && runIndex == 1) {
      batchDirectories[batchId] = BatchDirectoryState{
          directory, activeExperimentConfig,
          payload["batch"]["label"].get<std::string>(),
          payload["batch"].value("seed", std::string()),
          payload["batch"]["total"].get<int>()};
    }
    // Readable Markdown report is a companion: failure must never make a saved
    // machine-readable QoE run look unsaved or alter batch consistency.
    const fs::path reportPath = destination.parent_path() /
        ("run-" + std::to_string(isBatch ? runIndex : 1) + ".md");
    bool reportSaved = fs::exists(reportPath);
    if (!reportSaved) {
      std::string reportError;
      reportSaved = writeNewResult(reportPath,
          formatQoeReport(payload["qoe"],
              "Run " + std::to_string(isBatch ? runIndex : 1), false), reportError);
      if (!reportSaved)
        logMain("WARNING: cannot save readable QoE report: " + reportError);
    }
    // Telemetry filename is reserved with this QoE result, but finalized only
    // after tester exit so network samples and cleanup events are complete.
    if (activeTelemetry) {
      std::lock_guard<std::mutex> telemetryLock(activeTelemetry->mutex);
      activeTelemetry->outputPath = directory /
          ("run-" + std::to_string(isBatch ? runIndex : 1) + "-telemetry.json");
      activeTelemetry->player = payload.value("telemetry", nlohmann::json(nullptr));
    }
    resultSavedForActiveRun = true;
    std::cerr << "Saved QoE result: " << destination.string() << '\n';
    response.status = 201;
    response.set_content(nlohmann::json{{"saved", true}, {"runId", requestedRunId},
                                        {"file", destination.generic_string()},
                                        {"reportSaved", reportSaved},
                                        {"reportFile", reportSaved ? reportPath.generic_string() : ""}}.dump(),
                         "application/json");
  });

  // Configured successful runs produce one mean-QoE object. Histories are not averaged.
  server.Options("/experiment/average", [](const httplib::Request &, httplib::Response &response) {
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.status = 204;
  });
  server.Post("/experiment/average", [](const httplib::Request &request, httplib::Response &response) {
    const auto body = nlohmann::json::parse(request.body, nullptr, false);
    if (!body.is_object() || !body.contains("batch") || !body["batch"].is_object()) {
      response.status = 400;
      response.set_content(R"({"error":"Missing batch"})", "application/json");
      return;
    }
    const auto &batch = body["batch"];
    if (!batch.contains("label") || !batch["label"].is_string() ||
        !batch.contains("id") || !batch["id"].is_string() ||
        !batch.contains("total") || !batch["total"].is_number_integer() ||
        batch["total"].get<int>() < 2 || batch["total"].get<int>() > 20 ||
        !safeIdentifier(batch["label"].get<std::string>()) ||
        !safeIdentifier(batch["id"].get<std::string>())) {
      response.status = 400;
      response.set_content(R"({"error":"Invalid batch"})", "application/json");
      return;
    }
    std::lock_guard<std::mutex> lock(testerControlMutex);
    const std::string batchId = batch["id"].get<std::string>();
    const auto found = batchDirectories.find(batchId);
    if (found == batchDirectories.end()) {
      response.status = 409;
      response.set_content(R"({"error":"Batch not found in backend memory; restarting the backend prevents safely resuming this batch"})",
                           "application/json");
      return;
    }
    const BatchDirectoryState &info = found->second;
    if (info.total != batch["total"].get<int>() ||
        info.label != batch["label"].get<std::string>() ||
        info.seed != batch.value("seed", std::string())) {
      response.status = 409;
      response.set_content(R"({"error":"Batch settings mismatch"})", "application/json");
      return;
    }
    const fs::path directory = info.directory;
    std::error_code directoryError;
    if (!fs::is_directory(directory, directoryError) || directoryError) {
      response.status = 409;
      response.set_content(R"({"error":"Batch result directory not found"})", "application/json");
      return;
    }
    const fs::path destination = directory / "average.json";
    // The last finished run is deliberately cleared from activeRunId at exit.
    // Verify that no tester is active, and reject a known failed cleanup.
    // After a backend restart, the in-memory batch mapping is unavailable.
    if (isExperimentRunning.load() ||
        (lastFinishedRunId != 0 && !lastTesterExitSuccessful)) {
      response.status = 409;
      response.set_content(R"({"error":"Tester is still running or cleanup failed"})",
                           "application/json");
      return;
    }
    // Read each completed QoE object directly from disk (not browser memory).
    constexpr const char *fields[] = {
      "playbackDurationTotal480Sec", "playbackDurationTotal720Sec",
      "playbackDurationTotal1080Sec", "startupDelayMs", "stallCountTotal",
      "stallDurationTotalMs", "qualitySwitchCount", "abortedDownloadCount"};
    nlohmann::json average = nlohmann::json::object();
    for (const char *field : fields) average[field] = 0.0;
    for (int index = 1; index <= batch["total"].get<int>(); ++index) {
      std::ifstream stream(directory / ("run-" + std::to_string(index) + ".json"));
      if (!stream) {
        response.status = 409;
        response.set_content(R"({"error":"Missing QoE run; cannot calculate average"})",
                             "application/json");
        return;
      }
      const auto qoe = nlohmann::json::parse(stream, nullptr, false);
      if (!validQoe(qoe)) {
        response.status = 409;
        response.set_content(R"({"error":"Invalid QoE run; cannot calculate average"})",
                             "application/json");
        return;
      }
      for (const char *field : fields) {
        average[field] = average[field].get<double>() + qoe[field].get<double>() / batch["total"].get<double>();
      }
    }
    if (fs::exists(destination)) {
      std::ifstream stream(destination);
      const auto existing = nlohmann::json::parse(stream, nullptr, false);
      if (existing != average) {
        response.status = 409;
        response.set_content(R"({"error":"Conflicting existing average"})", "application/json");
        return;
      }
      response.status = 200;
    } else {
      std::string error;
      if (!writeNewResult(destination, orderedQoe(average).dump(2) + "\n", error)) {
        response.status = 500;
        response.set_content(nlohmann::json{{"error", error}}.dump(), "application/json");
        return;
      }
      response.status = 201;
    }
    // Save a readable batch summary alongside the numeric average.
    const fs::path reportPath = directory / "average.md";
    if (!fs::exists(reportPath)) {
      std::string reportError;
      if (!writeNewResult(reportPath,
              formatQoeReport(average, "Batch average", true), reportError))
        logMain("WARNING: cannot save readable average report: " + reportError);
    }
    nlohmann::ordered_json responseBody = nlohmann::ordered_json::object();
    responseBody["saved"] = true;
    responseBody["average"] = orderedQoe(average);
    responseBody["reportSaved"] = fs::exists(reportPath);
    responseBody["reportFile"] = fs::exists(reportPath) ? reportPath.generic_string() : "";
    response.set_content(responseBody.dump(), "application/json");
  });

  // ==================== 6. STOP EXPERIMENT ====================

  server.Options("/experiment/stop",
                 [](const httplib::Request &request, httplib::Response &httpResponse) {
                   httpResponse.set_header("Access-Control-Allow-Headers", "Content-Type");
                   httpResponse.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                   httpResponse.status = 204;
                 });

  server.Post(
      "/experiment/stop", [](const httplib::Request &request, httplib::Response &httpResponse) {
        std::lock_guard<std::mutex> lock(testerControlMutex);
        logMain(runLabel(activeRunId) + "POST /experiment/stop received; running=" +
                std::to_string(isExperimentRunning.load()) + " stopRequested=" +
                std::to_string(stopCommandSent));

        if (!isExperimentRunning.load()) {
          httpResponse.status = 409;
          httpResponse.set_content(R"({"error":"No experiment is running"})", "application/json");
          return;
        }

        if (testerStopWriteFd == -1) {
          httpResponse.status = 409;
          httpResponse.set_content(R"({"error":"Experiment is not ready to receive commands"})",
                                   "application/json");
          return;
        }

        if (stopCommandSent) {
          httpResponse.status = 202;
          httpResponse.set_content(R"({"status":"stop_already_requested"})", "application/json");
          return;
        }

        // Send cancellation to tester. Tester stops browsing containers and removes its network setup.
        // main.cpp does not remove tc rules itself.
        const char stopCommand[] = "c\n";

        ssize_t bytesWritten = write(testerStopWriteFd, stopCommand, sizeof(stopCommand) - 1);

        if (bytesWritten == -1) {
          int operationError = errno;
          logMain(runLabel(activeRunId) + "ERROR: STOP pipe write: " + std::strerror(operationError));

          httpResponse.status = 500;
          httpResponse.set_content(nlohmann::json{{"error", std::strerror(operationError)}}.dump(),
                                   "application/json");
          return;
        }

        if (bytesWritten != 2) {
          logMain(runLabel(activeRunId) + "ERROR: STOP pipe short write: " + std::to_string(bytesWritten));
          httpResponse.status = 500;
          httpResponse.set_content(R"({"error":"Incomplete stop command write"})",
                                   "application/json");
          return;
        }

        stopCommandSent = true;
        testerPrepared = false;
        logMain(runLabel(activeRunId) + "STOP sent to tester; awaiting process exit");

        httpResponse.status = 202;
        httpResponse.set_content(R"({"status":"stop_requested"})", "application/json");
      });

  // ==================== 7. LISTEN FOR HTTP REQUESTS ====================

  logMain("HTTP server starting on 0.0.0.0:8080");
  if (!server.listen("0.0.0.0", 8080)) {
    logMain("ERROR: HTTP server listen returned false (bind/listen failure or server stopped)");
    return 1;
  }
  logMain("HTTP server listen returned (server stopped)");
}
