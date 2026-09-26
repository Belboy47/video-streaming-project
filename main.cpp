#include <WinSock2.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <ws2tcpip.h>

bool sendAll(SOCKET &clientSocket, std::string &response);
bool sendAll(SOCKET &clientSocket, char *buffer, int bytesRead);
std::string normalHeader(std::string contentType, std::string contentLength);
std::string getFileSize(std::string filePath);

int main() {
  //--------------------------------------------SOCKET
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cout << "WSA Startup Failed";
    return -1;
  }
  SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket == INVALID_SOCKET) {
    std::cout << "Failed to create socket.";
    return -1;
  }
  sockaddr_in ServerAddress{};
  ServerAddress.sin_addr.S_un.S_addr = INADDR_ANY;
  ServerAddress.sin_port = htons(8080);
  ServerAddress.sin_family = AF_INET;
  bind(serverSocket, (const sockaddr *)&ServerAddress, sizeof(ServerAddress));
  listen(serverSocket, SOMAXCONN);
  sockaddr_in clientAddress{};
  int sizeOfClientAddress = sizeof(clientAddress);
  while (true) {
    SOCKET clientSocket = accept(serverSocket, (sockaddr *)&clientAddress, &sizeOfClientAddress);
    std::cout << "Request Received\n";
    //-------------------------------------------------RECEIVING
    char buffer[1024];
    std::string receivedMessage;
    int received = recv(clientSocket, buffer, sizeof(buffer), 0);
    if (received > 0) {
      receivedMessage.append(buffer, received);
      size_t firstSpace = receivedMessage.find(' ');
      size_t secondSpace = receivedMessage.find(' ', firstSpace + 1);
      size_t lineEnd = receivedMessage.find("\r\n");
      std::string method = receivedMessage.substr(0, firstSpace);
      std::string path = receivedMessage.substr(firstSpace + 1, (secondSpace - firstSpace) - 1);
      std::string version = receivedMessage.substr(secondSpace + 1, (lineEnd - secondSpace) - 1);
      std::cout << receivedMessage << "\n";

      size_t firstSlashPos;
      size_t secondSlashPos;
      size_t thirdSlashPos;
      std::string resourceType;
      std::string quality;
      std::string fileName;

      if (path.find("manifest.mpd") != std::string::npos) {
        firstSlashPos = path.find('/');
        secondSlashPos = path.find('/', firstSlashPos + 1);
        resourceType = path.substr(firstSlashPos + 1, secondSlashPos - firstSlashPos - 1);
        fileName = path.substr(secondSlashPos + 1);
      } else {
        firstSlashPos = path.find('/');
        secondSlashPos = path.find('/', firstSlashPos + 1);
        thirdSlashPos = path.find('/', secondSlashPos + 1);
        resourceType = path.substr(firstSlashPos + 1, secondSlashPos - firstSlashPos - 1);
        quality = path.substr(secondSlashPos + 1, thirdSlashPos - secondSlashPos - 1);
        fileName = path.substr(thirdSlashPos + 1);
      }
      std::cout << resourceType << '\n';
      std::cout << quality << '\n';
      std::cout << fileName << '\n';
      std::string filePath = path.substr(1);

      if (resourceType.empty() || fileName.empty() || resourceType != "segments" ||
          !quality.empty() && quality != "480" && quality != "720" && quality != "1080") {
        closesocket(clientSocket);
        continue;
      }
      std::string fileType;
      if (fileName.ends_with(".mpd"))
        fileType = "application/dash+xml";
      else if (fileName.ends_with(".mp4"))
        fileType = "video/mp4";
      else if (fileName.ends_with(".m4s"))
        fileType = "video/iso.segment";

      std::ifstream file(filePath, std::ios::binary);
      if (!file) {
        std::cout << "File DOES NOT EXIST";
        closesocket(clientSocket);
        continue;
      }
      std::string header = normalHeader(fileType, getFileSize(filePath));
      sendAll(clientSocket, header);

      char buffer[4096];
      while (file) {
        file.read(buffer, sizeof(buffer));

        auto bytesRead = file.gcount();

        if (bytesRead > 0) {
          sendAll(clientSocket, buffer, static_cast<int>(bytesRead));
        }
      }
    }
    closesocket(clientSocket);
  }
}

bool sendAll(SOCKET &clientSocket, std::string &response) {
  int totalSent{};
  while (totalSent < response.size()) {
    int sent = send(clientSocket, response.c_str() + totalSent, response.size() - totalSent, 0);
    if (sent == SOCKET_ERROR) {
      return false;
    }
    totalSent += sent;
  }
  return true;
}

bool sendAll(SOCKET &clientSocket, char *buffer, int bytesRead) {
  int totalSent{};
  while (totalSent < bytesRead) {
    int temp = send(clientSocket, buffer + totalSent, bytesRead - totalSent, 0);
    if (temp == SOCKET_ERROR)
      return false;

    totalSent += temp;
  }

  return true;
}

std::string normalHeader(std::string contentType, std::string contentLength) {

  return "HTTP/1.1 200 OK \r\n"
         "Content-Length: " +
         contentLength +
         "\r\n"
         "Content-Type: " +
         contentType +
         "\r\n"
         "Access-Control-Allow-Origin: *\r\n"
         "Connection: close\r\n"
         "\r\n";
}

std::string getFileSize(std::string filePath) {
  auto fileSize = std::filesystem::file_size(filePath);
  std::string fileSizeSTR = std::to_string(fileSize);
  return fileSizeSTR;
}
