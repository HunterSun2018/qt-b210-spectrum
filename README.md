# QT6 B210 Spectrum

使用 `UHD` 从 USRP B210 采集 IQ 数据，使用 `FFTW3` 计算频谱，使用 `Qt6 Widgets` 绘制实时频谱和瀑布图，并通过 `CMake` 构建。

## 依赖

- Qt6 Widgets
- UHD
- FFTW3
- Boost
- ALSA
- CMake 3.21+
- C++17 编译器

Ubuntu 示例：

```bash
sudo apt install cmake g++ qt6-base-dev libuhd-dev libfftw3-dev libboost-all-dev libasound2-dev pkg-config
```

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

## 运行

```bash
./build/qt6_b210_spectrum
```

默认参数：

- 输入源：`USRP B210` 或 `Simulator`
- 频谱处理器：`FftProcessor` 或 `IqFftwProcessor`
- 解调：`Off`、`FM`、`AM`
- 静噪：`Squelch`
- 设备地址：空，交给 UHD 自动发现
- 采样率：`2e6`
- 中心频率：`2.45e9`
- 增益：`40`
- FFT 点数：`2048`

`Simulator` 模式会生成多音信号和少量噪声，可在无硬件时直接验证频谱和瀑布图。

选择 `FM` 或 `AM` 后，程序会从同一批 IQ 数据中实时解调并通过本机 ALSA 默认音频设备播放。当前实现包含：

- FM 去加重
- AM 与音频低通
- 分数步进重采样到 `48 kHz`
- 基于信号功率的静噪
- 导频辅助的立体声 FM 解码

广播收听场景下，中心频率应直接调到目标电台载频附近。`FM` 模式会优先尝试立体声解码，导频不足时会自动退化到近似单声道输出。

如果 B210 未连接或初始化失败，界面会显示错误信息。
