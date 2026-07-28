# “智慧头”开发板设计

## 目标

为 ESP32-S3 N16R8 自定义板新增独立板型。板型目录、内部标识和发布固件名均为 `head`，`menuconfig` 中显示为“智慧头”。开发板无显示屏、无 LED，使用 Wi-Fi、Boot 按键、ES8311 播放和 ES7210 采集，并默认启用 Device-Side AEC。

## 硬件配置

- 主控：ESP32-S3，16 MB Flash，8 MB Octal PSRAM
- I2S：MCLK GPIO21、BCLK GPIO41、WS GPIO39、DOUT GPIO40、DIN GPIO38
- 音频 PA：GPIO42
- Codec I2C：SDA GPIO13、SCL GPIO14
- Codec：ES8311 默认地址、ES7210 默认地址
- Boot 按键：GPIO0
- 显示屏、LED：无
- ES7210 MIC1/MIC2：间距 45 mm 的左右语音麦克风，同时用于声源定位
- ES7210 MIC3：连接 ES8311 OUTP/OUTN，作为 AEC 播放回采参考

输入和输出采样率均为 24 kHz。

## 架构

新增 `main/boards/head/`，包含板级配置、发布配置、板类和板级专用音频 Codec。

`HeadBoard` 继承 `WifiBoard`，负责初始化 I2C、Boot 按键和音频 Codec。它不重写显示屏或 LED 接口，使用框架提供的 `NoDisplay` 和 `NoLed`。

板级专用 Codec 复用项目现有 ES8311、ES7210 和 I2S 驱动模式，但明确限定 ES7210 输入：

- 启用 MIC1、MIC2 和 MIC3；
- ES7210 TDM 数据按 `MIC1、MIC3参考、MIC2`（`MRM`）排列；
- 唤醒和对话提取通道 0/1，以 `MR` 格式送入单麦 AFE；
- 定位提取通道 0/2，并将极性相反的 MIC2 软件反相后送入 ESP-SR
  SRP-PHAT，输出左负右正的 `-90°～+90°` 声源方向；播放期间暂停结果。

采用板级专用 Codec，避免改变公共 `BoxAudioCodec` 的默认通道假设，从而不影响现有开发板。

## 注册与构建配置

在 `main/Kconfig.projbuild` 中新增 `BOARD_TYPE_HEAD`，选项文字为“智慧头”，仅在 ESP32-S3 目标下可选。将该板型加入 `USE_DEVICE_AEC` 依赖白名单。

在 `main/CMakeLists.txt` 中将 `CONFIG_BOARD_TYPE_HEAD` 映射到 `BOARD_TYPE` 值 `head`。

`main/boards/head/config.json` 使用 `esp32s3` 目标，构建名称为 `head`，配置 16 MB Flash、16 MB 分区表和 `CONFIG_USE_DEVICE_AEC=y`。项目的 ESP32-S3 默认配置已经启用 8 MB Octal PSRAM。

MIC1/MIC2/MIC3 使用 TDM slot 0/1/2。项目最低使用 ESP Codec Dev 1.5.11，
以确保全双工模式下 RX 掩码不会被错误应用到 STD TX。

## 交互

- 启动阶段单击 Boot：进入 Wi-Fi 配网模式。
- 其他状态单击 Boot：切换对话状态。
- 空闲状态双击 Boot：在关闭和设备端 AEC 之间切换。

由于没有显示屏，状态提示通过日志和音频行为体现。

## 错误处理

I2C、I2S 和 Codec 初始化沿用项目现有的 `ESP_ERROR_CHECK`/断言策略，初始化失败时立即暴露错误，不在板级代码中静默降级。AEC 只有在音频处理器、PSRAM和 `BOARD_TYPE_HEAD` 条件满足时才可配置。

## 验证

1. 配置解析：确认 `BOARD_TYPE_HEAD` 可见且映射到 `main/boards/head/`。
2. 编译：使用 `python scripts/release.py head` 完成 ESP32-S3 发布构建。
3. 静态检查：确认编译命令包含 `BOARD_TYPE="head"`，且 AEC 音频处理器被编入。
4. 硬件启动：确认 ES8311、ES7210 均能在 I2C 总线上初始化。
5. 音频：确认扬声器播放正常，输入通道依次为 MIC1、MIC3、MIC2。
6. AEC：播放 TTS 时确认参考通道来自 MIC3，并比较启用/关闭 AEC 时的回声抑制效果。
7. 按键：确认 Boot 单击和双击行为符合设计。
8. 定位：在正前方 ±60° 范围内验证定位误差不超过 ±15°，播放期间无新结果。

## 不在本次范围

- 双麦克风波束成形或 MIC1/MIC2 混音（定位仅旁路读取，不修改对话音频）
- 显示屏、LED 或其他 UI 外设
- 修改公共 `BoxAudioCodec` 的通道行为
