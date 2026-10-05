import { useState, useRef, useEffect } from "react";

import "./App.css";

function App() {
  const API_BASE = import.meta.env.DEV ? "http://127.0.0.1:8080" : "";

  //----------------------TODO: Make Similar ones into an Object
  const [qualityText, setQualityText] = useState(720);
  const [manualQuality, setManualQuality] = useState(1);
  const qualityNumberRef = useRef(1);
  const savedTime = useRef(0);
  const wasPaused = useRef(true);
  const videoRef = useRef(null);
  const sourceBufferRef = useRef(null);
  const nextSegmentRef = useRef(2);
  const segmentTimelineRef = useRef([]);
  const isSeekingRef = useRef(false);
  const isBufferingRef = useRef(false);
  const seekIdRef = useRef(0);
  const seekTimeoutRef = useRef(null);
  const [manifestData, setManifestData] = useState(null);
  const playerIdRef = useRef(1);
  const qualityIdRef = useRef(1);
  const mediaSourceRef = useRef(null);
  //-----------------------------------
  const stallCountRef = useRef(0);
  const stallDurationRef = useRef(0);
  const stallDurationTotalRef = useRef(0);
  const isCountingStallRef = useRef(false);
  const stallTimerStartRef = useRef(0);
  const stallTimerEndRef = useRef(0);
  const hasPlaybackStartedRef = useRef(false);

  const startupDelayStartRef = useRef(0);
  const startupDelayEndRef = useRef(0);
  const startupDelayRef = useRef(0);

  const qualitySwitchCountRef = useRef(0);
  const swtichDirRef = useRef([]);

  const durationOn480Ref = useRef(0);
  const durationOn720Ref = useRef(0);
  const durationOn1080Ref = useRef(0);
  const qualityStartTimeRef = useRef(0);
  //-------------
  const segmentInfoArrayRef = useRef([]);
  const staticsArrayRef = useRef([]);

  useEffect(() => {
    startupDelayStartRef.current = performance.now();
    async function proccessManifestFile() {
      const manifestFile = await (
        await fetch(`${API_BASE}/segments/manifest.mpd`)
      ).text();
      const parser = new DOMParser();
      const manifestXml = parser.parseFromString(
        manifestFile,
        "application/xml",
      );
      const mpdElement = manifestXml.documentElement;
      const duration = xmlVideoDurationParser(
        mpdElement.getAttribute("mediaPresentationDuration"),
      );
      const timescaleElement = mpdElement
        .querySelector("SegmentTemplate")
        .getAttribute("timescale");

      const segmentTimelineElement = mpdElement
        .querySelector("SegmentTimeline")
        .querySelectorAll("S");

      const timeLine = [];
      for (const s of segmentTimelineElement) {
        timeLine.push({
          segmentNumber: timeLine.length + 1,
          duration: s.getAttribute("d") / timescaleElement,
        });

        if (s.getAttribute("r")) {
          for (let i = 0; i < s.getAttribute("r"); i++) {
            timeLine.push({
              segmentNumber: timeLine.length + 1,
              duration: s.getAttribute("d") / timescaleElement,
            });
          }
        }
      }
      setManifestData({ duration, timeLine });
    }
    proccessManifestFile();
  }, []);

  useEffect(() => {
    if (!manifestData) return;
    const mediaSource = new MediaSource();
    const url = URL.createObjectURL(mediaSource);
    videoRef.current.src = url;
    mediaSource.addEventListener("sourceopen", async () => {
      //---------------------------------------------
      const sourceBuffer = mediaSource.addSourceBuffer(
        'video/mp4; codecs="avc1.64001e"',
      );
      segmentTimelineRef.current = manifestData.timeLine;
      mediaSource.duration = manifestData.duration;
      sourceBufferRef.current = sourceBuffer;
      mediaSourceRef.current = mediaSource;
      await setInitialVideo(mediaSource);
      findSoughtPosAndAppend();
      sourceBuffer.addEventListener("updateend", () => checkBuffer());
    });
  }, [manualQuality, manifestData]);

  function preservePauseState() {
    savedTime.current = videoRef.current.currentTime;
    wasPaused.current = videoRef.current.paused;
  }

  function resetPlayer() {
    playerIdRef.current++;
    segmentTimelineRef.current = [];
    nextSegmentRef.current = null;
    isSeekingRef.current = false;
    isBufferingRef.current = false;
  }

  async function setInitialVideo() {
    const playerId = playerIdRef.current;
    const qualityId = qualityIdRef.current;
    const sourceBuffer = sourceBufferRef.current;
    const response = await (
      await fetch(`${API_BASE}/segments/init_${qualityNumberRef.current}.mp4`)
    ).arrayBuffer();

    if (
      mediaSourceRef.current.readyState !== "open" ||
      playerId !== playerIdRef.current
    )
      return;

    if (sourceBuffer.updating) {
      await new Promise((resolve) => {
        sourceBuffer.addEventListener("updateend", resolve, {
          once: true,
        });
      });
    }
    if (
      mediaSourceRef.current.readyState !== "open" ||
      playerId !== playerIdRef.current ||
      qualityId !== qualityIdRef.current
    )
      return;
    sourceBuffer.appendBuffer(response);
  }
  function setPlayback(qualityNumber) {
    if (qualityNumberRef.current === 0)
      durationOn480Ref.current +=
        videoRef.current.currentTime - qualityStartTimeRef.current;
    else if (qualityNumberRef.current === 1)
      durationOn720Ref.current +=
        videoRef.current.currentTime - qualityStartTimeRef.current;
    else
      durationOn1080Ref.current +=
        videoRef.current.currentTime - qualityStartTimeRef.current;

    qualityStartTimeRef.current = videoRef.current.currentTime;

    if (qualityNumber > qualityNumberRef.current)
      swtichDirRef.current.push("upgrade");
    else swtichDirRef.current.push("downgrade");

    qualitySwitchCountRef.current++;
    qualityIdRef.current++;
    qualityNumberRef.current = qualityNumber;
    setQualityText(qualityNumber == 0 ? 480 : qualityNumber == 1 ? 720 : 1080);
    setInitialVideo();
    preservePauseState();
    console.log("New Quality Set!");
    console.log("Quality Switch COUNT: ", qualitySwitchCountRef.current);
    console.log("DIRECTION OF UPDATES: ", swtichDirRef.current);

    console.log("Time Spent on 480p: ", durationOn480Ref.current);
    console.log("Time Spent on 720p: ", durationOn720Ref.current);
    console.log("Time Spent on 1080p: ", durationOn1080Ref.current);
  }

  function getBufferLeft() {
    const currentTimeNew = videoRef.current.currentTime;
    let bufferInSeconds;
    let bufferLeft;

    for (let i = 0; i < videoRef.current.buffered.length; i++) {
      if (
        currentTimeNew >= videoRef.current.buffered.start(i) &&
        currentTimeNew < videoRef.current.buffered.end(i)
      ) {
        bufferInSeconds = videoRef.current.buffered.end(i);
        break;
      }
    }

    if (bufferInSeconds) return (bufferLeft = bufferInSeconds - currentTimeNew);
  }

  async function checkBuffer() {
    if (isBufferingRef.current) return;
    if (isSeekingRef.current) return;
    const playerId = playerIdRef.current;
    const sourceBuffer = sourceBufferRef.current;

    isBufferingRef.current = true;
    let bufferLeft = null;

    bufferLeft = getBufferLeft();

    console.log(bufferLeft);
    if (playerId !== playerIdRef.current) return;
    if (
      bufferLeft < 4 &&
      nextSegmentRef.current <= segmentTimelineRef.current.length &&
      !sourceBuffer.updating
    ) {
      if (playerId !== playerIdRef.current) return;
      if (!(await fetchAndAppendSegment())) {
        isBufferingRef.current = false;
        return;
      }
      nextSegmentRef.current++;
      console.log("Shortage of Buffer");
    }
    isBufferingRef.current = false;
  }

  async function findSoughtPosAndAppend() {
    isSeekingRef.current = true;
    seekIdRef.current++;
    const seekId = seekIdRef.current;

    const now = videoRef.current.currentTime;
    let startTime = 0;

    for (let i = 0; i < segmentTimelineRef.current.length; i++) {
      let duration = segmentTimelineRef.current[i].duration;

      const endTime = startTime + duration;
      if (now < endTime && now >= startTime) {
        console.log("Segment NUMBER IS: ", i + 1);
        if (seekId != seekIdRef.current) return;
        if (!(await fetchAndAppendSegment(i + 1, seekId))) {
          isSeekingRef.current = false;
          return;
        }
        nextSegmentRef.current = i + 2;
        isSeekingRef.current = false;
        break;
      }
      startTime = endTime;
    }
    isSeekingRef.current = false;
  }

  async function fetchAndAppendSegment(
    segmentNumber = nextSegmentRef.current,
    seekId = seekIdRef.current,
  ) {
    const segmentInfo = {
      segmentNumber: null,
      quality: null,
      size: null,
      buffer_before_download: null,
      downloadTime: null,
      throughput_speed: null,
      buffer_after_download: null,
      bufferBeforeRequest: null,
    };
    const statics = {
      average_throughput: null,
      average_buffer_before_download: null,
      minimum_throughput: null,
      maximum_throughput: 0,
    };
    const qualityId = qualityIdRef.current;
    segmentInfo.segmentNumber = segmentNumber;
    segmentInfo.quality = qualityNumberRef.current;
    const playerId = playerIdRef.current;
    let paddedSegmentNumber = String(segmentNumber).padStart(5, "0");
    segmentInfo.bufferBeforeRequest = getBufferLeft();
    if (playerId !== playerIdRef.current) return false;
    const start = performance.now();
    const response = await fetch(
      `${API_BASE}/segments/seg_${qualityNumberRef.current}_${paddedSegmentNumber}.m4s`,
    );
    const data = await response.arrayBuffer();
    segmentInfo.size = data.byteLength;
    const end = performance.now();
    segmentInfo.downloadTime = end - start;
    segmentInfo.throughput_speed =
      (segmentInfo.size * 8) / 1000000 / (segmentInfo.downloadTime / 1000);
    if (seekId != seekIdRef.current || playerId !== playerIdRef.current)
      return false;
    if (sourceBufferRef.current.updating) {
      if (seekId != seekIdRef.current || playerId !== playerIdRef.current)
        return;
      await new Promise((resolve) => {
        sourceBufferRef.current.addEventListener("updateend", resolve, {
          once: true,
        });
      });
    }
    if (
      seekId != seekIdRef.current ||
      playerId !== playerIdRef.current ||
      qualityIdRef.current != qualityId
    )
      return false;

    segmentInfo.buffer_before_download = getBufferLeft();
    sourceBufferRef.current.appendBuffer(data);

    await new Promise((resolve) => {
      sourceBufferRef.current.addEventListener("updateend", resolve, {
        once: true,
      });
    });
    segmentInfo.buffer_after_download = getBufferLeft();
    //---------------------------------------------------

    segmentInfoArrayRef.current.push(segmentInfo);
    console.log(segmentInfoArrayRef.current);

    const segmentInfoArray = segmentInfoArrayRef.current;
    let hasMinimum = false;

    for (let i = 0; i < segmentInfoArray.length; i++) {
      statics.average_throughput += segmentInfoArray[i].throughput_speed;
      if (segmentInfoArray[i].buffer_before_download != undefined)
        statics.average_buffer_before_download +=
          segmentInfoArray[i].buffer_before_download;
      if (!hasMinimum) {
        statics.minimum_throughput = segmentInfoArray[i].throughput_speed;
        hasMinimum = true;
      }
      if (statics.minimum_throughput > segmentInfoArray[i].throughput_speed)
        statics.minimum_throughput = segmentInfoArray[i].throughput_speed;

      if (statics.maximum_throughput < segmentInfoArray[i].throughput_speed)
        statics.maximum_throughput = segmentInfoArray[i].throughput_speed;
    }
    if (segmentInfoArray.length > 0) {
      statics.average_throughput /= segmentInfoArray.length;
      statics.average_buffer_before_download /= segmentInfoArray.length;
    }
    console.log(statics);

    staticsArrayRef.current.push(statics);
    return true;
  }

  //------------------------------------------------------
  function xmlVideoDurationParser(duration) {
    const tPos = duration.indexOf("T");
    const hPos = duration.indexOf("H");
    const mPos = duration.indexOf("M");
    const sPos = duration.indexOf("S");

    const hour = hPos != -1 ? tPos + 1 : -1;
    const minute = mPos != -1 ? (hPos != -1 ? hPos + 1 : tPos + 1) : -1;
    const second =
      sPos != -1
        ? mPos != -1
          ? mPos + 1
          : hPos != -1
            ? hPos + 1
            : tPos + 1
        : -1;

    const hourValue = hour != -1 ? Number(duration.substring(hour, hPos)) : 0;
    const minuteValue =
      minute != -1 ? Number(duration.substring(minute, mPos)) : 0;
    const secondValue =
      second != -1 ? Number(duration.substring(second, sPos)) : 0;

    const totalSeconds = hourValue * 3600 + minuteValue * 60 + secondValue;

    return totalSeconds;
  }

  return (
    <>
      <div>
        <div
          style={{
            marginLeft: "500px",
            display: "flex",
            alignItems: "center",
            gap: "20px",
          }}
        >
          <h2>Current Quality: {qualityText ?? manualQuality}</h2>
        </div>
        <video
          ref={videoRef}
          controls
          autoPlay
          style={{ width: "1280px", height: "720px" }}
          onLoadedMetadata={() => {
            videoRef.current.currentTime = savedTime.current;
            if (!wasPaused.current) {
              videoRef.current.play();
            }
          }}
          onTimeUpdate={() => checkBuffer()}
          onSeeking={() => {
            clearTimeout(seekTimeoutRef.current);
            seekTimeoutRef.current = setTimeout(() => {
              findSoughtPosAndAppend();
            }, 150);
          }}
          onWaiting={() => {
            if (
              !videoRef.current.paused &&
              !isSeekingRef.current &&
              videoRef.current.currentTime < videoRef.current.duration &&
              hasPlaybackStartedRef.current &&
              !isCountingStallRef.current
            ) {
              stallCountRef.current++;
              stallTimerStartRef.current = performance.now();
              isCountingStallRef.current = true;
            }
          }}
          onPlaying={() => {
            if (!hasPlaybackStartedRef.current) {
              qualityStartTimeRef.current = videoRef.current.currentTime;
              startupDelayEndRef.current = performance.now();
              startupDelayRef.current =
                startupDelayEndRef.current - startupDelayStartRef.current;
              console.log("Startup Delay: ", startupDelayRef.current);

              hasPlaybackStartedRef.current = true;
            }
            if (isCountingStallRef.current) {
              stallTimerEndRef.current = performance.now();
              isCountingStallRef.current = false;
              stallDurationRef.current =
                stallTimerEndRef.current - stallTimerStartRef.current;

              stallDurationTotalRef.current += stallDurationRef.current;

              console.log("DURATION: ", stallDurationRef.current);
              console.log("TOTAL DURATION: ", stallDurationTotalRef.current);

              console.log("TIMES: ", stallCountRef.current);
            }
          }}
          onEnded={() => {
            if (qualityNumberRef.current === 0)
              durationOn480Ref.current +=
                videoRef.current.currentTime - qualityStartTimeRef.current;
            else if (qualityNumberRef.current === 1)
              durationOn720Ref.current +=
                videoRef.current.currentTime - qualityStartTimeRef.current;
            else
              durationOn1080Ref.current +=
                videoRef.current.currentTime - qualityStartTimeRef.current;

            qualityStartTimeRef.current = videoRef.current.currentTime;
          }}
        ></video>
        <div>
          <h1>ABR-Behaving Buttons:</h1>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 0) {
                setPlayback(0);
                qualityNumberRef.current = 0;
              }
            }}
          >
            480p
          </button>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 1) {
                setPlayback(1);
                qualityNumberRef.current = 1;
              }
            }}
          >
            720p
          </button>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 2) {
                setPlayback(2);
                qualityNumberRef.current = 2;
              }
            }}
          >
            1080p
          </button>
        </div>
        <div>
          <h1>Normal Behaviour Buttons:</h1>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 0) {
                qualityNumberRef.current = 0;
                setManualQuality(0);
                setQualityText(480);
                preservePauseState();
                resetPlayer();
              }
            }}
          >
            480p
          </button>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 1) {
                qualityNumberRef.current = 1;
                setManualQuality(1);
                setQualityText(720);
                preservePauseState();
                resetPlayer();
              }
            }}
          >
            720p
          </button>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 2) {
                qualityNumberRef.current = 2;
                setManualQuality(2);
                setQualityText(1080);
                preservePauseState();
                resetPlayer();
              }
            }}
          >
            1080p
          </button>
        </div>
      </div>
    </>
  );
}
export default App;
