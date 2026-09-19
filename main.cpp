#include <WinSock2.h>
#include <fstream>
#include <iostream>
#include <string>
#include <ws2tcpip.h>

bool sendAll(SOCKET &clientSocket, std::string &response);
bool sendAll(SOCKET &clientSocket, char *videoBuffer, int bytesRead);
std::string normalHeader(std::string contentType, std::string contentLength);
std::string paritialHeader(std::string contentType, std::string contentLength, std::string start, std::string end);

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
      size_t range = receivedMessage.find("Range: bytes=");
      std::cout << receivedMessage << "\n";

      std::string rangeStarting;
      std::string rangeEnding;
      if (range == std::string::npos) {
        closesocket(clientSocket);
        continue;
      }
      // Finding Range
      size_t rangeStartingPos = receivedMessage.find("Range: bytes=");
      rangeStartingPos += std::string("Range: bytes=").length();
      size_t rangeDashPos = receivedMessage.find('-', rangeStartingPos);
      rangeStarting = receivedMessage.substr(rangeStartingPos, rangeDashPos - rangeStartingPos);
      size_t rangeEndingPos = rangeDashPos + 1;
      size_t endingR = receivedMessage.find("\r", rangeEndingPos);
      rangeEnding = receivedMessage.substr(rangeEndingPos, endingR - rangeEndingPos);

      std::string filepath;
      if (path == "/video/480")
        filepath = "files/video_480.mp4";

      else if (path == "/video/720")
        filepath = "files/video_720.mp4";

      else if (path == "/video/1080")
        filepath = "files/video_1080.mp4";

      else {
        std::cout << "Wrong path chosen.";
        return -1;
      }

      std::ifstream video(filepath, std::ios::binary);
      if (!video) {
        std::cout << "opening the video was not successful";
        return -1;
      }
      video.seekg(0, std::ios::end);
      auto filesize = video.tellg();
      video.seekg(0, std::ios::beg);

      std::string contentLength = std::to_string(filesize);
      std::string header = paritialHeader("video/mp4", contentLength, rangeStarting, rangeEnding);
      std::cout << header;
      sendAll(clientSocket, header);

      video.seekg(std::stoll(rangeStarting), std::ios::beg);
      char videoBuffer[4096];
      int howMuchToRead;
      if (!rangeEnding.empty())
        howMuchToRead = std::stoll(rangeEnding) - (std::stoll(rangeStarting) - 1);
      else
        howMuchToRead = std::stoll(contentLength) - std::stoll(rangeStarting);
      while (howMuchToRead) {
        if (howMuchToRead >= sizeof(videoBuffer))
          video.read(videoBuffer, sizeof(videoBuffer));
        else if (howMuchToRead < sizeof(videoBuffer))
          video.read(videoBuffer, howMuchToRead);

        auto bytesRead = video.gcount();
        if (bytesRead <= 0)
          break;

        sendAll(clientSocket, videoBuffer, static_cast<int>(bytesRead));
        howMuchToRead -= bytesRead;
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

bool sendAll(SOCKET &clientSocket, char *videoBuffer, int bytesRead) {
  int totalSent{};
  while (totalSent < bytesRead) {
    int temp = send(clientSocket, videoBuffer + totalSent, bytesRead - totalSent, 0);
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
         "\r\n";
}

std::string paritialHeader(std::string contentType, std::string contentLength, std::string start, std::string end) {
  if (end.empty()) {
    return "HTTP/1.1 206 Partial Content \r\n"
           "Content-Length: " +
           std::to_string(std::stoll(contentLength) - (std::stoll(start))) +
           "\r\n"
           "Content-Type: " +
           contentType +
           "\r\n"
           "Accept-Ranges: bytes\r\n"
           "Content-Range: bytes " +
           start + "-" + std::to_string(std::stoll(contentLength) - 1) + "/" + contentLength +
           "\r\n"
           "Connection: close \r\n"
           "\r\n";
  } else if (!end.empty()) {
    return "HTTP/1.1 206 Partial Content \r\n"
           "Content-Length: " +
           std::to_string(std::stoll(end) - std::stoll(start) + 1) +
           "\r\n"
           "Content-Type: " +
           contentType +
           "\r\n"
           "Accept-Ranges: bytes\r\n"
           "Content-Range: bytes " +
           start + "-" + end + "/" + contentLength +
           "\r\n"
           "Connection: close \r\n"
           "\r\n";
  }
  return "";
}
