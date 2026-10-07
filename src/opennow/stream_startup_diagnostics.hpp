#pragma once

namespace opennow
{
void WriteStreamStartupStage(const char* stage);
void LogAppLifecycleEvent(const char* event, const char* detail = nullptr);
void BeginAppLifecycleLog();
void AppendDiagnosticLogBlock(const char* source, const char* text, bool sync_to_disk = false);
// Bounded, non-blocking-ish action trace: events are copied into a fixed ring
// and written in batches by the UI loop. Do not pass typed text or credentials.
void TraceAppAction(const char* action, const char* detail = nullptr);
void FlushAppActionTrace();
void InstallAppCrashDiagnostics();
void SetAppCrashContext(int page, int launch_state, int stream_state);

// Persistent stage marker ("heartbeat"). SetCurrentStage() records which part of the loop is
// running; StartStageHeartbeat() spawns a thread that flushes that value to
// /data/gfnps4/last_stage.txt once per second with fsync. The last value on disk therefore
// identifies the stage that was executing when the process died, even when it died in a way
// that never ran the signal handlers - which is exactly what happened in 2.80, where the
// diagnostic log stopped mid-session with no fatal-signal line despite all five handlers
// being registered (boot log: failed_mask=0x00).
void SetCurrentStage(int stage);
int GetCurrentStage();
void StartStageHeartbeat();
void StopStageHeartbeat();
}

// C ABI bridge for the vendored libpeer C sources.
extern "C" void opennow_write_stream_startup_stage_from_c(const char* stage);
extern "C" void opennow_log_app_lifecycle_from_c(const char* event, const char* detail);
