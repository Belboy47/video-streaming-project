#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

void cancelThread(std::atomic_bool &isExcutingCommand);

int main() {
  struct networkCondition {
    double bandwidthMbps;
    int latencyMs;
    int jitterMs;
    double lossPercent;
    int durationSeconds;
  };
  std::atomic_bool isCommandExcuted(false);
  std::thread t1(cancelThread, std::ref(isCommandExcuted));

  std::vector<networkCondition> networkConditionVec;

  networkCondition excellent{15.0, 25, 0, 0.0, 20};
  networkCondition suddenDeterioration{2.5, 25, 0, 0.0, 40};
  networkCondition recovery{12.0, 25, 0, 0.0, 25};
  networkCondition latencySpike{12.0, 300, 0, 0.0, 15};
  networkCondition moderateNetwork{4.0, 50, 0, 0.0, 35};

  networkConditionVec.push_back(excellent);
  networkConditionVec.push_back(suddenDeterioration);
  networkConditionVec.push_back(recovery);
  networkConditionVec.push_back(latencySpike);
  networkConditionVec.push_back(moderateNetwork);

  while (true) {
    for (int i = 0; i < networkConditionVec.size(); i++) {
      std::ostringstream command;
      auto start = std::chrono::steady_clock::now();

      command << "tc qdisc replace dev eth0 root netem rate "
              << networkConditionVec[i].bandwidthMbps << "mbit delay "
              << networkConditionVec[i].latencyMs << "ms";

      isCommandExcuted = false;
      std::system(command.str().c_str());
      isCommandExcuted = true;
      std::cout << "Excuted Number " << i + 1 << std::endl;

      std::this_thread::sleep_until(start +
                                    std::chrono::seconds(networkConditionVec[i].durationSeconds));
    }
  }
  //----------------------------
}

void cancelThread(std::atomic_bool &isCommandExcuted) {
  char cancelCMD;

  std::cin >> cancelCMD;

  if (cancelCMD == 'c') {
    while (!isCommandExcuted) {
    }
    std::system("tc qdisc del dev eth0 root");
    std::cout << "Reverted Back! " << "\n";
    std::exit(0);
  }
}
