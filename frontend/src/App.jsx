import { useState, useRef, useEffect } from "react";

import "./App.css";

function App() {
  const API_BASE = import.meta.env.DEV ? `http://${window.location.hostname}:8080` : "";

  // TODO: Refactor related refs into playerRef / qoeRef / measurementRef
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
  const playerIdRef = useRef(1);
  const qualityIdRef = useRef(1);
  const mediaSourceRef = useRef(null);
  const hasPlaybackStartedRef = useRef(false);

  const [qualityText, setQualityText] = useState(720);
  const [manualQuality, setManualQuality] = useState(1);
  const [manifestData, setManifestData] = useState(null);
  //-----------------------------------
  const isCountingStallRef = useRef(false);
  const stallTimerStartRef = useRef(0);
  const stallTimerEndRef = useRef(0);

  const startupDelayStartRef = useRef(0);
  const startupDelayEndRef = useRef(0);
  const startupDelayRef = useRef(0);

  const swtichDirRef = useRef([]);

  const qualityStartTimeRef = useRef(0);
  const qualityEndTimeRef = useRef(0);
  const partialQualityTimeRef = useRef(0);
  const qualityIntervalActiveRef = useRef(false);

  const qualityOfExprienceRef = useRef({
    playbackDuration480: 0,
    playbackDuration720: 0,
    playbackDuration1080: 0,
    stallCount: 0,
    stallDurationTotal: 0,
    qualitySwitchCount: 0,
    abortedDownloadCount: 0,
  });
  //-------------
  const segmentInfoArrayRef = useRef([]);
  const staticsArrayRef = useRef([]);

  const abortUponSeekRef = useRef(new AbortController());

  const isInternalQualityChangeRef = useRef(false);

  const manifestBandwidthArrRef = useRef([]);

  useEffect(() => {
    startupDelayStartRef.current = performance.now();
    async function proccessManifestFile() {
      const manifestFile = await (await fetch(`${API_BASE}/segments/manifest.mpd`)).text();
      const parser = new DOMParser();
      const manifestXml = parser.parseFromString(manifestFile, "application/xml");
      const MPDElement = manifestXml.documentElement;
      const durationTotal = xmlVideoDurationParser(
        MPDElement.getAttribute("mediaPresentationDuration"),
      );
      const timescaleElement =
        MPDElement.querySelector("SegmentTemplate").getAttribute("timescale");

      const segmentTimelineElement =
        MPDElement.querySelector("SegmentTimeline").querySelectorAll("S");

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
      setManifestData({ durationTotal, timeLine });

      const allReps = MPDElement.querySelectorAll("Representation");
      for (let i = 0; i < 3; i++) {
        manifestBandwidthArrRef.current[i] = Number(allReps[i].getAttribute("bandwidth"));
      }
    }
    proccessManifestFile();
  }, []);

  useEffect(() => {
    if (!manifestData) return;
    const mediaSource = new MediaSource();
    mediaSourceRef.current = mediaSource;
    const url = URL.createObjectURL(mediaSource);
    videoRef.current.src = url;
    mediaSource.addEventListener("sourceopen", async () => {
      if (mediaSource !== mediaSourceRef.current) return;
      //---------------------------------------------
      const sourceBuffer = mediaSource.addSourceBuffer('video/mp4; codecs="avc1.64001e"');
      segmentTimelineRef.current = manifestData.timeLine;
      mediaSource.duration = manifestData.durationTotal;
      sourceBufferRef.current = sourceBuffer;
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

  async function setInitialVideo(mediaSource = mediaSourceRef.current) {
    const playerId = playerIdRef.current;
    const qualityId = qualityIdRef.current;
    const sourceBuffer = sourceBufferRef.current;
    const response = await (
      await fetch(`${API_BASE}/segments/init_${qualityNumberRef.current}.mp4`)
    ).arrayBuffer();

    if (mediaSourceRef.current.readyState !== "open" || playerId !== playerIdRef.current) return;

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
    if (mediaSource === mediaSourceRef.current) {
      sourceBuffer.appendBuffer(response);
      console.log("INIT APPEND");
    }
  }
  async function setPlaybackQuality(qualityNumber) {
    if (qualityNumberRef.current === 0) {
      qualityOfExprienceRef.current.playbackDuration480 +=
        partialQualityTimeRef.current +
        (videoRef.current.currentTime - qualityStartTimeRef.current);
    } else if (qualityNumberRef.current === 1) {
      qualityOfExprienceRef.current.playbackDuration720 +=
        partialQualityTimeRef.current +
        (videoRef.current.currentTime - qualityStartTimeRef.current);
    } else {
      qualityOfExprienceRef.current.playbackDuration1080 +=
        partialQualityTimeRef.current +
        (videoRef.current.currentTime - qualityStartTimeRef.current);
    }
    partialQualityTimeRef.current = 0;
    qualityStartTimeRef.current = videoRef.current.currentTime;

    if (qualityNumber > qualityNumberRef.current) swtichDirRef.current.push("upgrade");
    else swtichDirRef.current.push("downgrade");

    qualityOfExprienceRef.current.qualitySwitchCount++;
    if (!isInternalQualityChangeRef.current) qualityIdRef.current++;
    isInternalQualityChangeRef.current = false;
    qualityNumberRef.current = qualityNumber;
    setQualityText(qualityNumber == 0 ? 480 : qualityNumber == 1 ? 720 : 1080);
    await setInitialVideo();
    preservePauseState();
    console.log("New Quality Set!");
    console.log("Quality Switch COUNT: ", qualityOfExprienceRef.current.qualitySwitchCount);
    console.log("DIRECTION OF UPDATES: ", swtichDirRef.current);

    console.log("Time Spent on 480p: ", qualityOfExprienceRef.current.playbackDuration480);
    console.log("Time Spent on 720p: ", qualityOfExprienceRef.current.playbackDuration720);
    console.log("Time Spent on 1080p: ", qualityOfExprienceRef.current.playbackDuration1080);
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

    if (bufferInSeconds !== undefined) return bufferInSeconds - currentTimeNew;
    return 0;
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
      bufferLeft < 30 &&
      nextSegmentRef.current <= segmentTimelineRef.current.length &&
      !sourceBuffer.updating
    ) {
      if (playerId !== playerIdRef.current) return;
      console.log("Downloading New Segment...");

      if (!(await fetchAndAppendSegment())) {
        isBufferingRef.current = false;
        return;
      }
      nextSegmentRef.current++;
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

  function decidingDownloadTimeout(segmentSize) {
    if (segmentInfoArrayRef.current.length > 0) {
      const ETA = Math.max(
        (segmentSize * 8) /
          (segmentInfoArrayRef.current[segmentInfoArrayRef.current.length - 1].throughput_speed *
            1000000),
        (segmentSize * 8) /
          (staticsArrayRef.current[staticsArrayRef.current.length - 1].average_throughput *
            1000000),
      );

      console.log("TOA: ", ETA);

      const bufferLeft = getBufferLeft();

      console.log("TIMEOUT AMOUNT: ", ETA * 1.3 + bufferLeft * 0.2);

      return (ETA * 1.3 + bufferLeft * 0.2) * 1000;
    }
    return 10000;
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
    let data = null;
    let qualityNumber = qualityNumberRef.current;
    const seekSignal = abortUponSeekRef.current.signal;

    while (qualityNumber >= 0) {
      const sizeOfSegment =
        (manifestData.timeLine[segmentNumber - 1].duration *
          manifestBandwidthArrRef.current[qualityNumber]) /
        8;
      console.log("Downloading:", {
        segment: paddedSegmentNumber,
        quality: qualityNumber,
      });
      const abortUponTimeout = new AbortController();
      const abortUponTimeoutTimer = setTimeout(() => {
        abortUponTimeout.abort();
      }, decidingDownloadTimeout(sizeOfSegment));
      try {
        if (qualityNumber === 0) clearTimeout(abortUponTimeoutTimer);
        const response = await fetch(
          `${API_BASE}/segments/seg_${qualityNumber}_${paddedSegmentNumber}.m4s`,
          { signal: AbortSignal.any([abortUponTimeout.signal, seekSignal]) },
        );
        data = await response.arrayBuffer();
        console.log("Download successful:", {
          segment: paddedSegmentNumber,
          quality: qualityNumber,
        });
        break;
      } catch (error) {
        if (error.name === "AbortError") {
          console.log("Download aborted:", {
            segment: paddedSegmentNumber,
            quality: qualityNumber,
          });
          if (
            seekId !== seekIdRef.current ||
            playerId !== playerIdRef.current ||
            seekSignal.aborted
          )
            return false;
          qualityOfExprienceRef.current.abortedDownloadCount++;
          if (qualityNumber > 0) qualityNumber--;
        } else {
          console.error("Download Failed: ", error);
          return false;
        }
      } finally {
        if (qualityNumber !== 0) clearTimeout(abortUponTimeoutTimer);
      }
    }
    if (qualityNumberRef.current !== qualityNumber) {
      isInternalQualityChangeRef.current = true;
      await setPlaybackQuality(qualityNumber);
    }

    segmentInfo.size = data.byteLength;
    const end = performance.now();
    segmentInfo.downloadTime = end - start;
    segmentInfo.throughput_speed =
      (segmentInfo.size * 8) / 1000000 / (segmentInfo.downloadTime / 1000);
    if (seekId != seekIdRef.current || playerId !== playerIdRef.current) return false;
    if (sourceBufferRef.current.updating) {
      if (seekId != seekIdRef.current || playerId !== playerIdRef.current) return;
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
    console.log("SEGMENT APPEND");

    await new Promise((resolve) => {
      sourceBufferRef.current.addEventListener("updateend", resolve, {
        once: true,
      });
    });
    segmentInfo.buffer_after_download = getBufferLeft();
    //---------------------------------------------------

    segmentInfoArrayRef.current.push(segmentInfo);

    const segmentInfoArray = segmentInfoArrayRef.current;
    let hasMinimum = false;

    for (let i = 0; i < segmentInfoArray.length; i++) {
      statics.average_throughput += segmentInfoArray[i].throughput_speed;
      if (segmentInfoArray[i].buffer_before_download != undefined)
        statics.average_buffer_before_download += segmentInfoArray[i].buffer_before_download;
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
    staticsArrayRef.current.push(statics);

    //Average of Last 5 segments
    const segmentInfoLastFive = segmentInfoArray.slice(-5);
    let segmentInfoLastFiveAverage = 0;
    for (let i = 0; i < segmentInfoLastFive.length; i++) {
      segmentInfoLastFiveAverage += segmentInfoLastFive[i].throughput_speed;
    }
    segmentInfoLastFiveAverage /= segmentInfoLastFive.length;
    console.log("Last 5 Average: ", segmentInfoLastFiveAverage);

    // Estimating and setting the quality for the next segment
    const estimatedThroughput = Math.min(segmentInfo.throughput_speed, segmentInfoLastFiveAverage);
    let selectedQuality = chooseQuality(estimatedThroughput);

    const bufferLeft = getBufferLeft() ?? 0;

    if (bufferLeft < 4) selectedQuality = 0;

    console.log("SELECTED QUALITY: ", selectedQuality);
    if (selectedQuality !== qualityNumberRef.current) await setPlaybackQuality(selectedQuality);

    return true;
    //---------------------
  }

  function chooseQuality(estimatedThroughput) {
    const throughput = estimatedThroughput * 0.8;
    if (throughput >= 5) return 2;
    else if (throughput >= 2.5) return 1;
    else return 0;
  }

  //------------------------------------------------------
  function xmlVideoDurationParser(duration) {
    const tPos = duration.indexOf("T");
    const hPos = duration.indexOf("H");
    const mPos = duration.indexOf("M");
    const sPos = duration.indexOf("S");

    const hour = hPos != -1 ? tPos + 1 : -1;
    const minute = mPos != -1 ? (hPos != -1 ? hPos + 1 : tPos + 1) : -1;
    const second = sPos != -1 ? (mPos != -1 ? mPos + 1 : hPos != -1 ? hPos + 1 : tPos + 1) : -1;

    const hourValue = hour != -1 ? Number(duration.substring(hour, hPos)) : 0;
    const minuteValue = minute != -1 ? Number(duration.substring(minute, mPos)) : 0;
    const secondValue = second != -1 ? Number(duration.substring(second, sPos)) : 0;

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
          onTimeUpdate={() => {
            checkBuffer();
            if (!videoRef.current.seeking) qualityEndTimeRef.current = videoRef.current.currentTime;
          }}
          onSeeking={() => {
            if (hasPlaybackStartedRef.current) {
              console.log("SEEKED AND PREVIOUS DOWNLOADS ABORTED");

              abortUponSeekRef.current.abort();
              abortUponSeekRef.current = new AbortController();
            }

            clearTimeout(seekTimeoutRef.current);
            seekTimeoutRef.current = setTimeout(() => {
              findSoughtPosAndAppend();
            }, 150);
            if (qualityIntervalActiveRef.current) {
              partialQualityTimeRef.current += Math.max(
                0,
                qualityEndTimeRef.current - qualityStartTimeRef.current,
              );
              qualityIntervalActiveRef.current = false;
            }
          }}
          onSeeked={() => {
            qualityStartTimeRef.current = videoRef.current.currentTime;
            qualityEndTimeRef.current = videoRef.current.currentTime;
            qualityIntervalActiveRef.current = true;
          }}
          onWaiting={() => {
            if (
              !videoRef.current.paused &&
              !isSeekingRef.current &&
              videoRef.current.currentTime < videoRef.current.duration &&
              hasPlaybackStartedRef.current &&
              !isCountingStallRef.current
            ) {
              qualityOfExprienceRef.current.stallCount++;
              stallTimerStartRef.current = performance.now();
              isCountingStallRef.current = true;
            }
          }}
          onPlaying={() => {
            if (!hasPlaybackStartedRef.current) {
              qualityStartTimeRef.current = videoRef.current.currentTime;
              startupDelayEndRef.current = performance.now();
              qualityIntervalActiveRef.current = true;
              startupDelayRef.current = startupDelayEndRef.current - startupDelayStartRef.current;
              console.log("Startup Delay: ", startupDelayRef.current);

              hasPlaybackStartedRef.current = true;
            }
            if (isCountingStallRef.current) {
              stallTimerEndRef.current = performance.now();
              isCountingStallRef.current = false;
              let stallDuration = stallTimerEndRef.current - stallTimerStartRef.current;

              qualityOfExprienceRef.current.stallDurationTotal += stallDuration;

              console.log("TOTAL DURATION: ", qualityOfExprienceRef.current.stallDurationTotal);

              console.log("TIMES: ", qualityOfExprienceRef.current.stallCount);
            }
          }}
          onEnded={() => {
            if (qualityNumberRef.current === 0)
              qualityOfExprienceRef.current.playbackDuration480 +=
                videoRef.current.currentTime - qualityStartTimeRef.current;
            else if (qualityNumberRef.current === 1)
              qualityOfExprienceRef.current.playbackDuration720 +=
                videoRef.current.currentTime - qualityStartTimeRef.current;
            else
              qualityOfExprienceRef.current.playbackDuration1080 +=
                videoRef.current.currentTime - qualityStartTimeRef.current;

            qualityStartTimeRef.current = videoRef.current.currentTime;
          }}
        ></video>
        <div>
          <h1>ABR-Behaving Buttons:</h1>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 0) {
                setPlaybackQuality(0);
                qualityNumberRef.current = 0;
              }
            }}
          >
            480p
          </button>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 1) {
                setPlaybackQuality(1);
                qualityNumberRef.current = 1;
              }
            }}
          >
            720p
          </button>
          <button
            onClick={() => {
              if (qualityNumberRef.current != 2) {
                setPlaybackQuality(2);
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
