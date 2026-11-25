import "./QoEResult.css";

const QUALITY = [
  { key: "playbackDurationTotal1080Sec", label: "1080p", color: "#16a34a" },
  { key: "playbackDurationTotal720Sec", label: "720p", color: "#2563eb" },
  { key: "playbackDurationTotal480Sec", label: "480p", color: "#d97706" },
];

const QUALITY_NAMES = { 0: "480p", 1: "720p", 2: "1080p" };
const numeric = (value) => Number.isFinite(Number(value)) ? Number(value) : 0;
const fixed = (value, decimals = 2) => numeric(value).toFixed(decimals);
const totalQualitySeconds = (qoe) => QUALITY.reduce((total, item) => total + numeric(qoe[item.key]), 0);
const qualityLabel = (id) => QUALITY_NAMES[id] ?? `ID ${String(id)}`;

/** For console output, show units and readable switch/abort tables rather than a JSON dump. */
export function logQoEToConsole(qoe, title = "Run 1", isAverage = false) {
  const total = totalQualitySeconds(qoe);
  const stallCount = numeric(qoe.stallCountTotal);
  console.group(`ABR QoE · ${title}`);
  console.table({
    "Total video playback": `${fixed(total)} s`,
    "1080p share": `${total > 0 ? fixed(100 * numeric(qoe.playbackDurationTotal1080Sec) / total, 1) : "0.0"}%`,
    "Startup delay": `${fixed(numeric(qoe.startupDelayMs) / 1000)} s`,
    "Stalls": isAverage ? fixed(stallCount) : String(stallCount),
    "Stall duration": `${fixed(numeric(qoe.stallDurationTotalMs) / 1000)} s`,
    "Quality switches": isAverage ? fixed(qoe.qualitySwitchCount) : String(qoe.qualitySwitchCount),
    "Aborted downloads": isAverage ? fixed(qoe.abortedDownloadCount) : String(qoe.abortedDownloadCount),
  });
  console.table(QUALITY.map(({ key, label }) => ({
    "Quality": label,
    "Playback (s)": fixed(qoe[key]),
    "Share (%)": total > 0 ? fixed(100 * numeric(qoe[key]) / total, 1) : "0.0",
  })));
  if (!isAverage) {
    const switches = qoe.qualitySwitchHistoryArr ?? [];
    if (switches.length) {
      console.table(switches.map((item, index) => ({
        "#": index + 1,
        "At (s)": fixed(item.videoTimeSec),
        From: qualityLabel(item.from),
        To: qualityLabel(item.to),
        Reason: item.reason,
      })));
    }
    const aborts = qoe.abortHistoryArr ?? [];
    if (aborts.length) console.table(aborts);
  }
  console.debug("Machine-readable QoE:", qoe);
  console.groupEnd();
}

export function QoEResult({ qoe, isAverage = false, title = "Run 1", label = "" }) {
  const total = totalQualitySeconds(qoe);
  const switches = !isAverage && Array.isArray(qoe.qualitySwitchHistoryArr) ? qoe.qualitySwitchHistoryArr : [];
  const aborts = !isAverage && Array.isArray(qoe.abortHistoryArr) ? qoe.abortHistoryArr : [];
  const stallCount = numeric(qoe.stallCountTotal);
  const metricValue = (number) => isAverage ? fixed(number) : String(numeric(number));

  return (
    <section className="qoe-report" aria-label="Experiment result">
      <header className="qoe-report-header">
        <div>
          <div className="qoe-eyebrow">EXPERIMENT RESULTS {label ? `· ${label}` : ""}</div>
          <h2>{title}</h2>
          <p>Playback quality, responsiveness, and interruption metrics</p>
        </div>
        <span className={`qoe-status ${stallCount === 0 ? "qoe-status-good" : "qoe-status-warning"}`}>
          {stallCount === 0 ? "No stalls" : `${metricValue(stallCount)} stalls${isAverage ? " (mean)" : ""}`}
        </span>
      </header>

      <div className="qoe-stat-grid">
        <div className="qoe-stat"><span>Measured playback</span><strong>{fixed(total)} <small>s</small></strong></div>
        <div className="qoe-stat"><span>Time at 1080p</span><strong>{total ? fixed(100 * numeric(qoe.playbackDurationTotal1080Sec) / total, 1) : "0.0"}<small>%</small></strong></div>
        <div className="qoe-stat"><span>Startup delay</span><strong>{fixed(numeric(qoe.startupDelayMs) / 1000)} <small>s</small></strong></div>
        <div className="qoe-stat"><span>Stall duration</span><strong>{fixed(numeric(qoe.stallDurationTotalMs) / 1000)} <small>s</small></strong></div>
        <div className="qoe-stat"><span>Quality switches</span><strong>{metricValue(qoe.qualitySwitchCount)}</strong></div>
        <div className="qoe-stat"><span>Aborted downloads</span><strong>{metricValue(qoe.abortedDownloadCount)}</strong></div>
      </div>

      <div className="qoe-section-title">Playback by resolution</div>
      <div className="qoe-quality-list">
        {QUALITY.map(({ key, label: quality, color }) => {
          const seconds = numeric(qoe[key]);
          const percent = total ? (seconds / total) * 100 : 0;
          return (
            <div key={key} className="qoe-quality-row">
              <span className="qoe-quality-name">{quality}</span>
              <div className="qoe-quality-track" role="img" aria-label={`${quality}: ${fixed(seconds)} seconds, ${fixed(percent, 1)} percent`}>
                <div className="qoe-quality-fill" style={{ width: `${percent}%`, background: color }} />
              </div>
              <span className="qoe-quality-amount">{fixed(seconds)} s <small>({fixed(percent, 1)}%)</small></span>
            </div>
          );
        })}
      </div>

      {!isAverage && (
        <div className="qoe-details-grid">
          <div>
            <div className="qoe-section-title">Quality switch history</div>
            {switches.length ? (
              <div className="qoe-table-wrap"><table className="qoe-table">
                <thead><tr><th>At</th><th>From</th><th>To</th><th>Reason</th></tr></thead>
                <tbody>{switches.map((s, i) => (
                  <tr key={i}><td>{fixed(s.videoTimeSec)} s</td><td>{qualityLabel(s.from)}</td><td>{qualityLabel(s.to)}</td><td>{s.reason ?? "—"}</td></tr>
                ))}</tbody>
              </table></div>
            ) : <p className="qoe-empty">No quality switches recorded.</p>}
          </div>
          <div>
            <div className="qoe-section-title">Download aborts</div>
            {aborts.length ? (
              <div className="qoe-table-wrap"><table className="qoe-table">
                <thead><tr><th>Segment</th><th>Quality</th><th>Time</th><th>Reason</th></tr></thead>
                <tbody>{aborts.map((a, i) => (
                  <tr key={i}><td>{a.segmentNumber ?? "—"}</td><td>{qualityLabel(a.quality)}</td><td>{fixed(a.elapsedMs / 1000)} s</td><td>{a.abortReason ?? "—"}</td></tr>
                ))}</tbody>
              </table></div>
            ) : <p className="qoe-empty">No aborted downloads recorded.</p>}
          </div>
        </div>
      )}
      {isAverage && <p className="qoe-note">Values are arithmetic means across completed runs. Quality-switch and abort histories are not averaged.</p>}
      <details className="qoe-raw"><summary>Show raw JSON</summary><pre>{JSON.stringify(qoe, null, 2)}</pre></details>
    </section>
  );
}
