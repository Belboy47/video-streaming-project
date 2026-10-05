#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
//----------------------
#ifdef _WIN32

#include <WinSock2.h>
#include <ws2tcpip.h>

using Socket = SOCKET;

void closeSocket(Socket s) { closesocket(s); }

constexpr int SEND_FLAGS = 0;
#else

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using Socket = int;

void closeSocket(Socket s) { close(s); }

constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#endif
//-------------------

//----------------------

bool sendAll(Socket &clientSocket, std::string &response);
bool sendAll(Socket &clientSocket, char *buffer, int bytesRead);
std::string normalHeader(std::string contentType, std::string contentLength);
std::string getFileSize(std::string filePath);

int main() {
//--------------------------------------------SOCKET
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cout << "WSA Startup Failed";
    return -1;
  }
#endif
  Socket serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket == INVALID_SOCKET) {
    std::cout << "Failed to create socket.";
    return -1;
  }
  sockaddr_in ServerAddress{};
#ifdef _WIN32
  ServerAddress.sin_addr.S_un.S_addr = INADDR_ANY;

#else
  ServerAddress.sin_addr.s_addr = INADDR_ANY;
#endif
  ServerAddress.sin_port = htons(8080);
  ServerAddress.sin_family = AF_INET;
  bind(serverSocket, (const sockaddr *)&ServerAddress, sizeof(ServerAddress));
  listen(serverSocket, SOMAXCONN);
  sockaddr_in clientAddress{};
  socklen_t sizeOfClientAddress = sizeof(clientAddress);
  while (true) {
    Socket clientSocket = accept(serverSocket, (sockaddr *)&clientAddress, &sizeOfClientAddress);
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
      std::string fileName;

      firstSlashPos = path.find('/');
      secondSlashPos = path.find('/', firstSlashPos + 1);
      resourceType = path.substr(firstSlashPos + 1, secondSlashPos - firstSlashPos - 1);
      fileName = path.substr(secondSlashPos + 1);

      std::cout << resourceType << '\n';
      std::cout << fileName << '\n';
      std::string filePath = path.substr(1);

      if (resourceType.empty() || fileName.empty() || resourceType != "segments") {
        closeSocket(clientSocket);
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
        closeSocket(clientSocket);
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
    closeSocket(clientSocket);
  }
}

bool sendAll(Socket &clientSocket, std::string &response) {
  int totalSent{};
  while (totalSent < response.size()) {
    int sent = send(clientSocket, response.c_str() + totalSent, response.size() - totalSent, SEND_FLAGS);
    if (sent == SOCKET_ERROR) {
      return false;
    }
    totalSent += sent;
  }
  return true;
}

bool sendAll(Socket &clientSocket, char *buffer, int bytesRead) {
  int totalSent{};
  while (totalSent < bytesRead) {
    int temp = send(clientSocket, buffer + totalSent, bytesRead - totalSent, SEND_FLAGS);
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
