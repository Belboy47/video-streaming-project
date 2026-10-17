# Adaptive Bitrate Video Streaming (ABR)

A browser-based adaptive video streaming project with a custom C++ HTTP server, a React video player, and a C++ network-condition tester built around Linux traffic control (`tc netem`).

The goal is to investigate how throughput, available playback buffer, and changing network conditions affect video quality and playback continuity. The project emphasizes **quality of experience (QoE)**: maintaining useful video quality while limiting stalls, unnecessary quality changes, and abandoned downloads.

**Status:** Functional streaming and network-testing prototypes. ABR parameter tuning and controlled performance evaluation are in progress.

## How it works

```text
Windows browser
    |
    v
React player (Media Source Extensions)
    |
    | HTTP: manifest, initialization segments, media segments
    v
C++ HTTP server
    |
    v
Segmented video files

C++ network tester --> Linux tc/netem --> shaped server traffic
```

The player reads an MPEG-DASH MPD manifest, fetches fragmented MP4 segments, and appends them using the browser's Media Source Extensions (MSE) API. The C++ server serves the video assets over HTTP. Separately, the network tester applies timed Linux traffic-control settings so the player can be evaluated under changing conditions.

## Implemented features

### React player

- Three video representations: **480p, 720p, and 1080p**.
- MPD manifest parsing, including segment timing information.
- Fragmented MP4 playback through **MediaSource** and **SourceBuffer**.
- Automatic bitrate selection based on observed segment-download throughput.
- Buffer-aware fallback to the lowest quality when very little playback buffer remains.
- A dynamic download timeout based on estimated download time and available buffer.
- Cancellation and retry at a lower quality when a timed download attempt expires.
- Quality-switching controls, including buffer-preserving and player-reloading behaviors.
- Seeking support with cancellation of obsolete requests.
- In-memory measurements of throughput, buffering, quality changes, and playback QoE.

### C++ HTTP server

- A custom C++ server used to deliver the video manifest, initialization segments, and media segments.
- Runs locally on Linux/WSL2 for development and can be compiled for a Linux server.

### C++ network tester

- Uses Linux **`tc netem`** to apply scheduled network conditions.
- Supports predefined conditions such as high bandwidth, sudden deterioration, recovery, and increased latency.
- Uses **`std::chrono::steady_clock`** and scheduled deadlines to limit cumulative timing drift.
- Runs separately from the streaming server and player.

## Current ABR strategy

After a completed segment download, the player estimates available throughput conservatively using the lower of:

1. The throughput measured for the latest completed segment.
2. The average throughput of up to the five latest completed segments.

A safety factor is applied before selecting the next video representation. The current implementation also forces the lowest quality when less than four seconds of video remain buffered.

For download timeouts, the player estimates segment arrival time from segment size and measured throughput. Its current experimental timeout rule is:

```text
Timeout (seconds) = 1.3 × estimated download time + 0.2 × buffer remaining
```

The estimated download time uses a conservative comparison of the latest throughput and the cumulative average throughput. The initial download uses a 10-second fallback timeout; the lowest-quality download is not subject to the same timeout-based downgrade logic.

**These rules are experimental.** Their coefficients and thresholds have not yet been established as optimal by controlled tests.

## QoE and diagnostic measurements

The player currently tracks or derives:

| Measurement | Purpose |
| --- | --- |
| Playback duration at each quality | Compare time spent at 480p, 720p, and 1080p |
| Stall count | Count playback interruptions |
| Total stall duration | Measure interruption severity |
| Quality-switch count and direction | Detect frequent changes and oscillations |
| Timeout-triggered download aborts | Evaluate download retry behavior |
| Segment throughput and download duration | Estimate network performance |
| Buffer before and after segment downloads | Understand buffering behavior |
| Startup delay | Observe time to initial playback |

Measurements currently live in the application and browser console; a repeatable export/reporting pipeline is planned.

## Requirements

- A C++17-capable compiler such as **G++**.
- **Node.js** and **npm** for the React/Vite frontend.
- A modern browser with Media Source Extensions support.
- Video assets and an MPD manifest matching the paths expected by the server and player.
- **Linux** or **WSL2 Ubuntu** for `tc netem` testing.
- `iproute2` for `tc`; `iperf3` and `tcpdump` are useful optional diagnostics.
- Administrative privileges (`sudo`) to change Linux queueing disciplines.

On Ubuntu, useful packages can be installed with:

```bash
sudo apt update
sudo apt install build-essential iproute2 iperf3 tcpdump
```

Node.js can be installed independently using a Node version manager such as `nvm`.

## Running locally

The examples below assume `main.cpp` is in the repository root. **Replace the frontend and tester paths with their actual locations in your checkout.**

### 1. Build and run the C++ server

From the repository root:

```bash
g++ -std=c++17 -O2 -pthread main.cpp -o abr-server
./abr-server
```

The frontend expects the development HTTP server on **port 8080**. Make sure your segment files are present in the location the server expects. For access from Windows through WSL's network IP, the server must listen on a reachable interface, rather than only `127.0.0.1`.

### 2. Start the React frontend

In the directory containing the frontend's `package.json`:

```bash
npm install
npm run dev -- --host 0.0.0.0
```

Vite normally uses port **5173**. In WSL2, obtain the current Linux IP address with:

```bash
hostname -I
```

Open `http://<WSL-IP>:5173` in your Windows browser.

During development, the frontend targets `http://<browser-hostname>:8080`. For production builds, it uses relative `/segments/...` requests, which require appropriate same-origin hosting or reverse-proxy configuration.

### 3. Confirm that traffic shaping reaches the video connection

Do not assume that a WSL2 localhost connection passes through `eth0`. Inspect the browser's actual segment-request URL and, if necessary, capture traffic:

```bash
sudo tcpdump -ni eth0 'tcp port 8080'
```

Before benchmarking, confirm that media traffic crosses the interface you intend to shape. WSL networking modes and forwarding behavior can affect this.

## Manual network shaping

The following example applies a **4 Mbps outgoing rate limit** and **80 ms of additional one-way delay** to `eth0`:

```bash
sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 80ms
```

Inspect the configuration and counters:

```bash
sudo tc -s qdisc show dev eth0
```

Remove the test configuration when finished:

```bash
sudo tc qdisc del dev eth0 root
```

**Caution:** A root queueing discipline affects outgoing traffic on the selected interface, not only the video server. The settings remain active after the controlling program exits unless explicitly removed or the interface is reset. Use an isolated development environment and restore networking after interrupted tests.

## Automated network testing

The C++ tester is intended to play a predefined sequence of network profiles, with changes occurring at scheduled times. A representative first scenario is:

| Phase | Bandwidth | Added delay | Duration |
| --- | ---: | ---: | ---: |
| Excellent | 15 Mbps | 25 ms | 20 s |
| Sudden deterioration | 2.5 Mbps | 25 ms | 40 s |
| Recovery | 12 Mbps | 25 ms | 25 s |
| Latency spike | 12 Mbps | 300 ms | 15 s |
| Moderate network | 4 Mbps | 50 ms | 35 s |

These are **illustrative test profiles**, not standardized definitions of network quality. The initial tests keep jitter and packet loss at zero to better isolate bandwidth and latency effects.

To compile the tester, substitute its real source-file path:

```bash
g++ -std=c++17 -O2 -pthread <tester-source.cpp> -o network-tester
sudo ./network-tester
```

Always check and clear remaining traffic shaping after the experiment. Automated cleanup on interruption is an important improvement for the tester.

## Experimental methodology

The primary comparison is between ABR/timeout strategies under **the same network scenario**. Useful outcomes include:

- More playback time at higher video quality, without unacceptable interruptions.
- Fewer and shorter stalls.
- Fewer unnecessary quality oscillations.
- Fewer needless download aborts and less wasted transfer data.
- Faster appropriate adaptation after a bandwidth drop or recovery.

For meaningful comparisons, keep the video, segment representations, test sequence, initial playback state, and cache conditions consistent. Repeat each run and compare the results rather than interpreting a single trial as conclusive.

## Known limitations and next steps

- **Throughput measurement:** When a segment requires retries, the current measurement can include time spent on previous attempts, which can bias the effective throughput estimate.
- **Partial transfer waste:** Aborted download counts are tracked, but discarded bytes are not yet measured.
- **Reporting:** QoE measurements are not yet automatically exported to structured experiment files.
- **Test repeatability:** Planned improvements include synchronized playback/test starts, timestamps for actual condition changes, and repeated runs.
- **Test realism:** Future scenarios may include controlled jitter, packet loss, burst failures, and recorded network traces.
- **Platform validation:** WSL2 is useful for development, but key results should also be checked on a Linux VPS or dedicated Linux test environment.
- **Algorithm tuning:** Current quality thresholds and timeout coefficients require experimental evaluation.

## Project purpose

This is a hands-on exploration of adaptive video delivery, C++ networking, browser media APIs, Linux traffic control, and the relationship between network performance and perceived playback quality. The next major milestone is a **repeatable QoE benchmark** that provides evidence for comparing ABR strategies rather than selecting thresholds solely by intuition.
