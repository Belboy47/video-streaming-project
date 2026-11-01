#include "httplib.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

std::atomic_bool isExperimentRunning{false};
std::mutex controlPipeMutex;
int controlPipeWriteFd = -1;
bool stopWasSent = false;

extern char **environ;

// Wait for one startup byte from Tester: 'R' means the first tc command succeeded.
// All other results, including EOF or a timeout, mean startup is not confirmed.
bool waitForTesterReady(int readFd, bool &timedOut) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  timedOut = false;

  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
      timedOut = true;
      return false;
    }

    pollfd fd{readFd, POLLIN, 0};
    const int result = poll(&fd, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result == -1) return false;
    if (result == 0) {
      timedOut = true;
      return false;
    }

    if (fd.revents & (POLLIN | POLLHUP)) {
      char status = 0;
      ssize_t count;
      do {
        count = read(readFd, &status, 1);
      } while (count == -1 && errno == EINTR);
      return count == 1 && status == 'R';
    }
    if (fd.revents & (POLLERR | POLLNVAL)) return false;
  }
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  nlohmann::json response = {{"status", "started"}, {"success", true}};

  httplib::Server server;
  bool mounted = server.set_mount_point("/segments", "./segments");
  if (mounted) {
    server.set_file_extension_and_mimetype_mapping("mpd", "application/dash+xml");
    server.set_file_extension_and_mimetype_mapping("mp4", "video/mp4");
    server.set_file_extension_and_mimetype_mapping("m4s", "video/iso.segment");
  }

  server.set_post_routing_handler([](const httplib::Request &req, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Origin", "http://172.19.188.84:5173");
  });
  //========================================POST
  server.Options("/experiment/start", [](const httplib::Request &req, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    res.status = 200;
  });
  server.Post("/experiment/start", [&](const httplib::Request &req, httplib::Response &res) {
    if (isExperimentRunning.exchange(true)) {
      res.status = 409;
      res.set_content(R"({"error": "An Experiment is already running"})", "application/json");
      return;
    }

    int controlPipe[2];

    if (pipe(controlPipe) == -1) {
      int error = errno;
      isExperimentRunning.store(false);

      res.status = 500;
      res.set_content(nlohmann::json{{"error", std::strerror(error)}}.dump(), "application/json");
      return;
    }
    int readyPipe[2];
    if (pipe(readyPipe) == -1) {
      int error = errno;
      close(controlPipe[0]);
      close(controlPipe[1]);
      isExperimentRunning.store(false);

      res.status = 500;
      res.set_content(nlohmann::json{{"error", std::strerror(error)}}.dump(), "application/json");
      return;
    }

    posix_spawn_file_actions_t actions;

    int actionError = posix_spawn_file_actions_init(&actions);

    if (actionError != 0) {
      close(controlPipe[0]);
      close(controlPipe[1]);
      close(readyPipe[0]);
      close(readyPipe[1]);
      isExperimentRunning.store(false);

      res.status = 500;
      res.set_content(nlohmann::json{{"error", std::strerror(actionError)}}.dump(),
                      "application/json");
      return;
    }

    actionError = posix_spawn_file_actions_adddup2(&actions, controlPipe[0], STDIN_FILENO);

    if (actionError == 0) {
      actionError = posix_spawn_file_actions_addclose(&actions, controlPipe[0]);
    }

    if (actionError == 0) {
      actionError = posix_spawn_file_actions_addclose(&actions, controlPipe[1]);
    }

    if (actionError == 0) {
      actionError = posix_spawn_file_actions_adddup2(&actions, readyPipe[1], STDOUT_FILENO);
    }

    if (actionError == 0) {
      actionError = posix_spawn_file_actions_addclose(&actions, readyPipe[0]);
    }

    if (actionError == 0) {
      actionError = posix_spawn_file_actions_addclose(&actions, readyPipe[1]);
    }

    if (actionError != 0) {
      posix_spawn_file_actions_destroy(&actions);
      close(controlPipe[0]);
      close(controlPipe[1]);
      close(readyPipe[0]);
      close(readyPipe[1]);
      isExperimentRunning.store(false);

      res.status = 500;
      res.set_content(nlohmann::json{{"error", std::strerror(actionError)}}.dump(),
                      "application/json");
      return;
    }

    pid_t childPid;

    char arg0[] = "sudo";
    char arg1[] = "-n";
    char arg2[] = "/usr/local/bin/main-project-tester";
    char *args[] = {arg0, arg1, arg2, nullptr};

    int error = posix_spawn(&childPid, "/usr/bin/sudo", &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(controlPipe[0]);
    close(readyPipe[1]);

    if (error != 0) {
      close(controlPipe[1]);
      close(readyPipe[0]);
      isExperimentRunning.store(false);

      res.status = 500;
      res.set_content(nlohmann::json{{"error", std::strerror(error)}}.dump(), "application/json");
      return;
    }

    {
      std::lock_guard<std::mutex> lock(controlPipeMutex);

      controlPipeWriteFd = controlPipe[1];
      stopWasSent = false;
    }

    std::thread([childPid]() {
      int status;
      pid_t result;

      do {
        result = waitpid(childPid, &status, 0);
      } while (result == -1 && errno == EINTR);

      if (result == -1) {
        std::cerr << "waitpid failed: " << std::strerror(errno) << '\n';

      } else {
        if (WIFEXITED(status)) {
          std::cerr << "Tester exited with code " << WEXITSTATUS(status) << '\n';

        } else if (WIFSIGNALED(status)) {
          std::cerr << "Tester terminated by signal " << WTERMSIG(status) << '\n';
        }
        {
          std::lock_guard<std::mutex> lock(controlPipeMutex);

          if (controlPipeWriteFd != -1) {
            close(controlPipeWriteFd);
            controlPipeWriteFd = -1;
          }

          stopWasSent = false;
        }

        isExperimentRunning.store(false);
      }
    }).detach();

    bool timedOut = false;
    const bool ready = waitForTesterReady(readyPipe[0], timedOut);
    close(readyPipe[0]);

    if (!ready) {
      // Closing the command pipe tells Tester to cancel as soon as it polls stdin.
      {
        std::lock_guard<std::mutex> lock(controlPipeMutex);
        if (controlPipeWriteFd != -1) {
          close(controlPipeWriteFd);
          controlPipeWriteFd = -1;
        }
        stopWasSent = false;
      }

      res.status = timedOut ? 504 : 500;
      res.set_content(R"({"error":"Tester did not confirm network readiness"})", "application/json");
      return;
    }

    nlohmann::json response = {{"status", "ready"}, {"success", true}};

    res.status = 200;
    res.set_content(response.dump(), "application/json");
  });

  server.Options("/experiment/stop", [](const httplib::Request &req, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    res.status = 204;
  });

  server.Post("/experiment/stop", [](const httplib::Request &req, httplib::Response &res) {
    std::lock_guard<std::mutex> lock(controlPipeMutex);

    if (!isExperimentRunning.load()) {
      res.status = 409;
      res.set_content(R"({"error":"No experiment is running"})", "application/json");
      return;
    }

    if (controlPipeWriteFd == -1) {
      res.status = 409;
      res.set_content(R"({"error":"Experiment is not ready to receive commands"})",
                      "application/json");
      return;
    }

    if (stopWasSent) {
      res.status = 202;
      res.set_content(R"({"status":"stop_already_requested"})", "application/json");
      return;
    }

    const char stopCommand[] = "c\n";

    ssize_t bytesWritten = write(controlPipeWriteFd, stopCommand, sizeof(stopCommand) - 1);

    if (bytesWritten == -1) {
      int error = errno;

      res.status = 500;
      res.set_content(nlohmann::json{{"error", std::strerror(error)}}.dump(), "application/json");
      return;
    }

    if (bytesWritten != 2) {
      res.status = 500;
      res.set_content(R"({"error":"Incomplete stop command write"})", "application/json");
      return;
    }

    stopWasSent = true;

    res.status = 202;
    res.set_content(R"({"status":"stop_requested"})", "application/json");
  });

  server.listen("0.0.0.0", 8080);
}
