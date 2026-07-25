# 智慧头（head）

主控为 ESP32-S3 N16R8，无显示屏、无 LED。音频输出使用 ES8311，
输入使用 ES7210：MIC1 为语音，MIC3 接 ES8311 OUTP/OUTN 作为 AEC
参考，MIC2/MIC4 不参与当前固件的采集。

## 构建

```powershell
python scripts/release.py head
```

设备端 AEC 默认启用。Boot 单击切换对话；启动阶段单击进入配网；
空闲状态双击切换设备端 AEC。

## 硬件验证

首次烧录后应确认：

1. ES8311、ES7210 均能正常初始化。
2. 扬声器和 PA GPIO42 工作正常。
3. 输入的两个通道依次为 MIC1 和 MIC3 回采。
4. MIC2/MIC4 不进入采集数据流。
5. 播放 TTS 时，开启 AEC 后 MIC1 中的扬声器回声显著降低。
