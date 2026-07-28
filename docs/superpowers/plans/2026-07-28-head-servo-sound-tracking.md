# Head Servo Sound Tracking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make PCA9685 CH1 follow valid sound-source direction results within the calibrated 75°–135° mechanical range.

**Architecture:** `Application` forwards valid localization results through a default no-op `Board` event method. `HeadBoard` maps the sound angle and overwrites a one-element FreeRTOS queue; a dedicated task performs rate-limited PCA9685 I2C writes so the localization callback never blocks.

**Tech Stack:** C++17, ESP-IDF, FreeRTOS queue/task APIs, existing `SoundDirectionResult` callback and PCA9685 driver.

---

### Task 1: Add a board-level sound-direction event

**Files:**
- Modify: `main/boards/common/board.h`
- Modify: `main/application.cc`

- [ ] **Step 1: Add a default no-op board hook**

Add `virtual void OnSoundDirection(float angle_deg, float confidence)` to `Board`.
The base implementation casts both arguments to void so boards without a servo
need no changes.

- [ ] **Step 2: Forward localization results**

In the existing `callbacks.on_sound_direction` lambda, call
`Board::GetInstance().OnSoundDirection(result.angle_deg, result.confidence)`
before the existing 500 ms rate-limited log. Do not schedule or perform I2C in
this callback.

### Task 2: Add Head board tracking worker

**Files:**
- Modify: `main/boards/head/config.h`
- Modify: `main/boards/head/head_board.cc`

- [ ] **Step 1: Define calibrated tracking constants**

Add constants for channel 1, left 75°, center 105°, right 135°, 1° output
deadband, 200 ms minimum write interval, 15°/second maximum tracking speed,
and a one-element target queue.

- [ ] **Step 2: Make startup movement mechanically safe**

First command CH1 to 105° and wait 2 seconds for the unknown physical startup
position to settle. Then replace `0° → 90° → 180° → 90°` with a gradual
`75° → 105° → 135° → 105°` sequence at 15°/second, leaving CH1 at 105°.

- [ ] **Step 3: Create the queue and worker after PCA9685 initialization**

Create `QueueHandle_t servo_target_queue_` with one float item. Start a
`head_servo` task only after driver initialization and startup test succeed.
If queue/task creation fails, log and leave localization/audio/network running.

- [ ] **Step 4: Implement non-blocking event ingestion**

Override `OnSoundDirection`. Reject non-finite angles, clamp sound direction to
`[-90, 90]`, linearly map the left and right halves to `[75, 105]` and
`[105, 135]`, clamp output to `[75, 135]`, and use `xQueueOverwrite` so the
callback returns immediately with only the newest target.

- [ ] **Step 5: Implement rate-limited servo output**

The worker blocks on the queue while settled. While moving, wait until 200 ms has
elapsed since the previous write attempt, accepting replacement targets during
the wait. Move at most 3° per write so the maximum tracking speed is 15°/second.
Skip remaining movement below 1° from the last successful output. Update the
attempt timestamp before `SetAngle` so a disconnected device cannot cause an I2C
retry storm; update the current angle only after success. On failure, stop moving
until a new localization result arrives. With no new input, finish the current
gradual movement and then keep the last angle.

### Task 3: Document and statically verify tracking

**Files:**
- Modify: `main/boards/head/README.md`
- Modify: `docs/superpowers/specs/2026-07-28-head-pca9685-servo-design.md`

- [ ] **Step 1: Document mapping and runtime behavior**

Record `-90°→75°`, `0°→105°`, `+90°→135°`, the 1° deadband, 200 ms minimum
interval, latest-target queue, safe startup sequence, and hold-last-angle behavior.

- [ ] **Step 2: Run static verification**

Read IDE diagnostics for all edited C++ headers and sources. Search for the old
unsafe startup angle sequence and confirm the application callback calls only the
board hook, while `Pca9685::SetAngle` appears in the Head worker/startup path.

- [ ] **Step 3: Hand off hardware verification**

The user builds and flashes the Head firmware, then verifies left/front/right
sources move CH1 toward 75°/105°/135°, output remains within limits, updates are
at most 5 Hz, and silence/playback holds the last angle.
