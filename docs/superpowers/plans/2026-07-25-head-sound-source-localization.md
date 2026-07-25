# “智慧头”声源定位实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `head` 板上同时运行双麦声源定位、Device-Side AEC 和实时对话，并以日志和回调输出 `-90°~+90°` 方向。

**Architecture:** ES7210 输出 `MIC1/MIC2/MIC3` 三通道 `MMR` 数据。`AudioService` 将同一份 16 kHz 原始数据分别送入 ESP-SR AFE 和独立 `SoundSourceLocator`；AFE 输出对话用单声道，定位器封装 `afe_doa` 并在播放期间冻结角度。

**Tech Stack:** ESP-IDF、ESP-SR 2.3、AFE DOA/SRP-PHAT、ES7210 TDM、C++、FreeRTOS EventGroup、Kconfig、CMake

---

### Task 1：将 head 输入扩展为 MMR

**Files:**
- Modify: `main/boards/head/head_audio_codec.cc`

- [ ] **Step 1: 执行缺失三通道配置的失败检查**

```powershell
python -c "from pathlib import Path; c=Path('main/boards/head/head_audio_codec.cc').read_text(); assert 'input_channels_ = 3' in c and 'ES7210_SEL_MIC2' in c and 'I2S_TDM_SLOT1' in c"
```

Expected: FAIL。

- [ ] **Step 2: 启用 MIC1、MIC2 和 MIC3**

将 Codec 配置改为：

```cpp
input_channels_ = 3;

es7210_cfg.mic_selected =
    ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
```

将 RX TDM 槽改为：

```cpp
.slot_mask = static_cast<i2s_tdm_slot_mask_t>(
    I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2),
.total_slot = 4,
```

打开输入时保留四槽源并选择前三槽：

```cpp
.channel = 4,
.channel_mask = static_cast<uint16_t>(
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)),
```

同时为两路真实麦克风设置相同增益：

```cpp
ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(
    input_dev_,
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
    input_gain_));
```

- [ ] **Step 3: 重新运行三通道检查**

Run: Step 1 的命令。

Expected: PASS。

### Task 2：新增 ESP-SR 声源定位器

**Files:**
- Create: `main/audio/processors/sound_source_locator.h`
- Create: `main/audio/processors/sound_source_locator.cc`

- [ ] **Step 1: 定义定位器接口**

`sound_source_locator.h` 定义：

```cpp
class SoundSourceLocator {
public:
    SoundSourceLocator(const char* input_format, int sample_rate,
        float resolution_degrees, float microphone_distance_m,
        int window_samples, float minimum_rms);
    ~SoundSourceLocator();

    bool IsReady() const;
    void Feed(const std::vector<int16_t>& data, bool playback_active);
    void Reset();
    void OnDirection(std::function<void(float)> callback);

private:
    float ConvertAndSmooth(float raw_angle);
    bool HasEnoughEnergy(const int16_t* frame) const;
};
```

成员保存 `afe_doa_handle_t*`、三通道窗口缓冲、三值中值历史、平滑角度、回调及日志时间戳。禁止复制该类。

- [ ] **Step 2: 创建和释放 DOA 句柄**

构造函数调用：

```cpp
doa_ = afe_doa_create(
    input_format,
    sample_rate,
    resolution_degrees,
    microphone_distance_m,
    window_samples);
```

创建失败时只记录错误并保持 `IsReady()==false`。析构函数在非空时调用 `afe_doa_destroy()`。

- [ ] **Step 3: 累积 MMR 窗口并在播放期间冻结**

`Feed()` 必须：

1. 检查 DOA 已创建且输入样本数为三的倍数。
2. `playback_active=true` 时调用 `Reset()` 并返回。
3. 将输入追加到预留容量的缓冲区。
4. 每积累 `1024 * 3` 个样本，先计算 MIC1/MIC2 RMS。
5. RMS 达到门限后调用 `afe_doa_process(doa_, frame)`。
6. 处理后移除一个完整窗口，保留多出的后续样本。

- [ ] **Step 4: 转换、平滑并限频记录角度**

将 ESP-SR 的 `0°~180°` 转换为：

```cpp
float signed_angle = std::clamp(raw_angle - 90.0f, -90.0f, 90.0f);
```

对最近三个结果取中值，再以 `0.35` 系数低通：

```cpp
smoothed_angle_ = has_smoothed_angle_
    ? smoothed_angle_ * 0.65f + median * 0.35f
    : median;
```

每个有效窗口触发回调；日志最多每 250 ms 输出一次。

### Task 3：把定位器并联到 AudioService

**Files:**
- Modify: `main/audio/audio_service.h`
- Modify: `main/audio/audio_service.cc`

- [ ] **Step 1: 扩展回调与服务状态**

在 `AudioServiceCallbacks` 增加：

```cpp
std::function<void(float)> on_sound_direction_change;
```

在启用配置时包含定位器头文件并保存：

```cpp
std::unique_ptr<SoundSourceLocator> sound_source_locator_;
```

- [ ] **Step 2: 初始化 head 定位参数**

`AudioService::Initialize()` 在 Codec 和重采样器初始化后创建：

```cpp
sound_source_locator_ = std::make_unique<SoundSourceLocator>(
    "MMR", 16000, 5.0f, 0.048f, 1024, 200.0f);
sound_source_locator_->OnDirection([this](float angle) {
    if (callbacks_.on_sound_direction_change) {
        callbacks_.on_sound_direction_change(angle);
    }
});
```

仅当 `codec_->input_channels()==3` 且 `input_reference()==true` 时创建；否则记录一次配置错误并让对话继续。

- [ ] **Step 3: 使用播放事件位控制冻结**

复用当前未使用的 `AS_EVENT_PLAYBACK_NOT_EMPTY`：

- 统一根据解码队列、在途解码、播放队列和在途输出更新事件位。
- 使用 decoder generation 丢弃 `ResetDecoder()` 前已经开始的旧解码结果。
- 解码失败和音频测试队列转移也必须更新事件位。
- `Stop()` 等待输入、输出和 Opus 三个任务退出后才允许再次 `Start()`。
- AudioService 回调不得直接调用 `Start()`/`Stop()`，生命周期操作应投递到应用任务。

- [ ] **Step 4: 旁路送入定位数据**

在 `AudioInputTask()` 取得 16 kHz 数据后、移动数据给唤醒词或 AFE 之前调用：

```cpp
if (sound_source_locator_) {
    bool playback_active =
        xEventGroupGetBits(event_group_) & AS_EVENT_PLAYBACK_NOT_EMPTY;
    sound_source_locator_->Feed(data, playback_active);
}
```

定位器使用 `const` 引用，不修改 AFE 输入。

- [ ] **Step 5: 保持其他三通道路径兼容**

`CustomWakeWord`、`NoAudioProcessor` 和音频测试路径对所有
`input_channels() > 1` 的输入按通道步长抽取 MIC1，避免把 `MMR`
交错数据当成单声道。

### Task 4：增加构建开关

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `main/CMakeLists.txt`
- Modify: `main/boards/head/config.json`

- [ ] **Step 1: 新增仅限 head 的 Kconfig**

```kconfig
config USE_SOUND_SOURCE_LOCALIZATION
    bool "Enable Sound Source Localization"
    default y if BOARD_TYPE_HEAD
    depends on BOARD_TYPE_HEAD && USE_AUDIO_PROCESSOR
    help
        Estimate the sound direction with the two microphones on the Head board.
```

- [ ] **Step 2: 条件编译定位器**

```cmake
if(CONFIG_USE_SOUND_SOURCE_LOCALIZATION)
    list(APPEND SOURCES "audio/processors/sound_source_locator.cc")
endif()
```

- [ ] **Step 3: 发布配置显式启用**

在 `main/boards/head/config.json` 的 `sdkconfig_append` 加入：

```json
"CONFIG_USE_SOUND_SOURCE_LOCALIZATION=y"
```

### Task 5：静态验证与交付

**Files:**
- Modify: `main/boards/head/README.md`

- [ ] **Step 1: 更新硬件说明**

README 说明：

- MIC1 左、MIC2 右、MIC3 参考。
- 麦距约 48 mm，两个孔朝正前方。
- 日志角度左负、正前 0°、右正。
- 播放期间冻结角度。
- 双麦存在前后镜像歧义。

- [ ] **Step 2: 执行静态检查**

```powershell
python -m json.tool main/boards/head/config.json
git diff --check
```

Expected: 两条命令 exit code 0。

- [ ] **Step 3: 检查实现关键条件**

```powershell
python -c "from pathlib import Path; c=Path('main/boards/head/head_audio_codec.cc').read_text(); a=Path('main/audio/audio_service.cc').read_text(); k=Path('main/Kconfig.projbuild').read_text(); assert 'input_channels_ = 3' in c; assert 'afe_doa_create' in Path('main/audio/processors/sound_source_locator.cc').read_text(); assert 'AS_EVENT_PLAYBACK_NOT_EMPTY' in a; assert 'USE_SOUND_SOURCE_LOCALIZATION' in k"
```

Expected: PASS。

- [ ] **Step 4: 用户实机验证**

用户执行：

```powershell
python scripts/release.py head
```

烧录后验证实时对话、AEC、左/中/右角度、播放冻结和长时间并发稳定性。编译和烧录不由本实施会话执行。

## 提交说明

不自动创建 Git 提交；只有用户明确要求时才提交或推送。
