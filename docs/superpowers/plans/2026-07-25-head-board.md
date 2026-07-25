# “智慧头”开发板实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增可在 SDK 中选择的 `head`（智慧头）ESP32-S3 N16R8 板型，并通过 ES7210 MIC1 + MIC3 回采实现设备端 AEC。

**Architecture:** `HeadBoard` 负责 I2C、Boot 按键和音频对象创建，显示与 LED 沿用基类的空实现。板目录内的 `HeadAudioCodec` 独立实现 ES8311 输出和 ES7210 四槽 TDM 输入，只启用 MIC1/MIC3；ESP Codec Dev 选择 slot 0/2 并将其压缩为 AFE 所需的连续 `MR` 两通道数据。

**Tech Stack:** ESP-IDF、C++、ESP Codec Dev、ES8311、ES7210、I2S STD/TDM、ESP-SR AFE、CMake、Kconfig

---

## 文件映射

- 新建 `main/boards/head/config.h`：N16R8 板级引脚、采样率和 Codec 地址。
- 新建 `main/boards/head/config.json`：`head` 发布目标、16 MB 分区和 Device-Side AEC。
- 新建 `main/boards/head/head_audio_codec.h`：板级音频类接口。
- 新建 `main/boards/head/head_audio_codec.cc`：ES8311 输出、ES7210 MIC1/MIC3 输入和通道过滤。
- 新建 `main/boards/head/head_board.cc`：Wi-Fi 板、I2C 和 Boot 按键初始化。
- 修改 `main/Kconfig.projbuild`：增加“智慧头”板型并加入 AEC 白名单。
- 修改 `main/CMakeLists.txt`：将 `BOARD_TYPE_HEAD` 映射到 `main/boards/head/`。
- 修改 `main/idf_component.yml`：要求包含稀疏 TDM 掩码修复的 ESP Codec Dev 1.5.11。
- 新建 `main/boards/head/README.md`：硬件接线、构建与硬件验收说明。

### Task 1：注册板型和发布预设

**Files:**
- Create: `main/boards/head/config.h`
- Create: `main/boards/head/config.json`
- Modify: `main/Kconfig.projbuild:120-551,801-813`
- Modify: `main/CMakeLists.txt:86-130`

- [ ] **Step 1: 验证板型当前不存在**

Run:

```powershell
python scripts/release.py head
```

Expected: FAIL，提示找不到 `main/boards/head/config.json` 或找不到 `head` 板配置。

- [ ] **Step 2: 新增板级硬件配置**

Create `main/boards/head/config.h`:

```cpp
#ifndef _HEAD_BOARD_CONFIG_H_
#define _HEAD_BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_21
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_41
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_39
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_40
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_38

#define AUDIO_CODEC_PA_PIN          GPIO_NUM_42
#define AUDIO_CODEC_I2C_SDA_PIN     GPIO_NUM_13
#define AUDIO_CODEC_I2C_SCL_PIN     GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR     ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR     ES7210_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO GPIO_NUM_0

#endif
```

- [ ] **Step 3: 新增 N16R8 发布配置**

Create `main/boards/head/config.json`:

```json
{
    "target": "esp32s3",
    "builds": [
        {
            "name": "head",
            "sdkconfig_append": [
                "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
                "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/v2/16m.csv\"",
                "CONFIG_USE_DEVICE_AEC=y"
            ]
        }
    ]
}
```

- [ ] **Step 4: 增加 SDK 可选板名**

在 `main/Kconfig.projbuild` 的 `choice BOARD_TYPE` 末尾、`endchoice` 前加入：

```kconfig
    config BOARD_TYPE_HEAD
        bool "智慧头"
        depends on IDF_TARGET_ESP32S3
```

在 `config USE_DEVICE_AEC` 的板型依赖表达式末尾加入：

```kconfig
        || BOARD_TYPE_HEAD
```

保持反斜杠续行和括号有效。

- [ ] **Step 5: 增加 CMake 板目录映射**

在 `main/CMakeLists.txt` 的板型映射链中加入：

```cmake
elseif(CONFIG_BOARD_TYPE_HEAD)
    set(BOARD_TYPE "head")
```

- [ ] **Step 6: 验证配置文件语法**

Run:

```powershell
python -m json.tool main/boards/head/config.json
```

Expected: PASS，并输出格式化后的 JSON。

- [ ] **Step 7: 检查差异**

Run:

```powershell
git diff --check
git diff -- main/Kconfig.projbuild main/CMakeLists.txt main/boards/head/config.h main/boards/head/config.json
```

Expected: `git diff --check` 无输出；差异仅包含 `head` 板注册和配置。

### Task 2：实现 MIC1 + MIC3 板级音频 Codec

**Files:**
- Create: `main/boards/head/head_audio_codec.h`
- Create: `main/boards/head/head_audio_codec.cc`
- Modify: `main/idf_component.yml:33`

- [ ] **Step 1: 新增音频类接口**

Create `main/boards/head/head_audio_codec.h`:

```cpp
#ifndef _HEAD_AUDIO_CODEC_H_
#define _HEAD_AUDIO_CODEC_H_

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>

class HeadAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    std::mutex data_if_mutex_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din);
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

public:
    HeadAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
                   gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                   gpio_num_t dout, gpio_num_t din, gpio_num_t pa_pin,
                   uint8_t es8311_addr, uint8_t es7210_addr);
    ~HeadAudioCodec() override;

    void SetOutputVolume(int volume) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
};

#endif
```

- [ ] **Step 2: 实现 Codec 初始化和明确的物理通道选择**

Create `main/boards/head/head_audio_codec.cc`。构造函数必须：

```cpp
#include "head_audio_codec.h"

#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <esp_log.h>

#define TAG "HeadAudioCodec"

HeadAudioCodec::HeadAudioCodec(
    void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
    gpio_num_t din, gpio_num_t pa_pin, uint8_t es8311_addr,
    uint8_t es7210_addr) {
    duplex_ = true;
    input_reference_ = true;
    input_channels_ = 2;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != nullptr);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != nullptr);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != nullptr);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != nullptr);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != nullptr);

    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != nullptr);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != nullptr);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != nullptr);

    ESP_LOGI(TAG, "Initialized with MIC1 as microphone and MIC3 as AEC reference");
}
```

该选择保证 MIC2/MIC4 不被 ES7210 启用。

- [ ] **Step 3: 实现 STD 输出和四槽 TDM 输入**

`CreateDuplexChannels()` 使用与公共 Box codec 一致的 16-bit/24 kHz 时序。RX 帧保留四个物理槽，但 DMA 仅接收 slot 0/2，使输出连续排列为 MIC1、MIC3：

```cpp
void HeadAudioCodec::CreateDuplexChannels(
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(output_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = mclk, .bclk = bclk, .ws = ws, .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(input_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = static_cast<i2s_tdm_slot_mask_t>(
                I2S_TDM_SLOT0 | I2S_TDM_SLOT2),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = 4,
        },
        .gpio_cfg = {
            .mclk = mclk, .bclk = bclk, .ws = ws,
            .dout = I2S_GPIO_UNUSED, .din = din,
            .invert_flags = {},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
}
```

- [ ] **Step 4: 实现 MIC1/MIC3 过滤与音频生命周期**

先将 `main/idf_component.yml` 的依赖提升到包含全双工稀疏 TDM 掩码修复的版本：

```yaml
  espressif/esp_codec_dev: ~1.5.11
```

`EnableInput()` 用四通道源和 slot 0/2 掩码选择 MIC1/MIC3。ESP Codec Dev 1.5.11 会保留 TDM RX 的 `0x5` 掩码，并限制同步到 STD TX 的掩码为合法范围：

```cpp
void HeadAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) return;
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = static_cast<uint16_t>(
                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)),
            .sample_rate = static_cast<uint32_t>(input_sample_rate_),
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(
            input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void HeadAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) return;
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = static_cast<uint32_t>(output_sample_rate_),
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

void HeadAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

int HeadAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_read(input_dev_, dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int HeadAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_write(output_dev_, data, samples * sizeof(int16_t)));
    }
    return samples;
}
```

析构函数按 `input_dev_`、`output_dev_`、codec interface、control interface、GPIO interface、data interface 的逆初始化顺序关闭和释放：

```cpp
HeadAudioCodec::~HeadAudioCodec() {
    if (output_enabled_) ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    if (input_enabled_) ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(output_dev_);
    esp_codec_dev_delete(input_dev_);
    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}
```

- [ ] **Step 5: 编译检查 Codec**

Run:

```powershell
idf.py set-target esp32s3
idf.py build
```

Expected: 当前默认板构建通过；`head` 源文件尚未被默认板纳入，不影响已有板。

### Task 3：实现无屏、无 LED 的 HeadBoard

**Files:**
- Create: `main/boards/head/head_board.cc`

- [ ] **Step 1: 实现 I2C、按钮和音频对象**

Create `main/boards/head/head_board.cc`:

```cpp
#include "wifi_board.h"
#include "head_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <driver/i2c_master.h>
#include <esp_log.h>

#define TAG "HeadBoard"

class HeadBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;

    void InitializeI2c() {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(
                    app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

public:
    HeadBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
        ESP_LOGI(TAG, "Head board initialized");
    }

    AudioCodec* GetAudioCodec() override {
        static HeadAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR);
        return &audio_codec;
    }
};

DECLARE_BOARD(HeadBoard);
```

不要实现 `GetDisplay()` 或 `GetLed()`；`Board` 基类会返回 `NoDisplay` 和 `NoLed`。

- [ ] **Step 2: 构建 head 发布目标**

Run:

```powershell
python scripts/release.py head
```

Expected: PASS，生成 `head` 的 ESP32-S3 发布产物；日志显示启用 `CONFIG_USE_DEVICE_AEC`。

- [ ] **Step 3: 检查生成配置**

Run:

```powershell
Select-String -Path sdkconfig -Pattern "CONFIG_BOARD_TYPE_HEAD=y|CONFIG_USE_DEVICE_AEC=y|CONFIG_SPIRAM_MODE_OCT=y|CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"
```

Expected: 四项配置均被匹配。

### Task 4：补充硬件说明并完成验证

**Files:**
- Create: `main/boards/head/README.md`

- [ ] **Step 1: 写入板级说明**

Create `main/boards/head/README.md`，内容应明确：

```markdown
# 智慧头（head）

主控为 ESP32-S3 N16R8，无显示屏、无 LED。音频输出使用 ES8311，
输入使用 ES7210：MIC1 为语音，MIC3 接 ES8311 OUTP/OUTN 作为 AEC
参考，MIC2/MIC4 不参与当前固件的采集。

构建：

    python scripts/release.py head

设备端 AEC 默认启用。Boot 单击切换对话；启动阶段单击进入配网；
空闲状态双击切换设备端 AEC。

首次硬件验证应确认 ES8311、ES7210 均可初始化，并使用音频抓取验证
输出两通道顺序为 MIC1、MIC3。
```

- [ ] **Step 2: 执行最终静态验证**

Run:

```powershell
git diff --check
git status --short
```

Expected: `git diff --check` 无输出；仅设计文档、计划文档、`head` 新文件以及 Kconfig/CMake 注册发生变化。

- [ ] **Step 3: 执行最终构建**

Run:

```powershell
python scripts/release.py head
```

Expected: Exit code 0，无编译错误。

- [ ] **Step 4: 执行硬件验收**

烧录后检查：

1. 启动日志出现 `HeadBoard` 和 `HeadAudioCodec` 初始化信息。
2. ES8311 播放无爆音、PA GPIO42 能正常使能。
3. 抓取输入数据确认第 1 路为 MIC1，第 2 路为 MIC3 回采。
4. 遮挡或断开 MIC2 时采集结果不变。
5. 播放 TTS 并分别关闭/开启 AEC，开启后近端 MIC1 中的扬声器回声显著降低。
6. Boot 单击及双击行为符合设计。

硬件验收依赖实物，不以仅编译通过替代。

## 提交说明

本计划不自动创建 Git 提交。只有用户明确要求提交时，才将相关文件暂存并创建提交。
