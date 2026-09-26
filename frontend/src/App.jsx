import { useState, useRef, useEffect } from "react";

import "./App.css";

function App() {
  const [source, setSource] = useState(720);
  const videoRef = useRef(null);
  const savedTime = useRef(0);
  const wasPaused = useRef(false);
  const sourceBufferRef = useRef(null);
  const nextSegmentRef = useRef(2);
  const segmentTimelineRef = useRef([]);
  const durationRef = useRef(0);
  const isSeekingRef = useRef(false);
  const isBufferingRef = useRef(false);
  const seekIdRef = useRef(0);
  const selectedQualityRef = useRef(720);

  function setPlaybackTime() {
    savedTime.current = videoRef.current.currentTime;
    wasPaused.current = videoRef.current.paused;
  }

  useEffect(() => {
    const mediaSource = new MediaSource();
    const url = URL.createObjectURL(mediaSource);
    videoRef.current.src = url;
    mediaSource.addEventListener("sourceopen", async () => {
      console.log("MediaSource Opened.");

      const sourceBuffer = mediaSource.addSourceBuffer(
        'video/mp4; codecs="avc1.4d401f"',
      );
      sourceBufferRef.current = sourceBuffer;
      const xmlInit = await fetch(
        "http://127.0.0.1:8080/segments/manifest.mpd",
      );
      const answer = await xmlInit.text();
      const parser = new DOMParser();
      const xml = parser.parseFromString(answer, "application/xml");
      const mpd = xml.documentElement;
      durationRef.current = mpd.getAttribute("mediaPresentationDuration");
      durationRef.current = xmlVideoDurationParser(durationRef.current);
      mediaSource.duration = durationRef.current;
      console.log(mpd);
      const segmentTimelineElements = mpd.querySelectorAll("SegmentTimeline S");
      const timescale = mpd
        .querySelector("SegmentTemplate")
        .getAttribute("timescale");
      for (const s of segmentTimelineElements) {
        segmentTimelineRef.current.push({
          segmentNumber: segmentTimelineRef.current.length + 1,
          duration: s.getAttribute("d") / timescale,
        });
        for (let i = 0; i < s.getAttribute("r"); i++) {
          segmentTimelineRef.current.push({
            segmentNumber:
              segmentTimelineRef.current[segmentTimelineRef.current.length - 1],
            duration: s.getAttribute("d") / timescale,
          });
        }
      }

      const response = await fetch(
        `http://127.0.0.1:8080/segments/${selectedQualityRef.current}/init.mp4`,
      );
      const initData = await response.arrayBuffer();
      sourceBuffer.appendBuffer(initData);

      buffering();

      sourceBuffer.addEventListener("updateend", () => checkBuffer());
    });
  }, []);

  async function checkBuffer() {
    if (isBufferingRef.current) return;
    if (isSeekingRef.current) return;
    isBufferingRef.current = true;
    const sourceBuffer = sourceBufferRef.current;
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

    if (bufferInSeconds) bufferLeft = bufferInSeconds - currentTimeNew;

    console.log(currentTimeNew);
    console.log(bufferInSeconds);
    console.log(bufferLeft);

    if (bufferLeft < 40) {
      if (nextSegmentRef.current <= 222) {
        let segmentNumber = String(nextSegmentRef.current).padStart(5, "0");
        const response = await fetch(
          `http://127.0.0.1:8080/segments/${selectedQualityRef.current}/seg_${segmentNumber}.m4s`,
        );
        const Data = await response.arrayBuffer();
        if (!sourceBuffer.updating) {
          sourceBuffer.appendBuffer(Data);
          nextSegmentRef.current++;
          console.log("NEW BUFFER LOADED IMMEDIETLY");
        }
      }
    }
    isBufferingRef.current = false;
  }

  async function buffering() {
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
        const response = await fetch(
          `http://127.0.0.1:8080/segments/${selectedQualityRef.current}/seg_${String(i + 1).padStart(5, "0")}.m4s`,
        );
        if (seekId !== seekIdRef.current) return;
        const Data = await response.arrayBuffer();
        if (seekId !== seekIdRef.current) return;
        if (sourceBufferRef.current.updating) {
          await new Promise((resolve) => {
            sourceBufferRef.current.addEventListener("updateend", resolve, {
              once: true,
            });
          });
        }
        if (seekId !== seekIdRef.current) return;
        sourceBufferRef.current.appendBuffer(Data);

        nextSegmentRef.current = i + 2;
        isSeekingRef.current = false;
        break;
      }
      startTime = endTime;
    }
    isSeekingRef.current = false;
  }

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
        <h1>Video Player</h1> <h2>Current Quality: {source}</h2>
        <video
          ref={videoRef}
          controls
          onLoadedMetadata={() => {
            videoRef.current.currentTime = savedTime.current;
            // if (!wasPaused.current) {
            //   videoRef.current.play();
            // }
          }}
          onTimeUpdate={() => checkBuffer()}
          onSeeking={() => buffering()}
        ></video>
        <button
          onClick={() => {
            setPlaybackTime();
            setSource(480);
          }}
        >
          480p
        </button>
        <button
          onClick={() => {
            setPlaybackTime();
            setSource(720);
          }}
        >
          720p
        </button>
        <button
          onClick={() => {
            setPlaybackTime();
            setSource(1080);
          }}
        >
          1080p
        </button>
      </div>
    </>
  );
}
export default App;
