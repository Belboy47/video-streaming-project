// Tester for a controlled shared-link ABR + DigiKala browsing experiment.
// Source baseline: user's saved tester(8).cpp (Nov 12, 2026).
// Protocol with main.cpp: stdout sends 'R' or 'E'; stdin receives 's' to START,
// 'c' to CANCEL. All diagnostic logs go to stderr.
// Browser protocol over 127.0.0.1:19091: READY <slot>\n -> START\n.
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ==================== CONFIGURATION ====================
// Workload values supplied by main.cpp from its validated experiment-parameters.js snapshot.
// Browser count 0 means no Docker browser or WebPageReplay processes.
std::size_t gBrowserCount = 0;
bool gCustomAbrEnabled = false;  // For diagnostics; React owns these players.
bool gDashJsEnabled = false;
// Delay between launching Docker containers. Separates CPU-heavy initialization.
int gBrowserLaunchIntervalMs = 2000;
// Delay between *shopping journey starts* after GO: 0, 5, 10, ... seconds.
// Independent of Docker launch timing; change this to vary household activity.
int gBrowserStartIntervalMs = 5000;
int gBrowserStartDelayMs = 0;
int gBrowserStartJitterMs = 0;
std::string gTrafficSeed = "baseline";

// Stable per-slot FNV-1a hash. The same seed/config produces the same offsets.
std::uint64_t slotHash(std::size_t slot) {
  std::uint64_t h = 14695981039346656037ull;
  const std::string key = gTrafficSeed + ":" + std::to_string(slot);
  for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
  return h;
}

long long browserOffsetMs(std::size_t slot) {
  const long long base = static_cast<long long>(gBrowserStartDelayMs) +
      static_cast<long long>(slot - 1) * gBrowserStartIntervalMs;
  if (gBrowserStartJitterMs == 0) return base;
  const unsigned long long span = static_cast<unsigned long long>(gBrowserStartJitterMs) * 2ull + 1ull;
  const long long jitter = static_cast<long long>(slotHash(slot) % span) - gBrowserStartJitterMs;
  return std::max(0LL, base + jitter);
}
constexpr int kControlPort = 19091;
// After the final container is launched, allow this long for every READY handshake.
constexpr int kStartupTimeoutSec = 120;
constexpr int kStartTimeoutSec = 120; // Time main has to send START after READY.
constexpr const char *kDockerImage = "sitespeedio/browsertime:28.0.0";
constexpr const char *kDockerExecutable = "/usr/bin/docker";
constexpr const char *kJourneyFile = "digikala-journey.mjs";
constexpr const char *kArchiveFile = "WPR-archives/digikala.wprgo";
constexpr double kBandwidthMbps = 16.0;
constexpr int kDelayMs = 100;
constexpr int kJitterMs = 0;
constexpr double kLossPercent = 0.0;

volatile sig_atomic_t gStop = 0;
std::atomic_bool gTelemetryStop{false};
std::mutex gTraceMutex;
Clock::time_point gGoTime{};

long long epochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
long long sinceGoMs() {
  if (gGoTime == Clock::time_point{}) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - gGoTime).count();
}
// These messages are parsed into run-N-telemetry.json by main.cpp.
void telemetryEvent(const std::string &event, const std::string &detail = "") {
  std::ostringstream msg;
  msg << "TELEMETRY_EVENT {\"event\":\"" << event
      << "\",\"epochMs\":" << epochMs()
      << ",\"tMs\":" << sinceGoMs();
  if (!detail.empty()) msg << ",\"detail\":\"" << detail << "\"";
  msg << "}";
  std::lock_guard<std::mutex> lock(gTraceMutex);
  std::cerr << msg.str() << '\n' << std::flush;
}

bool tcOutput(const std::string &commandLine, std::string &output) {
  FILE *pipe = popen(commandLine.c_str(), "r");
  if (!pipe) return false;
  char buf[2048];
  while (fgets(buf, sizeof(buf), pipe)) output.append(buf);
  const bool errored = ferror(pipe) != 0;
  return pclose(pipe) == 0 && !errored;
}

long long portBytes(const std::string &output, int port) {
  const std::string marker = "src_port " + std::to_string(port);
  auto from = output.find(marker);
  // Match an entire decimal port, not the 80 prefix of 8080.
  while (from != std::string::npos &&
         from + marker.size() < output.size() &&
         output[from + marker.size()] >= '0' && output[from + marker.size()] <= '9')
    from = output.find(marker, from + 1);
  if (from == std::string::npos) return -1;
  const auto sent = output.find("Sent ", from);
  const auto nextFilter = output.find("src_port ", from + marker.size());
  if (sent == std::string::npos || (nextFilter != std::string::npos && sent > nextFilter)) return -1;
  long long bytes = -1;
  std::istringstream parser(output.substr(sent + 5));
  parser >> bytes;
  return bytes;
}

long long extractNumber(const std::string &line, const std::string &marker) {
  const auto pos = line.find(marker);
  if (pos == std::string::npos) return -1;
  long long n = -1;
  std::istringstream parser(line.substr(pos + marker.size()));
  parser >> n;
  return n;
}

void sampleNetwork(const std::string &ethDevice) {
  long long previousAtMs = -1, previousVideo = -1, previousBrowser = -1, previousShaped = -1;
  while (!gTelemetryStop.load()) {
    std::string lo, eth, qdisc;
    const bool validLo = tcOutput("tc -s filter show dev lo egress 2>/dev/null", lo);
    const bool validEth = tcOutput("tc -s filter show dev " + ethDevice + " egress 2>/dev/null", eth);
    const bool validQdisc = tcOutput("tc -s qdisc show dev ifb-abr 2>/dev/null", qdisc);
    if (validLo && validEth && validQdisc) {
      const long long loVideo = portBytes(lo, 8080);
      const long long ethVideo = portBytes(eth, 8080);
      const long long browser80 = portBytes(lo, 80);
      const long long browser443 = portBytes(lo, 443);
      const long long video = std::max(0LL,loVideo) + std::max(0LL,ethVideo);
      const long long browser = std::max(0LL,browser80) + std::max(0LL,browser443);
      const auto sent = qdisc.find("Sent ");
      const long long shaped = sent == std::string::npos ? -1 : extractNumber(qdisc, "Sent ");
      const long long backlog = extractNumber(qdisc, "backlog ");
      const long long dropped = extractNumber(qdisc, "dropped ");
      const long long overlimits = extractNumber(qdisc, "overlimits ");
      const long long now = epochMs();
      const double interval = previousAtMs >= 0 ? (now - previousAtMs) / 1000.0 : 0;
      auto mbps = [interval](long long current, long long previous) {
        return interval > 0 && previous >= 0 && current >= previous
            ? (current - previous) * 8.0 / (interval * 1000000.0) : -1.0;
      };
      std::ostringstream record;
      record << std::fixed << std::setprecision(3)
             << "TELEMETRY_NETWORK {\"epochMs\":" << now
             << ",\"tMs\":" << sinceGoMs()
             << ",\"video8080Bytes\":" << video
             << ",\"browserHttpHttpsBytes\":" << browser
             << ",\"ifbSentBytes\":" << shaped
             << ",\"ifbBacklogBytes\":" << backlog
             << ",\"ifbDroppedPackets\":" << dropped
             << ",\"ifbOverlimits\":" << overlimits
             << ",\"videoMbps\":" << mbps(video, previousVideo)
             << ",\"browserMbps\":" << mbps(browser, previousBrowser)
             << ",\"ifbMbps\":" << mbps(shaped, previousShaped)
             << ",\"sampleIntervalSec\":" << interval << "}";
      {
        std::lock_guard<std::mutex> lock(gTraceMutex);
        std::cerr << record.str() << '\n' << std::flush;
      }
      previousAtMs = now;
      previousVideo = video;
      previousBrowser = browser;
      previousShaped = shaped;
    } else {
      telemetryEvent("network_sample_failed");
    }
    for (int i=0; i<10 && !gTelemetryStop.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void onSignal(int) { gStop = 1; }

// Diagnostic logs MUST stay on stderr. Stdout carries the single-byte
// P/R/E protocol to main.cpp; writing logs there would break startup.
void trace(const std::string &message) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t raw = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&raw, &local);
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count() % 1000;
  std::ostringstream line;
  line << '[' << std::put_time(&local, "%H:%M:%S") << '.'
       << std::setw(3) << std::setfill('0') << milliseconds
       << "] [tester pid=" << getpid() << "] " << message << '\n';
  std::lock_guard<std::mutex> lock(gTraceMutex);
  std::cerr << line.str() << std::flush;
}

bool command(const std::string &cmd) {
  trace("COMMAND BEGIN: " + cmd);
  const auto started = Clock::now();
  int status = std::system(cmd.c_str());
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - started).count();
  trace("COMMAND END: status=" + std::to_string(status) +
        ", duration=" + std::to_string(elapsed) + "ms: " + cmd);
  if (status == 0)
    return true;
  std::cerr << "Command failed: " << cmd << " (status " << status << ")\n";
  return false;
}

bool qdiscListing(const char *device, std::string &out) {
  std::string cmd = std::string("tc qdisc show dev ") + device;
  trace("QDISC BEGIN: " + cmd);
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    trace("QDISC popen failed on " + std::string(device) +
          ": " + std::strerror(errno));
    return false;
  }
  trace("QDISC popen returned for " + std::string(device) + "; reading output");
  char buf[512];
  while (fgets(buf, sizeof(buf), pipe))
    out += buf;
  const bool readError = ferror(pipe);
  trace("QDISC finished reading " + std::string(device) + "; waiting for child (pclose)");
  const int exitStatus = pclose(pipe);
  trace("QDISC END: " + std::string(device) + ", exitStatus=" +
        std::to_string(exitStatus) + ", readError=" +
        (readError ? "true" : "false") + ", output=" + out);
  return !readError && exitStatus == 0;
}

std::string detectEthernetInterface() {
  trace("INTERFACE begin detecting default-route eth0/eth1");
  FILE *pipe = popen("ip -4 route get 1.1.1.1 2>/dev/null", "r");
  if (!pipe)
    return "";

  std::string output;
  char buf[256];
  while (fgets(buf, sizeof(buf), pipe))
    output += buf;
  if (pclose(pipe) != 0) {
    trace("INTERFACE route lookup failed");
    return "";
  }
  trace("INTERFACE route lookup: " + output);

  std::istringstream stream(output);
  std::string token;
  while (stream >> token) {
    if (token == "dev" && stream >> token) {
      if (token == "eth0" || token == "eth1")
        return token;
      break;
    }
  }
  return "";
}

struct NetworkState {
  std::string ethDevice;
  bool ifb = false;
  bool loClsact = false;
  bool ethClsact = false;
  bool videoLo = false;
  bool videoEth = false;
  bool httpLo = false;
  bool httpsLo = false;
};

bool installFilter(
    const char *dev, int priority, int sourcePort, bool wprLoopback, bool &installed) {
  std::ostringstream cmd;
  cmd << "tc filter add dev " << dev << " egress protocol ip pref " << priority
      << " handle 1 flower ip_proto tcp ";
  if (wprLoopback)
    cmd << "src_ip 127.0.0.0/8 dst_ip 127.0.0.0/8 ";
  cmd << "src_port " << sourcePort << " action mirred egress redirect dev ifb-abr";
  if (!command(cmd.str()))
    return false;
  installed = true;
  return true;
}

bool setupNetwork(NetworkState &n) {
  trace("NETWORK SETUP BEGIN");
  n.ethDevice = detectEthernetInterface();
  if (n.ethDevice.empty()) {
    std::cerr << "No suitable eth0/eth1 default-route interface found.\n";
    return false;
  }
  std::cerr << "Selected network interface: " << n.ethDevice << "\n";
  std::string lo, eth;
  if (!qdiscListing("lo", lo) || !qdiscListing(n.ethDevice.c_str(), eth))
    return false;
  if (eth.find("qdisc netem ") != std::string::npos) {
    std::cerr << "Old netem root still active on " << n.ethDevice << "; refusing a second limit.\n";
    return false;
  }
  if (!command("ip link add ifb-abr type ifb"))
    return false;
  n.ifb = true;
  if (!command("ip link set ifb-abr up"))
    return false;
  if (lo.find("qdisc clsact ") == std::string::npos) {
    if (!command("tc qdisc add dev lo clsact"))
      return false;
    n.loClsact = true;
  }
  if (eth.find("qdisc clsact ") == std::string::npos) {
    if (!command("tc qdisc add dev " + n.ethDevice + " clsact"))
      return false;
    n.ethClsact = true;
  }

  // Both video (8080) and WPR (80/443) feed one netem queue.
  if (!installFilter("lo", 8080, 8080, false, n.videoLo))
    return false;
  if (!installFilter(n.ethDevice.c_str(), 8080, 8080, false, n.videoEth))
    return false;
  if (!installFilter("lo", 1800, 80, true, n.httpLo))
    return false;
  if (!installFilter("lo", 1443, 443, true, n.httpsLo))
    return false;

  std::ostringstream cmd;
  cmd << "tc qdisc replace dev ifb-abr root netem rate " << kBandwidthMbps << "mbit delay "
      << kDelayMs << "ms " << kJitterMs << "ms loss " << kLossPercent << "%";
  const bool ok = command(cmd.str());
  trace(std::string("NETWORK SETUP END: ") + (ok ? "success" : "failure"));
  return ok;
}

bool deleteFilter(const char *dev, int pref, bool &flag) {
  if (!flag)
    return true;
  const std::string cmd = std::string("tc filter del dev ") + dev + " egress protocol ip pref " +
                          std::to_string(pref) + " handle 1 flower";
  if (!command(cmd))
    return false;
  flag = false;
  return true;
}

bool cleanupNetwork(NetworkState &n) {
  trace("NETWORK CLEANUP BEGIN: interface=" + n.ethDevice);
  bool ok = true;
  if (!deleteFilter("lo", 1800, n.httpLo))
    ok = false;
  if (!deleteFilter("lo", 1443, n.httpsLo))
    ok = false;
  if (!deleteFilter("lo", 8080, n.videoLo))
    ok = false;
  if (!deleteFilter(n.ethDevice.c_str(), 8080, n.videoEth))
    ok = false;
  if (n.loClsact && command("tc qdisc del dev lo clsact"))
    n.loClsact = false;
  else if (n.loClsact)
    ok = false;
  if (n.ethClsact && command("tc qdisc del dev " + n.ethDevice + " clsact"))
    n.ethClsact = false;
  else if (n.ethClsact)
    ok = false;
  // Never delete IFB while any of our redirect filters remain installed.
  if (n.videoLo || n.videoEth || n.httpLo || n.httpsLo)
    return false;
  if (n.ifb && command("ip link delete ifb-abr"))
    n.ifb = false;
  else if (n.ifb)
    ok = false;
  trace(std::string("NETWORK CLEANUP END: ") + (ok ? "success" : "failure"));
  return ok;
}

// ==================== DOCKER CHILD PROCESS MANAGEMENT ====================
struct Child {
  pid_t pid = -1;            // PID of 'docker run' CLI, not container init.
  std::string containerName; // Used for 'docker stop', which stops the container.
};

bool spawnProcess(Child &child, const std::vector<std::string> &args) {
  trace("SPAWN BEGIN: child=" + child.containerName +
        ", executable=" + (args.empty() ? "(none)" : args.front()));
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const std::string &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t io;
  int err = posix_spawn_file_actions_init(&io);
  if (err != 0)
    return false;
  // Tester stdout is reserved for the one-byte R/E protocol to main.cpp.
  err = posix_spawn_file_actions_addopen(&io, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (err == 0)
    err = posix_spawn_file_actions_adddup2(&io, STDERR_FILENO, STDOUT_FILENO);
  if (err == 0)
    err = posix_spawn(&child.pid, args.front().c_str(), &io, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&io);
  if (err != 0) {
    trace("SPAWN FAILED: " + child.containerName + ": " + std::strerror(err));
    std::cerr << "posix_spawn failed: " << std::strerror(err) << '\n';
    return false;
  }
  trace("SPAWN END: " + child.containerName + ", pid=" +
        std::to_string(child.pid));
  return true;
}

bool hasExited(Child &child) {
  if (child.pid <= 0)
    return false;
  int status = 0;
  const pid_t result = waitpid(child.pid, &status, WNOHANG);
  if (result == 0 || (result == -1 && errno == EINTR))
    return false;
  if (result == child.pid) {
    if (WIFEXITED(status))
      std::cerr << child.containerName << " exited with code " << WEXITSTATUS(status) << '\n';
    else if (WIFSIGNALED(status))
      std::cerr << child.containerName << " killed by signal " << WTERMSIG(status) << '\n';
  } else {
    std::cerr << "waitpid error for " << child.containerName << ": " << std::strerror(errno)
              << '\n';
  }
  child.pid = -1;
  return true;
}

void reapChild(Child &child) {
  if (child.pid <= 0)
    return;
  // docker stop is supposed to end its docker-run CLI; wait with a deadline.
  for (int i = 0; i < 40; ++i) {
    if (hasExited(child))
      return;
    poll(nullptr, 0, 100);
  }
  kill(child.pid, SIGTERM);
  for (int i = 0; i < 20; ++i) {
    if (hasExited(child))
      return;
    poll(nullptr, 0, 100);
  }
  kill(child.pid, SIGKILL);
  int status = 0;
  while (waitpid(child.pid, &status, 0) == -1 && errno == EINTR) {
  }
  child.pid = -1;
}

void stopContainer(Child &child) {
  trace("STOP CONTAINER begin: " + child.containerName);
  if (!child.containerName.empty()) {
    // Unique generated name; never constructed from untrusted input.
    command(std::string(kDockerExecutable) + " stop -t 2 " + child.containerName +
            " >/dev/null 2>&1");
  }
  reapChild(child);
  trace("STOP CONTAINER end: " + child.containerName);
}

std::vector<std::string> dockerBase(const std::string &name, const fs::path &root) {
  return {kDockerExecutable,
          "run",
          "--rm",
          "--name",
          name,
          "--network",
          "host",
          "-v",
          root.string() + ":/sitespeed.io:ro"};
}

bool startWpr(Child &wpr, const fs::path &root, const std::string &runPrefix) {
  wpr.containerName = runPrefix + "-wpr";
  auto args = dockerBase(wpr.containerName, root);
  args.insert(args.end(),
              {"--entrypoint",
               "/usr/local/bin/wpr",
               kDockerImage,
               "replay",
               "--http_port",
               "80",
               "--https_port",
               "443",
               "--https_cert_file",
               "/webpagereplay/certs/wpr_cert.pem",
               "--https_key_file",
               "/webpagereplay/certs/wpr_key.pem",
               "--inject_scripts",
               "/webpagereplay/scripts/deterministic.js",
               std::string("/sitespeed.io/") + kArchiveFile});
  return spawnProcess(wpr, args);
}

bool startBrowser(Child &browser,
                  const fs::path &root,
                  const std::string &runPrefix,
                  std::size_t slot) {
  browser.containerName = runPrefix + "-b" + std::to_string(slot);
  auto args = dockerBase(browser.containerName, root);
  args.insert(args.end(),
              {"--shm-size",
               "1g",
               "-v",
               (root / "tester/experiments/browser-logs").string() + ":/browsertime",
               "-e",
               "BROWSER_SLOT=" + std::to_string(slot),
               // All containers receive START together; their journeys apply this offset.
               "-e",
               "BROWSER_START_OFFSET_MS=" + std::to_string(browserOffsetMs(slot)),
               "-e",
               "BROWSER_RANDOM_SEED=" + gTrafficSeed + "-" + std::to_string(slot),
               "-e",
               "BROWSER_CONTROL_PORT=" + std::to_string(kControlPort),
               kDockerImage,
               std::string("/sitespeed.io/") + kJourneyFile,
               "--chrome.webPageReplayHostResolver",
               "--chrome.webPageReplayHTTPPort",
               "80",
               "--chrome.webPageReplayHTTPSPort",
               "443",
               "--resultDir",
               "/browsertime/" + runPrefix + "/browser-" + std::to_string(slot),
               "-n",
               "1"});
  return spawnProcess(browser, args);
}

// ==================== CONTROL SOCKETS AND READINESS ====================
struct Connection {
  int fd = -1;
  std::size_t slot = 0; // Set once READY <slot> was received.
  std::string buffer;
};

int listenerSocket() {
  trace("LISTENER BEGIN: binding 127.0.0.1:" + std::to_string(kControlPort));
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1)
    return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kControlPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1 ||
      listen(fd, SOMAXCONN) == -1) {
    close(fd);
    return -1;
  }
  trace("LISTENER READY: fd=" + std::to_string(fd));
  return fd;
}

// Avoid a blocking connect() during WPR preflight or readiness checks.
// With WSL mirrored networking, an unresponsive localhost port can stall connect().
// The socket is nonblocking and each probe has a strict one-second deadline.
bool portOpen(int port) {
  constexpr auto kProbeTimeout = std::chrono::milliseconds(1000);
  const auto started = Clock::now();
  trace("PORT PROBE begin: 127.0.0.1:" + std::to_string(port) + " (nonblocking, 1000ms timeout)");

  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd == -1) {
    trace("PORT PROBE error: socket: " + std::string(std::strerror(errno)));
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  bool connected = false;
  int connectError = 0;
  const int result = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  if (result == 0) {
    connected = true;
  } else if (errno != EINPROGRESS) {
    connectError = errno;
  } else {
    pollfd event{fd, POLLOUT, 0};
    int polled = -1;
    const auto deadline = started + kProbeTimeout;
    while (!gStop) {
      const auto now = Clock::now();
      if (now >= deadline) {
        connectError = ETIMEDOUT;
        break;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int timeoutMs = std::max(1, static_cast<int>(remaining.count()));
      polled = poll(&event, 1, timeoutMs);
      if (polled < 0 && errno == EINTR)
        continue;
      if (polled == 0) {
        connectError = ETIMEDOUT;
      } else if (polled < 0) {
        connectError = errno;
      } else {
        socklen_t errorLength = sizeof(connectError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &connectError, &errorLength) != 0)
          connectError = errno;
        connected = connectError == 0;
      }
      break;
    }
    if (gStop && !connected && connectError == 0)
      connectError = EINTR;
  }

  close(fd);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - started).count();
  trace("PORT PROBE end: " + std::to_string(port) +
        (connected ? " OPEN" : (" UNREACHABLE: " + std::string(std::strerror(connectError)))) +
        ", duration=" + std::to_string(elapsed) + "ms");
  return connected;
}

bool processReadinessLine(Connection &c, const std::string &line, std::vector<bool> &ready) {
  std::istringstream parser(line);
  std::string tag;
  std::size_t slot = 0;
  std::string extra;
  if (!(parser >> tag >> slot) || tag != "READY" || (parser >> extra) || slot < 1 ||
      slot > gBrowserCount || ready[slot - 1]) {
    std::cerr << "Invalid or duplicate READY: " << line << '\n';
    return false;
  }
  c.slot = slot;
  ready[slot - 1] = true;
  std::cerr << "Browsertime browser " << slot << " READY\n";
  telemetryEvent("browser_ready", std::to_string(slot));
  return true;
}

bool readyAll(const std::vector<bool> &ready) {
  for (bool value : ready)
    if (!value)
      return false;
  return true;
}

bool anyChildExited(Child &wpr, std::vector<Child> &browsers) {
  if (hasExited(wpr))
    return true;
  for (Child &browser : browsers)
    if (hasExited(browser))
      return true;
  return false;
}

bool readControlByte(char &value) {
  char buf[32];
  const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0)
    return false; // EOF or error cancels the experiment.
  for (ssize_t i = 0; i < n; ++i) {
    if (buf[i] == 'c' || buf[i] == 'C') {
      value = 'c';
      return true;
    }
    if (buf[i] == 's' || buf[i] == 'S') {
      value = 's';
      return true;
    }
  }
  value = '?';
  return true;
}

// One nonblocking/short-bounded iteration of the READY/STOP socket protocol.
// Used while spacing out launches AND while waiting for the remaining browsers.
bool serviceReadiness(int listener,
                      std::vector<Connection> &clients,
                      std::vector<bool> &ready,
                      Child &wpr,
                      std::vector<Child> &browsers,
                      int timeoutMs) {
  if (gStop || anyChildExited(wpr, browsers)) {
    trace("READINESS aborted: signal or child exit");
    return false;
  }
  std::vector<pollfd> fds{{listener, POLLIN, 0}, {STDIN_FILENO, POLLIN | POLLHUP, 0}};
  for (const auto &client : clients)
    fds.push_back({client.fd, POLLIN | POLLHUP, 0});
  const int result = poll(fds.data(), fds.size(), timeoutMs);
  if (result < 0)
    return errno == EINTR && !gStop;
  if (gStop)
    return false;
  if (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
    char cmd = '?';
    const bool readable = readControlByte(cmd);
    trace("READINESS control pipe event: " +
          std::string(!readable ? "EOF/error" : std::string(1, cmd)));
    if (!readable || cmd == 'c')
      return false;
    if (cmd == 's') {
      std::cerr << "START received before READY\n";
      return false;
    }
  }
  if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
    return false;
  // Handle existing clients before accepting new ones (indices refer to fds).
  for (std::size_t i = 0; i < clients.size(); ++i) {
    const short events = fds[i + 2].revents;
    if (!events)
      continue;
    if (events & (POLLERR | POLLNVAL))
      return false;
    if (events & POLLIN) {
      char buf[256];
      const ssize_t n = read(clients[i].fd, buf, sizeof(buf));
      if (n <= 0)
        return false;
      clients[i].buffer.append(buf, static_cast<std::size_t>(n));
      if (clients[i].buffer.size() > 1024)
        return false;
      const std::size_t end = clients[i].buffer.find('\n');
      if (end != std::string::npos) {
        const std::string line = clients[i].buffer.substr(0, end);
        if (clients[i].slot != 0 || !processReadinessLine(clients[i], line, ready))
          return false;
        clients[i].buffer.erase(0, end + 1);
        if (!clients[i].buffer.empty())
          return false;
      }
    }
    if (events & POLLHUP)
      return false;
  }
  if (fds[0].revents & POLLIN) {
    const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd == -1)
      return false;
    trace("READINESS accepted browser control connection, fd=" + std::to_string(fd));
    clients.push_back({fd, 0, ""});
  }
  return !anyChildExited(wpr, browsers);
}

bool notifyMain(char status); // Progress bytes keep Main's inactivity timer alive.

// Only the interval between *launches* is staggered. READY sockets are served
// during the gap so an early browser never fills a listening backlog while
// later containers are being started. STOP and child failures remain responsive.
bool waitForNextLaunch(Clock::time_point nextLaunch,
                       int listener,
                       std::vector<Connection> &clients,
                       std::vector<bool> &ready,
                       Child &wpr,
                       std::vector<Child> &browsers) {
  trace("LAUNCH SPACING begin: waiting to launch next browser");
  auto nextHeartbeat = Clock::now() + std::chrono::seconds(5);
  while (!gStop && Clock::now() < nextLaunch) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(nextLaunch - Clock::now()).count();
    const int timeout =
        static_cast<int>(std::min<long long>(250, std::max<long long>(1, remaining)));
    if (!serviceReadiness(listener, clients, ready, wpr, browsers, timeout)) {
      trace("LAUNCH SPACING abort: readiness/control/child failure");
      return false;
    }
    if (Clock::now() >= nextHeartbeat) {
      if (!notifyMain('P'))
        return false;
      nextHeartbeat = Clock::now() + std::chrono::seconds(5);
    }
  }
  const bool ok = !gStop && !anyChildExited(wpr, browsers);
  trace(std::string("LAUNCH SPACING end: ") + (ok ? "success" : "failure"));
  return ok;
}

bool waitForBrowsers(int listener,
                     std::vector<Connection> &clients,
                     std::vector<bool> &ready,
                     Child &wpr,
                     std::vector<Child> &browsers) {
  trace("READY WAIT begin: waiting for " + std::to_string(gBrowserCount) +
        " browser(s), timeout=" + std::to_string(kStartupTimeoutSec) + "s");
  const auto deadline = Clock::now() + std::chrono::seconds(kStartupTimeoutSec);
  while (!gStop && Clock::now() < deadline) {
    if (readyAll(ready)) {
      trace("READY WAIT complete: all browser(s) ready");
      return true;
    }
    if (!serviceReadiness(listener, clients, ready, wpr, browsers, 250)) {
      trace("READY WAIT aborted: readiness/control/child failure");
      return false;
    }
  }
  std::cerr << "Browsertime readiness timeout\n";
  return false;
}

bool notifyMain(char status) {
  trace(std::string("STATUS to main: '") + status + "'");
  while (true) {
    const ssize_t written = write(STDOUT_FILENO, &status, 1);
    if (written == 1)
      return true;
    if (written < 0 && errno == EINTR)
      continue;
    return false;
  }
}

bool sendToBrowsers(const std::vector<Connection> &clients, const char *message) {
  for (const Connection &client : clients) {
    if (client.slot < 1)
      return false;
    const ssize_t sent = send(client.fd, message, std::strlen(message), MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(std::strlen(message)))
      return false;
  }
  return true;
}

bool waitForStart(Child &wpr,
                  std::vector<Child> &browsers,
                  const std::vector<Connection> &clients) {
  trace("GO WAIT begin: awaiting START command from main");
  const auto deadline = Clock::now() + std::chrono::seconds(kStartTimeoutSec);
  while (!gStop && Clock::now() < deadline) {
    if (anyChildExited(wpr, browsers))
      return false;
    std::vector<pollfd> fds{{STDIN_FILENO, POLLIN | POLLHUP, 0}};
    for (const auto &client : clients)
      fds.push_back({client.fd, POLLIN | POLLHUP, 0});
    const int r = poll(fds.data(), fds.size(), 250);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    for (size_t i = 1; i < fds.size(); ++i) {
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
        std::cerr << "Browser control socket closed before START\n";
        return false;
      }
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      char cmd = '?';
      if (!readControlByte(cmd) || cmd == 'c')
        return false;
      if (cmd == 's') {
        std::cerr << "Tester releasing " << gBrowserCount << " browser(s): journey starts every "
                  << gBrowserStartIntervalMs << " ms after GO\n";
        return sendToBrowsers(clients, "START\n");
      }
    }
  }
  std::cerr << "START command timeout\n";
  return false;
}

// A Browsertime container completing its journey is not an experiment failure.
// The ABR player can still be collecting its final QoE sample. Before GO, a
// browser exit remains a startup failure (handled by anyChildExited()).
bool observeRunningChild(Child &child, bool successfulCompletionAllowed) {
  if (child.pid <= 0)
    return true;  // No child / already successfully completed.

  int status = 0;
  pid_t result;
  do {
    result = waitpid(child.pid, &status, WNOHANG);
  } while (result == -1 && errno == EINTR);

  if (result == 0)
    return true;  // Still running.
  if (result == -1) {
    trace("RUNNING: waitpid failed for " + child.containerName +
          ": " + std::strerror(errno));
    return false;
  }

  // The child has been reaped; do not attempt to waitpid() it again.
  child.pid = -1;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      successfulCompletionAllowed) {
    trace("RUNNING: " + child.containerName +
          " finished successfully; awaiting ABR/player STOP");
    telemetryEvent("browser_completed", child.containerName);
    return true;
  }

  if (WIFEXITED(status)) {
    trace("RUNNING: unexpected exit of " + child.containerName +
          " (code " + std::to_string(WEXITSTATUS(status)) + ")");
  } else if (WIFSIGNALED(status)) {
    trace("RUNNING: " + child.containerName + " terminated by signal " +
          std::to_string(WTERMSIG(status)));
  } else {
    trace("RUNNING: " + child.containerName + " stopped unexpectedly");
  }
  telemetryEvent("workload_exited_unexpectedly", child.containerName);
  return false;
}

bool waitForStop(Child &wpr, std::vector<Child> &browsers) {
  trace("RUNNING: waiting for STOP/EOF; allowing successful browser completion");
  while (!gStop) {
    // WPR is a replay service: even a clean exit before STOP is unexpected.
    if (!observeRunningChild(wpr, false))
      return false;
    // Individual browser jobs can complete while the shared-link experiment
    // and its remaining workloads continue running.
    for (Child &browser : browsers)
      if (!observeRunningChild(browser, true))
        return false;

    pollfd fd{STDIN_FILENO, POLLIN | POLLHUP | POLLERR | POLLNVAL, 0};
    const int r = poll(&fd, 1, 250);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (fd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
      char cmd = '?';
      if (!readControlByte(cmd))
        return true; // Main closed the pipe -> stop.
      if (cmd == 'c')
        return true;
    }
  }
  return true;
}

// ==================== EXPERIMENT LIFECYCLE ====================
int main(int argc, char **argv) {
  // Frozen settings, validated by main.cpp before spawning root-owned tester.
  if (argc != 9) {
    std::cerr << "Usage: main-project-tester <browser_count> <custom_abr 0|1> "
                 "<dash_js 0|1> <launch_ms> <start_delay_ms> "
                 "<start_interval_ms> <jitter_ms> <traffic_seed>\n";
    return 2;
  }
  char *end = nullptr;
  errno = 0;
  const long count = std::strtol(argv[1], &end, 10);
  if (errno != 0 || !end || *end != '\0' || count < 0 || count > 16) {
    std::cerr << "Invalid browser count\n";
    return 2;
  }
  auto flag = [](const char *value, bool &out) {
    if (std::strcmp(value, "0") == 0) { out = false; return true; }
    if (std::strcmp(value, "1") == 0) { out = true; return true; }
    return false;
  };
  if (!flag(argv[2], gCustomAbrEnabled) || !flag(argv[3], gDashJsEnabled)) {
    std::cerr << "Invalid workload switch\n";
    return 2;
  }
  auto nonnegative = [](const char *raw, int &value, long maximum) {
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw, &end, 10);
    if (errno || !end || *end != '\0' || parsed < 0 || parsed > maximum) return false;
    value = static_cast<int>(parsed);
    return true;
  };
  if (!nonnegative(argv[4], gBrowserLaunchIntervalMs, 600000) ||
      !nonnegative(argv[5], gBrowserStartDelayMs, 3600000) ||
      !nonnegative(argv[6], gBrowserStartIntervalMs, 3600000) ||
      !nonnegative(argv[7], gBrowserStartJitterMs, 3600000)) {
    std::cerr << "Invalid browser schedule settings\n";
    return 2;
  }
  gTrafficSeed = argv[8];
  if (gTrafficSeed.empty() || gTrafficSeed.size() > 64 ||
      !std::all_of(gTrafficSeed.begin(), gTrafficSeed.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
      })) {
    std::cerr << "Invalid traffic seed\n";
    return 2;
  }
  gBrowserCount = static_cast<std::size_t>(count);
  if (gBrowserCount == 0 && !gCustomAbrEnabled && !gDashJsEnabled) {
    std::cerr << "No workloads enabled\n";
    return 2;
  }
  trace("TESTER START: executable entered");
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);
  signal(SIGHUP, onSignal);

  NetworkState net;
  Child wpr;
  std::vector<Child> browsers;
  std::vector<Connection> clients;
  std::vector<bool> ready(gBrowserCount, false);
  int listener = -1;
  bool sentStatus = false;
  bool success = false;
  std::thread networkSampler;

  // The backend launches Tester from the Main_Project working directory.
  const fs::path root = fs::current_path();
  const std::string prefix = "mp" + std::to_string(getpid());
  trace("Working directory: " + root.string());
  trace("Configuration: browsers=" + std::to_string(gBrowserCount) +
        ", customAbr=" + std::to_string(gCustomAbrEnabled) +
        ", dashJs=" + std::to_string(gDashJsEnabled) +
        ", seed=" + gTrafficSeed +
        ", launchIntervalMs=" + std::to_string(gBrowserLaunchIntervalMs) +
        ", firstDelayMs=" + std::to_string(gBrowserStartDelayMs) +
        ", startIntervalMs=" + std::to_string(gBrowserStartIntervalMs) +
        ", startJitterMs=" + std::to_string(gBrowserStartJitterMs) +
        ", bandwidth=" + std::to_string(kBandwidthMbps) + "Mbps, delay=" +
        std::to_string(kDelayMs) + "ms");

  do {
    if (gBrowserCount > 0 &&
        (!fs::is_regular_file(root / kArchiveFile) || !fs::is_regular_file(root / kJourneyFile))) {
      std::cerr << "Missing WPR archive or DigiKala journey in " << root << '\n';
      break;
    }
    std::error_code ec;
    fs::create_directories(root / "tester/experiments/browser-logs", ec);
    if (ec) {
      std::cerr << "Result directory: " << ec.message() << '\n';
      break;
    }

    trace("FILES verified; result directory created");
    if (gBrowserCount > 0) {
      listener = listenerSocket();
      if (listener == -1) {
        std::cerr << "Cannot bind readiness port " << kControlPort << '\n';
        break;
      }
    }
    trace("Starting traffic-control configuration");
    if (!setupNetwork(net)) {
      trace("NETWORK SETUP returned failure");
      break;
    }
    trace("Traffic-control configuration succeeded");
    telemetryEvent("ifb_configured");

    if (gBrowserCount > 0) {
      // WPR must bind both ports before Chrome begins; verify no existing listener.
      trace("Preflight: checking ports 80 and 443 (no WPR started yet)");
      if (portOpen(80) || portOpen(443)) {
        std::cerr << "TCP 80/443 is already in use. Stop that service before retrying.\n";
        break;
      }
      trace("WPR: launching Docker container");
      if (!startWpr(wpr, root, prefix)) {
        trace("WPR spawnProcess returned failure");
        break;
      }
      trace("WPR spawned; waiting up to 30 seconds for ports 80/443");
      bool wprReady = false;
      const auto wprDeadline = Clock::now() + std::chrono::seconds(30);
      while (!gStop && Clock::now() < wprDeadline) {
        if (hasExited(wpr))
          break;
        if (portOpen(80) && portOpen(443)) {
          wprReady = true;
          break;
        }
        poll(nullptr, 0, 200);
      }
      if (!wprReady) {
        trace("WPR startup failed: ports did not become ready");
        std::cerr << "WPR ports did not become ready\n";
        break;
      }
      trace("WPR ready on ports 80 and 443");
      telemetryEvent("wpr_ready");
    } else {
      trace("No browsers configured: skipping WPR, Docker, and readiness listener");
    }

    bool allLaunched = true;
    for (std::size_t index = 0; index < gBrowserCount && !gStop; ++index) {
      // Each container starts no sooner than the configured interval after
      // the previous one. Chrome startup may continue concurrently.
      trace("BROWSER launch begin: slot=" + std::to_string(index + 1));
      const auto launchTime = Clock::now();
      browsers.emplace_back();
      if (!startBrowser(browsers.back(), root, prefix, index + 1)) {
        trace("BROWSER spawn failure: slot=" + std::to_string(index + 1));
        allLaunched = false;
        break;
      }
      std::cerr << "Started browser " << index + 1 << '/' << gBrowserCount
                << "; journey starts after " << browserOffsetMs(index + 1) << "ms\n";
      telemetryEvent("browser_launched", std::to_string(index + 1));
      if (!notifyMain('P')) {
        allLaunched = false;
        break;
      }
      if (index + 1 < gBrowserCount &&
          !waitForNextLaunch(launchTime + std::chrono::milliseconds(gBrowserLaunchIntervalMs),
                             listener,
                             clients,
                             ready,
                             wpr,
                             browsers)) {
        allLaunched = false;
        break;
      }
    }
    if (!allLaunched || gStop || browsers.size() != gBrowserCount ||
        (gBrowserCount > 0 && browsers.back().pid <= 0)) {
      trace("BROWSER launch phase incomplete; initiating cleanup");
      break;
    }
    trace("BROWSER all containers spawned");

    // R is sent only after every custom journey has reached its waiting point.
    if (!waitForBrowsers(listener, clients, ready, wpr, browsers)) {
      trace("BROWSER readiness phase failed; initiating cleanup");
      break;
    }
    if (!notifyMain('R')) {
      trace("Failed writing READY status to main");
      break;
    }
    sentStatus = true;

    // The main process now has to send 's' explicitly; no navigation starts yet.
    if (!waitForStart(wpr, browsers, clients)) {
      trace("GO WAIT failed/cancelled; initiating cleanup");
      break;
    }
    gGoTime = Clock::now();
    telemetryEvent("go_received");
    trace("GO received; starting workloads");
    gTelemetryStop.store(false);
    networkSampler = std::thread(sampleNetwork, net.ethDevice);
    for (std::size_t slot=1; slot<=gBrowserCount; ++slot)
      telemetryEvent("browser_start_signaled", std::to_string(slot));
    for (Connection &client : clients) {
      close(client.fd);
      client.fd = -1;
    }
    clients.clear();
    if (listener >= 0) close(listener);
    listener = -1;

    // Remain active until main sends 'c' or closes its command pipe.
    success = waitForStop(wpr, browsers);
  } while (false);

  gTelemetryStop.store(true);
  if (networkSampler.joinable()) networkSampler.join();
  telemetryEvent("cleanup_begin");
  trace("CLEANUP BEGIN: sentStatus=" + std::string(sentStatus ? "true" : "false") +
        ", stopSignal=" + std::string(gStop ? "true" : "false"));
  if (!sentStatus)
    notifyMain('E');
  for (const Connection &client : clients)
    if (client.fd >= 0) {
      send(client.fd, "STOP\n", 5, MSG_NOSIGNAL);
      close(client.fd);
    }
  if (listener >= 0)
    close(listener);
  for (auto it = browsers.rbegin(); it != browsers.rend(); ++it)
    stopContainer(*it);
  if (!wpr.containerName.empty())
    stopContainer(wpr);
  if (!cleanupNetwork(net))
    success = false;
  trace(std::string("TESTER EXIT: code=") + (success ? "0" : "1"));
  return success ? 0 : 1;
}