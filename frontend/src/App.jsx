import { useState, useRef, useEffect } from "react";
import { CompetingPlayer } from "./CompetingPlayer.jsx";
import { QoEResult, logQoEToConsole } from "./QoEResult.jsx";

import "./App.css";


const BATCH_KEY = "abr-batch-from-parameters-v4";


function loadBatchSession(experimentConfig) {
  const { repeatRuns, resultLabel: label, trafficSeed: seed } = experimentConfig.runSettings;
  if (repeatRuns === 1) return null;
  if (!experimentConfig.workloads.customAbr) {
    throw new Error("Repeated QoE batch requires customAbr=true");
  }
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(label) || !/^[A-Za-z0-9_-]{1,64}$/.test(seed)) {
    throw new Error("Batch label and seed must contain only letters, digits, _ or -");
  }
  // Changing an actual ABR parameter automatically starts a fresh multi-run batch.
  const key = JSON.stringify({ label, seed, parameters: experimentConfig.abrParameters, experimentConfig });
  try {
    const saved = JSON.parse(sessionStorage.getItem(BATCH_KEY) || "null");
    if (saved?.key === key && /^[A-Za-z0-9_-]{1,64}$/.test(saved.id) &&
        Number.isInteger(saved.completed) && saved.completed >= 0 &&
        saved.completed <= repeatRuns) return saved;
  } catch (error) { console.warn("Cannot restore batch session:", error); }
  // The browser sends a stable unique batch key. The backend assigns the
  // human-readable numeric folder prefix from tester/experiments on disk.
  const now = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  const dateTime = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}` +
    `_${pad(now.getHours())}-${pad(now.getMinutes())}-${pad(now.getSeconds())}`;
  const uniqueSuffix = crypto.randomUUID().replaceAll("-", "").slice(0, 8);
  const state = {
    id: `${dateTime}_${uniqueSuffix}`,
    key, label, seed, completed: 0, averageSaved: false,
  };
  sessionStorage.setItem(BATCH_KEY, JSON.stringify(state));
  return state;
}

async function waitForTesterCleanup(apiBase, runId) {
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    const response = await fetch(`${apiBase}/experiment/status`, {
      cache: "no-store", signal: AbortSignal.timeout(5000),
    });
    if (!response.ok) throw new Error(`Status HTTP ${response.status}`);
    const status = await response.json();
    if (!status.running && status.lastFinishedRunId === runId) {
      if (!status.lastTesterExitSuccessful) {
        throw new Error(`Tester cleanup failed (exit code ${status.lastTesterExitCode})`);
      }
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw new Error("Timed out waiting for tester cleanup");
}

function App() {
  const API_BASE = import.meta.env.DEV ? `http://${window.location.hostname}:8080` : "";
  const [experimentConfig, setExperimentConfig] = useState(null);
  // Tuning comes from the same immutable backend snapshot as workload switches.
  const ABR_PARAMETERS = experimentConfig?.abrParameters;
  const [batchSession, setBatchSession] = useState(null);
  const [batchMessage, setBatchMessage] = useState("");
  const [reportedQoe, setReportedQoe] = useState(null);
  const [reportedQoeIsAverage, setReportedQoeIsAverage] = useState(false);
  const noAbrRunStartedRef = useRef(false);
  const noAbrAutoStopTimerRef = useRef(null);
  const activeBackendRunIdRef = useRef(null);
  const batchTotal = experimentConfig?.runSettings.repeatRuns ?? 1;
  const batchFinished = batchSession !== null && batchSession.completed >= batchTotal;

  // TODO: Refactor related refs into playerRef / qoeRef / measurementRef

  const [qualityText, setQualityText] = useState(720);
  const [manualQuality, setManualQuality] = useState(1);
  const [manifestData, setManifestData] = useState(null);

  const qualityNumberRef = useRef(1);
  const savedTime = useRef(0);
  const wasPaused = useRef(true);
  const videoRef = useRef(null);
  const videoSourceBufferRef = useRef(null);
  const audioSourceBufferRef = useRef(null);
  const nextSegmentRef = useRef(1);
  const segmentTimelineRef = useRef([]);
  const isSeekingRef = useRef(false);
  const isBufferingRef = useRef(false);
  const seekIdRef = useRef(0);
  const seekTimeoutRef = useRef(null);
  const playerIdRef = useRef(1);
  const qualityIdRef = useRef(1);
  const mediaSourceRef = useRef(null);
  const hasPlaybackStartedRef = useRef(false);
  //-----------------------------------
  const isCountingStallRef = useRef(false);
  const stallTimerStartRef = useRef(0);
  const stallTimerEndRef = useRef(0);

  const experimentStartRef = useRef(0);
  const startupDelayEndRef = useRef(0);

  const qualityStartTimeRef = useRef(0);
  const qualityEndTimeRef = useRef(0);
  const partialQualityTimeRef = useRef(0);
  const qualityIntervalActiveRef = useRef(false);

  const isReportingQOERef = useRef(false);

  const qualityOfExprienceRef = useRef({
    playbackDurationTotal480Sec: 0,
    playbackDurationTotal720Sec: 0,
    playbackDurationTotal1080Sec: 0,
    startupDelayMs: 0,
    stallCountTotal: 0,
    stallDurationTotalMs: 0,
    qualitySwitchCount: 0,
    abortedDownloadCount: 0,
    qualitySwitchHistoryArr: [],
    abortHistoryArr: [],
  });
  //-------------
  const segmentInfoArrayRef = useRef([]);
  const staticsArrayRef = useRef([]);

  const abortUponSeekRef = useRef(new AbortController());

  const isInternalQualityChangeRef = useRef(false);

  const manifestBandwidthArrRef = useRef([]);

  const isRetryingDownloadRef = useRef(false);

  const segmentAfterSeekRef = useRef(0);

  const InitFilesArrRef = useRef([null, null, null, null]);
  const experimentGoSentRef = useRef(false);
  // Captured only in memory during a run; posted with the existing QoE result.
  // Epoch timestamps align with tester/network records; tMs is monotonic relative to GO.
  const playerTelemetryRef = useRef(null);
  function recordPlayerTelemetry(type, details = {}) {
    const trace = playerTelemetryRef.current;
    if (!trace || !experimentGoSentRef.current) return;
    if (trace.events.length >= 1500) {
      trace.droppedEvents++;
      return;
    }
    trace.events.push({
      type,
      tMs: Math.round((performance.now() - experimentStartRef.current) * 1000) / 1000,
      epochMs: Date.now(),
      videoTimeSec: videoRef.current?.currentTime ?? null,
      ...details,
    });
  }

  // Mount dash.js only after the common GO signal, not during preparation.
  const [isExperimentReady, setIsExperimentReady] = useState(false);

  // Load the single backend-validated config before starting either player.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const response = await fetch(`${API_BASE}/experiment/config`, { cache: "no-store" });
        const config = await response.json();
        if (!response.ok) throw new Error(config.error || `HTTP ${response.status}`);
        if (cancelled) return;
        setBatchSession(loadBatchSession(config));
        setExperimentConfig(config);
      } catch (error) {
        if (!cancelled) setBatchMessage(`Experiment configuration error: ${error.message}`);
      }
    })();
    return () => { cancelled = true; };
  }, [API_BASE]);

  // The other workloads can run without the custom ABR player. In this mode
  // there is deliberately no fabricated QoE result or multi-run average.
  useEffect(() => {
    if (!experimentConfig || experimentConfig.workloads.customAbr || batchFinished ||
        noAbrRunStartedRef.current) return;
    noAbrRunStartedRef.current = true;
    (async () => {
      if (!(await experimentModify("start"))) {
        setBatchMessage("Experiment could not be prepared. Check backend logs/config.");
        return;
      }
      if (!(await experimentModify("go"))) {
        await experimentModify("stop");
        setBatchMessage("Experiment failed to start other workloads.");
        return;
      }
      setIsExperimentReady(true);
      setBatchMessage("Custom ABR disabled: running enabled workloads for 120 seconds (no custom QoE output).");
      const runId = activeBackendRunIdRef.current;
      noAbrAutoStopTimerRef.current = setTimeout(async () => {
        setIsExperimentReady(false);
        if (!(await experimentModify("stop"))) {
          setBatchMessage("Timed run ended, but tester stop failed. Check backend status.");
          return;
        }
        try {
          await waitForTesterCleanup(API_BASE, runId);
          setBatchMessage("Timed run complete; all enabled workloads stopped. No custom-player QoE recorded.");
        } catch (error) {
          setBatchMessage(`Experiment cleanup failed: ${error.message}`);
        }
      }, 120000);
    })();
  }, [experimentConfig, batchFinished]);

  useEffect(() => {
    if (batchFinished || !experimentConfig?.workloads.customAbr) return;
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
      for (let i = 0; i < 4; i++) {
        manifestBandwidthArrRef.current[i] = Number(allReps[i].getAttribute("bandwidth"));
      }
    }
    proccessManifestFile();
  }, [experimentConfig, batchFinished]);

  useEffect(() => {
    if (!manifestData || batchFinished || !experimentConfig?.workloads.customAbr) return;
    const mediaSource = new MediaSource();
    mediaSourceRef.current = mediaSource;
    const url = URL.createObjectURL(mediaSource);
    videoRef.current.src = url;
    mediaSource.addEventListener("sourceopen", async () => {
      if (mediaSource !== mediaSourceRef.current) return;
      experimentGoSentRef.current = false;
      setIsExperimentReady(false);
      //---------------------------------------------
      const videoSourceBuffer = mediaSource.addSourceBuffer('video/mp4; codecs="avc1.64001e"');
      const audioSourceBuffer = mediaSource.addSourceBuffer('audio/mp4; codecs="mp4a.40.2"');
      segmentTimelineRef.current = manifestData.timeLine;
      mediaSource.duration = manifestData.durationTotal;
      videoSourceBufferRef.current = videoSourceBuffer;
      audioSourceBufferRef.current = audioSourceBuffer;
      await storeInitialFiles();
      if (mediaSource !== mediaSourceRef.current) return;
      const experimentReady = await experimentModify("start");
      if (!experimentReady) return;
      if (mediaSource !== mediaSourceRef.current) return;

      await setInitAudio();
      await setInitVideo(mediaSource);
      if (mediaSource !== mediaSourceRef.current) return;

      // Release browsing only after initialization, before any measured segments.
      // This is approximate, not millisecond-accurate synchronization with dash.js.
      const released = await experimentModify("go");
      if (!released) {
        await experimentModify("stop");
        return;
      }
      if (mediaSource !== mediaSourceRef.current) return;
      experimentStartRef.current = performance.now();
      playerTelemetryRef.current = {
        schemaVersion: 1,
        source: "custom-abr",
        clock: "performance.now() relative to frontend GO, plus Date.now() wall clock",
        frontendGoEpochMs: Date.now(),
        events: [],
        droppedEvents: 0,
      };
      experimentGoSentRef.current = true;
      recordPlayerTelemetry("go_acknowledged", { backendRunId: activeBackendRunIdRef.current });

      videoSourceBuffer.addEventListener("updateend", () => checkBuffer());
      setIsExperimentReady(true); // Mounts the existing CompetingPlayer after GO.
      seekAndAppendSegment();
    });
  }, [manualQuality, manifestData, experimentConfig, batchFinished]);

  async function experimentModify(command) {
    try {
      const response = await fetch(`${API_BASE}/experiment/${command}`, {
        method: "POST",
        ...(command === "start" ? {
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ config: experimentConfig }),
        } : {}),
      });
      const result = await response.json();
      if (command === "start") {
        const success = response.status === 200 && result.status === "ready" &&
          Number.isSafeInteger(result.runId) && result.runId > 0 &&
          JSON.stringify(result.config) === JSON.stringify(experimentConfig);
        if (success) activeBackendRunIdRef.current = result.runId;
        if (!success) {
          console.error("Experiment failed to start:", result);
          setBatchMessage(result.error || "Backend configuration mismatch. Reload the page.");
        }
        return success;
      }
      if (command === "go") {
        const success = response.status === 200 &&
          (result.status === "started" || result.status === "already_started");
        if (!success) {
          console.error("Experiment failed to start browsing:", result);
        }
        return success;
      }
      if (command === "stop") {
        const success = response.status === 202;
        if (!success) {
          console.error("Experiment failed to stop:", result);
        } else {
          experimentGoSentRef.current = false;
          setIsExperimentReady(false);
        }
        return success;
      }
      console.error("Unknown experiment command:", command);
      return false;
    } catch (error) {
      console.error("Experiment request failed:", error);
      return false;
    }
  }

  async function storeInitialFiles() {
    for (let i = 0; i < InitFilesArrRef.current.length; i++) {
      const response = await fetch(`${API_BASE}/segments/init_${i}.mp4`);
      const result = await response.arrayBuffer();
      InitFilesArrRef.current[i] = result;
    }
  }

  async function setInitAudio() {
    audioSourceBufferRef.current.appendBuffer(InitFilesArrRef.current[3]);
  }

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

  async function setInitVideo(
    mediaSource = mediaSourceRef.current,
    qualityNumber = qualityNumberRef.current,
  ) {
    const playerId = playerIdRef.current;
    const qualityId = qualityIdRef.current;
    const videoSourceBuffer = videoSourceBufferRef.current;
    if (mediaSourceRef.current.readyState !== "open" || playerId !== playerIdRef.current) return;

    if (videoSourceBuffer.updating) {
      await new Promise((resolve) => {
        videoSourceBuffer.addEventListener("updateend", resolve, {
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
      videoSourceBuffer.appendBuffer(InitFilesArrRef.current[qualityNumber]);
    }
  }
  async function setPlaybackQualityAndLog(qualityNumber, reason = "no reason given") {
    if (qualityNumber === qualityNumberRef.current) return;
    const qualitySwitchHistory = {
      from: qualityNumberRef.current,
      to: qualityNumber,
      videoTimeSec: videoRef.current.currentTime,
      reason,
    };
    qualityOfExprienceRef.current.qualitySwitchHistoryArr.push(qualitySwitchHistory);
    recordPlayerTelemetry("quality_switch", qualitySwitchHistory);
    if (!isInternalQualityChangeRef.current) qualityIdRef.current++;
    isInternalQualityChangeRef.current = false;
    qualityOfExprienceRef.current.qualitySwitchCount++;
    qualityNumberRef.current = qualityNumber;

    await setInitVideo(undefined, qualityNumber);
    preservePauseState();
  }

  function calculateQualityDuration() {
    let totalDuration = 0;
    for (let i = 0; i < segmentInfoArrayRef.current.length; i++) {
      if (totalDuration < videoRef.current.currentTime) {
        let segmentDuration =
          manifestData.timeLine[segmentInfoArrayRef.current[i].segmentNumber - 1].duration;
        totalDuration += segmentDuration;
        if (totalDuration > videoRef.current.currentTime) {
          let excess = totalDuration - videoRef.current.currentTime;
          segmentDuration -= excess;
        }
        if (segmentInfoArrayRef.current[i].quality === 0) {
          qualityOfExprienceRef.current.playbackDurationTotal480Sec += segmentDuration;
        } else if (segmentInfoArrayRef.current[i].quality === 1) {
          qualityOfExprienceRef.current.playbackDurationTotal720Sec += segmentDuration;
        } else if (segmentInfoArrayRef.current[i].quality === 2) {
          qualityOfExprienceRef.current.playbackDurationTotal1080Sec += segmentDuration;
        }
      }
    }
  }

  function findSoughtSegment() {
    let totalDuration = 0;
    const currentTime = videoRef.current.currentTime;
    for (let i = 0; i < manifestData.timeLine.length; i++) {
      const segmentDuration = manifestData.timeLine[i].duration;
      totalDuration += segmentDuration;
      if (totalDuration > currentTime && totalDuration - segmentDuration <= currentTime) {
        const soughtSegmentNumber = manifestData.timeLine[i].segmentNumber;
        return soughtSegmentNumber;
      }
    }
    return "";
  }

  function settingQualityText() {
    const soughtSegmentNumber = findSoughtSegment();
    for (let i = 0; i < segmentInfoArrayRef.current.length; i++) {
      if (segmentInfoArrayRef.current[i].segmentNumber === soughtSegmentNumber) {
        //found the same segment in the segmentInfoArray
        let qualityNumber = segmentInfoArrayRef.current[i].quality;
        qualityNumber = qualityNumber == 0 ? 480 : qualityNumber == 1 ? 720 : 1080;
        if (qualityText !== qualityNumber) setQualityText(qualityNumber);
        return;
      }
    }
  }
  function getBufferLeft() {
    const currentTimeNew = videoRef.current.currentTime;
    let bufferInSeconds;

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

  async function seekAndAppendSegment() {
    seekIdRef.current++;
    const seekId = seekIdRef.current;
    isBufferingRef.current = false;
    isSeekingRef.current = true;
    const soughtSegmentNumber = findSoughtSegment();
    if (seekId !== seekIdRef.current) return;
    if (soughtSegmentNumber) nextSegmentRef.current = soughtSegmentNumber;
    else {
      isSeekingRef.current = false;
      return;
    }
    isSeekingRef.current = false;
    checkBuffer();
  }

  function decidingDownloadTimeout(segmentSize, etaRequested) {
    if (segmentInfoArrayRef.current.length > 0) {
      const ETA = Math.max(
        (segmentSize * 8) /
          (segmentInfoArrayRef.current[segmentInfoArrayRef.current.length - 1].throughput_speed *
            1000000),
        (segmentSize * 8) /
          (staticsArrayRef.current[staticsArrayRef.current.length - 1].average_throughput *
            1000000),
      );

      if (etaRequested === undefined || etaRequested === false) {

        const bufferLeft = getBufferLeft();

        return (ETA * ABR_PARAMETERS.timeoutEtaFactor +
          bufferLeft * ABR_PARAMETERS.timeoutBufferFactor) * 1000;
      } else return ETA;
    }
    return ABR_PARAMETERS.initialDownloadTimeoutMs;
  }

  async function checkBuffer() {
    if (!experimentGoSentRef.current) return false;
    let qualityNumber = qualityNumberRef.current;
    while (getBufferLeft() < ABR_PARAMETERS.bufferTargetSec) {
      if (isBufferingRef.current) return false;
      if (isSeekingRef.current) return false;
      const seekId = seekIdRef.current;
      const playerId = playerIdRef.current;
      if (seekId !== seekIdRef.current) return false;
      const nextSegment = nextSegmentRef.current;
      const videoSourceBuffer = videoSourceBufferRef.current;
      isBufferingRef.current = true;
      if (
        seekId !== seekIdRef.current ||
        nextSegment > segmentTimelineRef.current.length ||
        nextSegment !== nextSegmentRef.current ||
        videoSourceBuffer.updating ||
        playerId !== playerIdRef.current
      ) {
        isBufferingRef.current = false;
        return false;
      }

      let result = await fetchAndAppendSegment(undefined, undefined, qualityNumber);
      if (result === "AbortError") {
        if (
          seekId !== seekIdRef.current ||
          nextSegment > segmentTimelineRef.current.length ||
          nextSegment !== nextSegmentRef.current ||
          playerId !== playerIdRef.current
        ) {
          return false;
        }
        qualityNumber > 0 ? qualityNumber-- : qualityNumber;
        if (seekId === seekIdRef.current) isBufferingRef.current = false;
        continue;
      } else if (result === "TypeError" || result === "DownloadFailed") {
        await new Promise((resolve) => {
          setTimeout(() => {
            resolve();
          }, ABR_PARAMETERS.downloadRetryDelayMs);
        });
        if (seekId === seekIdRef.current) isBufferingRef.current = false;
        continue;
      } else if (result === false) {
        if (seekId === seekIdRef.current) isBufferingRef.current = false;
        return;
      }
      if (
        seekId !== seekIdRef.current ||
        nextSegment > segmentTimelineRef.current.length ||
        nextSegment !== nextSegmentRef.current ||
        playerId !== playerIdRef.current
      ) {
        if (seekId === seekIdRef.current) isBufferingRef.current = false;
        return false;
      }
      if (result === true) {
        nextSegmentRef.current++;
        qualityNumber = qualityNumberRef.current;
      }
      if (seekId === seekIdRef.current) isBufferingRef.current = false;
      if (getBufferLeft() >= ABR_PARAMETERS.bufferTargetSec) return true;
      if (isRetryingDownloadRef.current) return false;
    }
  }

  async function fetchAndAppendSegment(
    segmentNumber = nextSegmentRef.current,
    seekId = seekIdRef.current,
    qualityNumber = qualityNumberRef.current,
  ) {
    const segmentInfo = {
      segmentNumber: null,
      quality: null,
      size: null,
      buffer_before_download: null,
      downloadTimeMs: null,
      throughput_speed: null,
      buffer_after_download: null,
      bufferBeforeRequest: null,
      requestStartMs: null,
    };
    const statics = {
      average_throughput: null,
      average_buffer_before_download: null,
      minimum_throughput: null,
      maximum_throughput: 0,
    };
    const failedSegmentInfo = {
      segmentNumber: null,
      quality: null,
      requstStartMs: null,
      abortTimeMs: null,
      elapsedMs: null,
      timeoutMs: null,
      bufferLeft: null,
      ETA: null,
      abortReason: null,
    };
    const qualityId = qualityIdRef.current;
    segmentInfo.segmentNumber = segmentNumber;
    const playerId = playerIdRef.current;
    let paddedSegmentNumber = String(segmentNumber).padStart(5, "0");
    segmentInfo.bufferBeforeRequest = getBufferLeft();
    if (playerId !== playerIdRef.current) return false;
    let dataVideo = null;
    const seekSignal = abortUponSeekRef.current.signal;
    let startFetch = null;
    let end = null;
    segmentInfo.buffer_before_download = getBufferLeft();

    const durationOfSegment = manifestData.timeLine[segmentNumber - 1].duration;
    const sizeOfSegment =
      (durationOfSegment *
        (manifestBandwidthArrRef.current[qualityNumber] + manifestBandwidthArrRef.current[3])) /
      8;
    const abortUponTimeout = new AbortController();
    const timeoutMs = decidingDownloadTimeout(sizeOfSegment);
    const abortUponTimeoutTimer = setTimeout(
      () => {
        abortUponTimeout.abort();
      },
      qualityNumber == 0 ? ABR_PARAMETERS.lowestQualityTimeoutMs : timeoutMs,
    );
    try {
      if (seekId !== seekIdRef.current) return false;
      startFetch = performance.now();
      segmentInfo.requestStartMs = startFetch - experimentStartRef.current;
      const response = await fetch(
        `${API_BASE}/segments/seg_${qualityNumber}_${paddedSegmentNumber}.m4s`,
        { signal: AbortSignal.any([abortUponTimeout.signal, seekSignal]) },
      );
      if (seekId !== seekIdRef.current) return false;
      dataVideo = await response.arrayBuffer();
    } catch (error) {
      const abortTimeMs = performance.now();
      failedSegmentInfo.segmentNumber = segmentNumber;
      failedSegmentInfo.quality = qualityNumber;
      failedSegmentInfo.requstStartMs = startFetch - experimentStartRef.current;
      failedSegmentInfo.abortTimeMs = abortTimeMs - experimentStartRef.current;
      failedSegmentInfo.elapsedMs = abortTimeMs - startFetch;
      failedSegmentInfo.timeoutMs = timeoutMs;
      failedSegmentInfo.bufferLeft = segmentInfo.buffer_before_download;
      failedSegmentInfo.ETA = decidingDownloadTimeout(sizeOfSegment, true);
      recordPlayerTelemetry("segment_fetch_failed", {
        segmentNumber, quality: qualityNumber,
        reason: error.name, elapsedMs: failedSegmentInfo.elapsedMs,
        timeoutMs, bufferBeforeSec: segmentInfo.buffer_before_download,
      });

      if (error.name === "AbortError") {
        if (seekId === seekIdRef.current && playerId === playerIdRef.current && !seekSignal.aborted)
          qualityOfExprienceRef.current.abortedDownloadCount++;
        failedSegmentInfo.abortReason = "AbortError";
        qualityOfExprienceRef.current.abortHistoryArr.push(failedSegmentInfo);
        return "AbortError";
      } else if (error.name === "TypeError") {
        failedSegmentInfo.abortReason = "TypeError";
        qualityOfExprienceRef.current.abortHistoryArr.push(failedSegmentInfo);
        return "TypeError";
      }
      console.error("Download Failed: ", error.name);

      failedSegmentInfo.abortReason = "DownloadFailed";
      qualityOfExprienceRef.current.abortHistoryArr.push(failedSegmentInfo);
      return "DownloadFailed";
    } finally {
      clearTimeout(abortUponTimeoutTimer);
    }
    if (seekId !== seekIdRef.current) return false;
    if (playerId !== playerIdRef.current) return false;
    const response = await fetch(`${API_BASE}/segments/seg_3_${paddedSegmentNumber}.m4s`);
    if (!response.ok) return "DownloadFailed";
    if (seekId !== seekIdRef.current) return false;
    if (playerId !== playerIdRef.current) return false;
    const dataAudio = await response.arrayBuffer();

    end = performance.now();

    if (seekId !== seekIdRef.current) return false;
    if (playerId !== playerIdRef.current) return false;
    if (qualityId !== qualityIdRef.current) return false;
    if (qualityNumber !== qualityNumberRef.current) {
      isInternalQualityChangeRef.current = true;
      await setPlaybackQualityAndLog(qualityNumber, "Abort");
    }
    segmentInfo.quality = qualityNumber;
    segmentInfo.size = dataVideo.byteLength + dataAudio.byteLength;
    segmentInfo.downloadTimeMs = end - startFetch;
    segmentInfo.throughput_speed =
      (segmentInfo.size * 8) / 1000000 / (segmentInfo.downloadTimeMs / 1000);
    if (seekId != seekIdRef.current || playerId !== playerIdRef.current) return false;
    if (videoSourceBufferRef.current.updating) {
      if (seekId != seekIdRef.current || playerId !== playerIdRef.current) return false;
      await new Promise((resolve) => {
        videoSourceBufferRef.current.addEventListener("updateend", resolve, {
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

    videoSourceBufferRef.current.appendBuffer(dataVideo);

    await new Promise((resolve) => {
      videoSourceBufferRef.current.addEventListener("updateend", resolve, {
        once: true,
      });
    });

    if (seekId != seekIdRef.current || playerId !== playerIdRef.current) return false;
    if (audioSourceBufferRef.current.updating) {
      await new Promise((resolve) => {
        audioSourceBufferRef.current.addEventListener("updateend", resolve, {
          once: true,
        });
      });
    }
    if (seekId != seekIdRef.current || playerId !== playerIdRef.current) return false;
    audioSourceBufferRef.current.appendBuffer(dataAudio);
    await new Promise((resolve) => {
      audioSourceBufferRef.current.addEventListener("updateend", resolve, {
        once: true,
      });
    });

    segmentInfo.buffer_after_download = getBufferLeft();
    //----
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

    // Average throughput of the configured most recent segments.
    const segmentInfoLastFive = segmentInfoArray.slice(-ABR_PARAMETERS.throughputAveragingSegments);
    let segmentInfoThroughputLastFiveAverage = 0;
    for (let i = 0; i < segmentInfoLastFive.length; i++) {
      segmentInfoThroughputLastFiveAverage += segmentInfoLastFive[i].throughput_speed;
    }
    segmentInfoThroughputLastFiveAverage /= segmentInfoLastFive.length;

    // Estimating and setting the quality for the next segment
    const estimatedThroughput = Math.min(
      // segmentInfo.throughput_speed,
      segmentInfoThroughputLastFiveAverage,
    );
    let selectedQuality = chooseQuality(estimatedThroughput);

    const bufferLeft = getBufferLeft() ?? 0;

    if (bufferLeft < ABR_PARAMETERS.lowBufferThresholdSec) selectedQuality = 0;
    recordPlayerTelemetry("segment_complete", {
      segmentNumber,
      downloadedQuality: qualityNumber,
      videoBytes: dataVideo.byteLength,
      audioBytes: dataAudio.byteLength,
      totalBytes: segmentInfo.size,
      requestStartMs: segmentInfo.requestStartMs,
      downloadMs: segmentInfo.downloadTimeMs,
      throughputMbps: segmentInfo.throughput_speed,
      smoothedThroughputMbps: segmentInfoThroughputLastFiveAverage,
      selectedEstimatorMbps: estimatedThroughput,
      bufferBeforeSec: segmentInfo.buffer_before_download,
      bufferAfterSec: segmentInfo.buffer_after_download,
      bufferAtDecisionSec: bufferLeft,
      timeoutMs,
      nextQuality: selectedQuality,
      currentQualityBeforeDecision: qualityNumberRef.current,
      smoothingWindow: ABR_PARAMETERS.throughputAveragingSegments,
      safetyFactor: ABR_PARAMETERS.qualitySafetyFactor,
    });
    if (selectedQuality !== qualityNumberRef.current)
      await setPlaybackQualityAndLog(selectedQuality, "ABR");

    return true;
    //---------------------
  }

  function chooseQuality(estimatedThroughput) {
    const throughput = estimatedThroughput * ABR_PARAMETERS.qualitySafetyFactor;

    for (let i = manifestBandwidthArrRef.current.length - 2; i >= 0; i--) {
      if (
        throughput >=
        (manifestBandwidthArrRef.current[i] + manifestBandwidthArrRef.current[3]) / 1000000
      )
        return i;
    }
    return 0;
  }

  //------------------------------------------------------
  async function finishAndRecordExperiment() {
    // Do not reload or stop before the result is successfully persisted.
    if (isCountingStallRef.current) {
      stallTimerEndRef.current = performance.now();
      isCountingStallRef.current = false;
      qualityOfExprienceRef.current.stallDurationTotalMs +=
        stallTimerEndRef.current - stallTimerStartRef.current;
    }
    calculateQualityDuration();
    videoRef.current?.pause();
    const qoe = structuredClone(qualityOfExprienceRef.current);
    recordPlayerTelemetry("qoe_finalized", {
      stallCount: qoe.stallCountTotal,
      stallDurationMs: qoe.stallDurationTotalMs,
      playback1080Sec: qoe.playbackDurationTotal1080Sec,
    });
    const telemetry = playerTelemetryRef.current ? structuredClone(playerTelemetryRef.current) : null;


    const runId = activeBackendRunIdRef.current;
    if (!runId) {
      setBatchMessage("Cannot save result: missing backend run ID. Auto-repeat stopped.");
      await experimentModify("stop");
      return;
    }
    const batch = batchSession === null ? null : {
      id: batchSession.id,
      label: batchSession.label,
      seed: batchSession.seed, // Captured in the backend config and browser start schedule.
      index: batchSession.completed + 1,
      total: batchTotal,
    };

    try {
      const response = await fetch(`${API_BASE}/experiment/result`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ runId, batch, parameters: ABR_PARAMETERS, qoe, telemetry }),
      });
      const data = await response.json();
      if (response.status !== 201 || !data.saved) {
        throw new Error(data.error || `Result save failed (HTTP ${response.status})`);
      }
      // Display a human-readable report while keeping the saved JSON machine-readable.
      setReportedQoeIsAverage(false);
      setReportedQoe(qoe);
      logQoEToConsole(qoe, `Run ${batch?.index ?? 1}`);
    } catch (error) {
      console.error("Result not saved; stopping without repeating:", error);
      setBatchMessage(`Save failed: ${error.message}. Automatic repetition stopped.`);
      await experimentModify("stop");
      return;
    }

    if (!(await experimentModify("stop"))) {
      setBatchMessage("Result saved, but stop request failed. Automatic repetition stopped.");
      return;
    }
    try {
      await waitForTesterCleanup(API_BASE, runId);
    } catch (error) {
      console.error(error);
      setBatchMessage(`Result saved, but cleanup failed: ${error.message}. Auto-repeat stopped.`);
      return;
    }

    // Normal single runs still stop after saving; only opt-in batches reload.
    if (!batchSession) {
      setBatchMessage("Result saved; experiment finished.");
      return;
    }
    const updated = { ...batchSession, completed: batchSession.completed + 1 };
    sessionStorage.setItem(BATCH_KEY, JSON.stringify(updated));
    setBatchSession(updated);
    if (updated.completed < batchTotal) {
      // Waited for tester cleanup above: reload starts the next fresh experiment.
      window.location.reload();
    } else {
      await saveBatchAverage(updated);
    }
  }

  async function saveBatchAverage(batch) {
    try {
      const response = await fetch(`${API_BASE}/experiment/average`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ batch: {
          id: batch.id, label: batch.label, seed: batch.seed,
          total: batchTotal,
        } }),
      });
      const data = await response.json();
      if (!response.ok || !data.saved) {
        throw new Error(data.error || `Average failed (HTTP ${response.status})`);
      }
      const updated = { ...batch, averageSaved: true };
      sessionStorage.setItem(BATCH_KEY, JSON.stringify(updated));
      setBatchSession(updated);
      setBatchMessage("");
      setReportedQoeIsAverage(true);
      setReportedQoe(data.average);
      logQoEToConsole(data.average, `Average of ${batchTotal} runs`, true);
    } catch (error) {
      console.error("Could not save average:", error);
      setBatchMessage(`Average not saved: ${error.message}`);
    }
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
      {experimentConfig && (
        <p>Workloads: custom ABR {experimentConfig.workloads.customAbr ? "ON" : "OFF"},
          dash.js {experimentConfig.workloads.dashJs ? "ON" : "OFF"},
          browsers {experimentConfig.workloads.browsers.enabled
            ? experimentConfig.workloads.browsers.count : 0} (shared limiter)</p>
      )}
      {batchSession && (
        <div>
          Repeated experiment: {batchSession.completed}/{batchTotal}
          {batchFinished && (batchSession.averageSaved ? " — complete" : " — average pending")}
          {batchMessage && <p>{batchMessage}</p>}
          {batchFinished && !batchSession.averageSaved && (
            <button onClick={() => saveBatchAverage(batchSession)}>Retry average</button>
          )}
        </div>
      )}
      {!batchSession && batchMessage && <p>{batchMessage}</p>}
      {reportedQoe && (
        <QoEResult
          qoe={reportedQoe}
          isAverage={reportedQoeIsAverage}
          title={reportedQoeIsAverage
            ? `Average of ${batchTotal} runs`
            : `Run ${batchSession ? batchSession.completed + 1 : 1}`}
          label={experimentConfig?.runSettings.resultLabel ?? ""}
        />
      )}
      {experimentConfig && !experimentConfig.workloads.customAbr && (
        <button onClick={async () => {
          clearTimeout(noAbrAutoStopTimerRef.current);
          noAbrAutoStopTimerRef.current = null;
          setIsExperimentReady(false);
          await experimentModify("stop");
        }}>Stop experiment</button>
      )}
      {experimentConfig?.workloads.customAbr && <div>
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
            settingQualityText();
          }}
          onSeeking={() => {
            if (hasPlaybackStartedRef.current) {
              clearTimeout(seekTimeoutRef.current);
              seekTimeoutRef.current = setTimeout(() => {

                abortUponSeekRef.current.abort();
                abortUponSeekRef.current = new AbortController();
                seekAndAppendSegment();
              }, 150);
              segmentAfterSeekRef.current = segmentInfoArrayRef.current.length;
            }
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
              qualityOfExprienceRef.current.stallCountTotal++;
              recordPlayerTelemetry("stall_start", { bufferLeftSec: getBufferLeft() });
              stallTimerStartRef.current = performance.now();
              isCountingStallRef.current = true;
            }
          }}
          onPlaying={() => {
            if (!hasPlaybackStartedRef.current) {
              qualityStartTimeRef.current = videoRef.current.currentTime;
              startupDelayEndRef.current = performance.now();
              qualityIntervalActiveRef.current = true;
              qualityOfExprienceRef.current.startupDelayMs =
                startupDelayEndRef.current - experimentStartRef.current;
              hasPlaybackStartedRef.current = true;
              recordPlayerTelemetry("playback_started", {
                startupDelayMs: qualityOfExprienceRef.current.startupDelayMs,
              });
            }
            if (isCountingStallRef.current) {
              stallTimerEndRef.current = performance.now();
              isCountingStallRef.current = false;
              let stallDuration = stallTimerEndRef.current - stallTimerStartRef.current;

              qualityOfExprienceRef.current.stallDurationTotalMs += stallDuration;
              recordPlayerTelemetry("stall_end", { stallDurationMs: stallDuration });


            }
            if (hasPlaybackStartedRef.current && !isReportingQOERef.current) {
              isReportingQOERef.current = true;
              setTimeout(() => {
                void finishAndRecordExperiment();
              }, 120000);
            }
          }}
          onEnded={() => {}}
        ></video>
        <div>
          <button
            onClick={() => {
              experimentModify("stop");
            }}
          >
            STOPING EXPERIMENT
          </button>
        </div>
        <div>
          <h1>ABR-Behaving Buttons:</h1>
          <button
            onClick={() => {
              setPlaybackQualityAndLog(0, "Manual");
            }}
          >
            480p
          </button>
          <button
            onClick={() => {
              setPlaybackQualityAndLog(1, "Manual");
            }}
          >
            720p
          </button>
          <button
            onClick={() => {
              setPlaybackQualityAndLog(2, "Manual");
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
      </div>}
      {isExperimentReady && experimentConfig?.workloads.dashJs && <CompetingPlayer />}
    </>
  );
}
export default App;
