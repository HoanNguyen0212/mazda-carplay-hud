# CMU diagnostics clock-sync payload

The parent repository carries the CarPlay/HUD source changes. `diagnostics/` contains the reviewed listener source and audited scripts for the Mazda v3.7.3 bundle.

## Clock model

The 5SE relay sends `SYNC <epoch_ms> <monotonic_ms>` over the existing UDP 8888 channel. The CMU listener anchors that trusted epoch to its monotonic clock and writes new evidence records with sequence, monotonic time, epoch milliseconds, source, UTC, and payload. When CMU realtime is invalid or differs by more than six hours, the root listener corrects `CLOCK_REALTIME` once and records the result in `clock-sync-status.txt`.

## Retention

The 5SE LaunchDaemon keeps the newest seven dated folders under `/var/mobile/Documents/GMapHUD/auto-sessions`. It records CMU snapshot provenance in `meta/cmu-snapshots.ndjson`. It does not delete CMU source logs.
