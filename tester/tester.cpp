#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

bool waitUntilOrCancelled(std::chrono::steady_clock::time_point deadline) {
  while (true) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now())
                         .count();

    if (remaining <= 0)
      return false;

    pollfd inputFd{};
    inputFd.fd = STDIN_FILENO;
    inputFd.events = POLLIN;

    int result = poll(&inputFd, 1, static_cast<int>(remaining));

    if (result == 0)
      return false;

    if (result == -1) {
      if (errno == EINTR)
        continue;

      std::cerr << "poll failed: " << std::strerror(errno) << '\n';
      return true;
    }

    if (inputFd.revents & (POLLIN | POLLHUP)) {
      char input[64];

      ssize_t bytesRead = read(STDIN_FILENO, input, sizeof(input));

      if (bytesRead == 0)
        return true;

      if (bytesRead == -1) {
        if (errno == EINTR)
          continue;

        std::cerr << "read failed: " << std::strerror(errno) << '\n';
        return true;
      }

      for (ssize_t i = 0; i < bytesRead; i++) {
        if (input[i] == 'c' || input[i] == 'C')
          return true;
      }
    }

    if (inputFd.revents & (POLLERR | POLLNVAL))
      return true;
  }
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  struct networkCondition {
    double bandwidthMbps;
    int latencyMs;
    int jitterMs;
    double lossPercent;
    int durationSeconds;
  };

  std::vector<networkCondition> profileMain;

  networkCondition excellent{15.0, 25, 0, 0.0, 20};
  networkCondition suddenDeterioration{0.8, 25, 0, 0.0, 35};
  networkCondition recovery{10.0, 25, 0, 0.0, 30};
  networkCondition moderate{2.0, 50, 0, 0.0, 45};

  profileMain.push_back(excellent);
  profileMain.push_back(suddenDeterioration);
  profileMain.push_back(recovery);
  profileMain.push_back(moderate);

  bool stopRequested = false;
  bool qdiscMayBeActive = false;
  bool startupStatusSent = false;

  while (!stopRequested) {
    for (size_t i = 0; i < profileMain.size(); i++) {

      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(profileMain[i].durationSeconds);

      std::ostringstream command;

      command << "tc qdisc replace dev eth0 root netem rate " << profileMain[i].bandwidthMbps
              << "mbit delay " << profileMain[i].latencyMs << "ms";

      int commandStatus = std::system(command.str().c_str());

      if (commandStatus == 0)
        qdiscMayBeActive = true;

      if (!startupStatusSent) {
        startupStatusSent = true;
        const char readyMessage = commandStatus == 0 ? 'R' : 'E';
        if (write(STDOUT_FILENO, &readyMessage, 1) != 1) {
          std::cerr << "Failed to send startup status\n";
          stopRequested = true;
          break;
        }
      }

      if (commandStatus != 0) {
        std::cerr << "tc command failed\n";
        stopRequested = true;
        break;
      }
      std::cerr << "Executed Number " << i + 1 << std::endl;

      if (waitUntilOrCancelled(deadline)) {
        stopRequested = true;
        break;
      }
    }
  }

  if (qdiscMayBeActive) {
    int cleanupStatus = std::system("tc qdisc del dev eth0 root");

    if (cleanupStatus == 0)
      std::cerr << "Network configuration reverted!\n";
    else
      std::cerr << "Failed to remove tc qdisc\n";
  }

  return 0;
}
