# Head Log and README Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove repetitive localization/servo logs and replace the custom header of the root README with an accurate Chinese project introduction.

**Architecture:** Keep errors, warnings, and one-shot initialization summaries while removing periodic angle output from hot paths. Limit README edits to the text above the existing separator so the upstream documentation remains byte-for-byte unchanged.

**Tech Stack:** ESP-IDF logging macros and Markdown.

---

### Task 1: Remove repetitive runtime logs

**Files:**
- Modify: `main/application.cc`
- Modify: `main/audio/audio_service.cc`
- Modify: `main/boards/head/head_board.cc`

- [ ] **Step 1: Remove localization angle logging**

Delete the 500 ms `Sound direction` log and its static timestamp from
`callbacks.on_sound_direction`. Keep forwarding every valid result to
`Board::OnSoundDirection`.

- [ ] **Step 2: Remove per-step servo logs**

Delete startup-center, startup-step, and tracking-step info logs. Keep all
`ESP_LOGE`/`ESP_LOGW` calls plus the one-shot tracking-started and
Head-board-initialized summaries.

- [ ] **Step 3: Remove input-level diagnostics**

Delete the three-channel RMS, peak, and correlation accumulation plus its
`Input levels` log from `AudioService::ReadAudioData`; this diagnostic hot path
must not consume CPU after its log is removed.

### Task 2: Rewrite only the custom README header

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace the content above the separator**

Describe the ESP32-S3 Head board, ES8311/ES7210 audio channels, real-time chat,
sound localization, PCA9685 wiring, 75°/105°/135° mapping, 20°/second tracking,
GPIO0 warning, build command, board selection, and upstream v2.2.6 baseline.

- [ ] **Step 2: Preserve upstream content**

Verify the separator and every line below it are unchanged.

### Task 3: Verify

- [ ] **Step 1: Run static checks**

Read diagnostics for the two edited C++ files and run `git diff --check`.

- [ ] **Step 2: Search removed logs**

Confirm `Sound direction`, `startup angle`, `startup center`, and
`Servo tracking angle` and `Input levels` no longer appear in production
sources, while all error paths remain.
