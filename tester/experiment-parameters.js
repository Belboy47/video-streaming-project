// ============================================================================
// Main_Project experiment parameters — ONE file for workload + ABR controls.
// Edit ONLY the values on the right of '='. All named lines must be present.
// Syntax: export const name = value;  (literals: boolean, number, or simple quoted text)
// Changes take effect on the next frontend reload / experiment start.
// The backend parses literal assignments; do not use JS expressions or imports.
// ============================================================================

// WORKLOADS
export const customAbr = true; // Our React/MSE ABR player
export const dashJs = true; // Competing dash.js player
export const browsersEnabled = true; // Browsertime / WPR simulator group
export const browserCount = 2; // 0..16 (ignored when browsersEnabled=false)

// BROWSER SCHEDULING / RANDOMNESS
// Docker containers start successively after browserLaunchIntervalMs.
export const browserLaunchIntervalMs = 2000; // ms between Docker launches
// All journeys receive GO together, then each one waits its own start offset.
export const browserStartDelayMs = 0; // ms before first journey
export const browserStartIntervalMs = 5000; // ms spacing between journeys
export const browserStartJitterMs = 0; // +/- ms deterministic jitter (0 = off)
// trafficSeed below is used to reproduce the jitter sequence. It is also
// passed as BROWSER_RANDOM_SEED, but the journey must read it to seed its own RNG.

// EXPERIMENT EXECUTION / RESULT FOLDERS
export const repeatRuns = 1; // 1 = single run; 2..20 = repeat + average
export const trafficSeed = "baseline"; // safe characters: A-Z a-z 0-9 _ -
export const resultLabel = "telemetry_validation"; // appended at end of folder name
// Outputs: tester/experiments/001_HH-MM-SS_YYYY-MM-DD_baseline/
// URL query parameters are no longer needed; just open localhost:5173/.

// CUSTOM ABR PLAYER TUNING
// These starting values are from earlier project context; compare to your
// actual previous frontend/src/abr-parameters.js before scientific test runs.
export const throughputAveragingSegments = 5;
export const qualitySafetyFactor = 0.8;
export const lowBufferThresholdSec = 4;
export const bufferTargetSec = 30;
export const timeoutEtaFactor = 1.3;
export const timeoutBufferFactor = 0.25;
export const lowestQualityTimeoutMs = 15000;
export const initialDownloadTimeoutMs = 10000;
export const downloadRetryDelayMs = 3000;
