# Video Streaming Project

An experimental video streaming project built to learn how HTTP video delivery works at a lower level.

The backend is written in **C++ using Winsock** and handles HTTP byte-range requests directly. A **React + Vite** frontend is used to play the video in the browser.

This is an early version of the project and is being developed incrementally toward a more complete adaptive video streaming system.

## Current Features

* TCP server built with Winsock
* Basic HTTP request parsing
* HTTP `Range` header parsing
* `206 Partial Content` responses
* `Content-Range` and `Content-Length` generation
* Streaming MP4 files in chunks instead of loading the entire file into memory
* Handling partial `send()` operations
* Seeking directly to requested positions in video files
* Three video quality endpoints:

  * `/video/480`
  * `/video/720`
  * `/video/1080`
* Browser playback through a React frontend

## How It Works

The browser requests part of a video using an HTTP byte range:

```http
GET /video/720 HTTP/1.1
Range: bytes=1048576-
```

The C++ server parses the range and responds with:

```http
HTTP/1.1 206 Partial Content
Content-Type: video/mp4
Accept-Ranges: bytes
Content-Range: bytes 1048576-224273952/224273953
```

The server then:

1. Opens the requested video file.
2. Seeks to the requested starting byte.
3. Reads the video in small chunks.
4. Sends those chunks through the TCP connection.
5. Closes the connection after the requested range has been transferred.

This allows the browser to seek through the video without requiring the entire file to be transferred first.

## Project Structure

```text
video-streaming-project/
├── main.cpp
├── files/
│   ├── video_480.mp4
│   ├── video_720.mp4
│   └── video_1080.mp4
├── src/
├── package.json
└── ...
```

The MP4 files are excluded from the repository through `.gitignore`.

To run the project, provide your own videos using these filenames:

```text
files/video_480.mp4
files/video_720.mp4
files/video_1080.mp4
```

## Backend

The backend currently targets Windows and uses the Winsock API.

### Requirements

* Windows
* C++ compiler
* Winsock2

Example using MinGW/G++:

```bash
g++ main.cpp -o server.exe -lws2_32
```

Run:

```bash
./server.exe
```

The server listens on:

```text
http://localhost:8080
```

Available video URLs are:

```text
http://localhost:8080/video/480
http://localhost:8080/video/720
http://localhost:8080/video/1080
```

## Frontend

The frontend uses React with Vite.

Install dependencies:

```bash
npm install
```

Start the development server:

```bash
npm run dev
```

Vite will normally make the frontend available at:

```text
http://localhost:5173
```

## Current Limitations

This is currently a learning prototype rather than a complete HTTP server.

Some current limitations include:

* One client is handled at a time.
* Only a small subset of HTTP is implemented.
* Only byte-range video requests are currently handled.
* Video paths are hardcoded.
* HTTP requests are currently read using a single `recv()` call.
* Invalid and unsatisfiable ranges are not fully handled.
* HTTP error responses such as `400`, `404`, and `416` are not yet implemented.
* No adaptive bitrate selection is implemented yet.

## Planned Development

The project is intended to gradually develop into an adaptive video streaming experiment.

Planned stages include:

* Improve HTTP request handling
* Add better error and range validation
* Support multiple simultaneous clients
* Manual switching between video qualities
* Implement adaptive bitrate selection
* Track playback and network metrics
* Simulate different network conditions
* Measure startup delay, buffering, throughput, and other QoE metrics
* Experiment with video streaming behavior under different network and VPN conditions

## Purpose

The main purpose of this project is to understand video streaming from the networking side rather than relying entirely on existing web-server frameworks.

The project explores concepts including:

* TCP sockets
* HTTP
* HTTP byte ranges
* Partial content responses
* File I/O
* Streaming data in chunks
* Browser video behavior
* Adaptive bitrate streaming
* Network performance and Quality of Experience
