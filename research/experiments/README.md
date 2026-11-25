# Preliminary experiment data — 23 November 2026

[Per-run metrics](run-metrics.csv) preserve the numeric fields from all 35 available run JSON records in the supplied experiment archive. [Batch configuration index](batch-configurations.csv) records workload switches and throughput averaging settings from the saved configuration snapshots. The batch ID is the original numbered folder prefix.

The six complete five-run batches (007, 008, 009, 011, 012, 013) underpin the table in the main README. The remaining three-run batch (010) and two later instrumented single runs (014, 015) are supplementary, not additional replicates pooled into that table. They are retained here for transparency. Batch 014 recorded 111.96 seconds at 1080p despite dash.js and one browser being configured; workload health was not independently established. The later instrumented full-contention run (015) recorded 47.94 seconds at 1080p.

All configurations enabled the custom player. The shared bottleneck was configured at 16 Mbps and 100 ms delay, with no configured jitter or loss. The saved ABR tuning parameters were the same except for the throughput averaging window (seven segments in batch 007, five elsewhere). The common recorded values were a 30 s buffer target, 4 s low-buffer threshold, 0.8 quality safety factor, 1.3 ETA factor, 0.25 timeout buffer factor, 15000 ms lowest-quality timeout, 10000 ms initial download timeout and 3000 ms retry delay. Browser scheduling snapshots specify a 2000 ms launch interval, 0 ms initial delay, 5000 ms start interval and 0 ms controller jitter. These are configured values, not a guarantee of actual application start timing. The browser journey also contains its own scheduling and seed handling; exact equivalence to the saved controller seed is not established.

The baseline is therefore not a strictly matched control for causal attribution to workload alone. Results are exploratory, and comparisons reflect observed configurations rather than validated effects.

The historical 480p/720p counter-label issue should be considered before interpreting those columns. The numeric columns are retained under their original field names, not silently relabeled. The 1080p duration is unaffected by that specific issue.

This CSV export contains scalar metrics, not the original JSON event histories, browser logs or full telemetry traces. No new experiments were performed for this documentation update.
