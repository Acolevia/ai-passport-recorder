# Folo AI Passport C3 Recorder

把 Folo AI Passport C3 变成一支离线优先的录音笔。录音始终先保存到设备 Flash；配置家庭/办公室 Wi-Fi 和服务器后，设备会在空闲时自动上传，断网或服务器不可用不会影响录音，恢复连接后从中断位置续传。

## 功能

- 16 kHz 单声道、Opus 24 kbps VBR（20 ms 帧），约 3.1 KB/s
- 6.38 MiB FatFS 录音分区，约可录 35 分钟
- 断电恢复未完成录音，本机播放、音量调节、电量和存储用量显示
- 工牌上选择和删除录音
- 独立 Wi-Fi 热点网页：试听 Ogg/Opus、下载 PCM WAV、删除录音
- 网页配置上网 Wi-Fi、上传服务器 URL 和访问令牌
- 屏幕显示 `NET NOT SET`、`WIFI OFFLINE`、`SERVER OFFLINE`、`UPLOADING`、`SERVER ONLINE` 等状态
- 后台断点续传；录音、回放和热点导出期间自动暂停上传
- 无第三方依赖的 Python 上传服务，提供 Docker Compose 一行部署

设备上传的是原始 `.FRC` 文件。它由 32 字节头和带长度前缀的 Opus 帧组成，既节省 Flash，也能在意外断电后扫描到最后一个完整帧。热点网页试听时实时封装为 Ogg/Opus，下载时实时解码成标准 16-bit PCM WAV，不会在设备上生成第二份文件。

## 使用方法

### 1. 部署服务器

在一台装有 Docker 和 Docker Compose 的 Linux 服务器上：

```sh
./deploy.sh
```

脚本首次运行会生成 `.env`、创建随机上传令牌并启动服务。把 `.env` 中的 `FOLO_UPLOAD_TOKEN` 记下来。数据保存在 Docker 卷 `folo-recordings`，容器升级不会删除录音。默认端口为 `8080`；生产环境建议在它前面配置带 HTTPS 的 Caddy、Nginx 或其他反向代理。

已有令牌时也可以直接一行启动：

```sh
FOLO_UPLOAD_TOKEN='replace-with-a-long-random-token' docker compose up -d --build
```

健康检查：`http://服务器地址:8080/health`。更多接口和备份说明见 [server/README.md](server/README.md)。

### 2. 配置工牌

1. 长按下键开启 `Folo-Recorder-XXXX` 热点。
2. 输入屏幕显示的 8 位随机数字密码。
3. 手机访问 `http://192.168.4.1`。
4. 在“Automatic server upload”中填写路由器 Wi-Fi、服务器 URL 和 `FOLO_UPLOAD_TOKEN`。
5. 保存后长按下键退出热点；设备会在后台测试连接并上传现有录音。

服务器 URL 可以是 `http://IP:8080`，也可以是正式的 `https://录音域名`。公网使用时强烈建议 HTTPS；Wi-Fi 密码和上传令牌保存在设备 NVS 中，不会显示在热点页面。

### 3. 按键

| 操作 | 功能 |
| --- | --- |
| 确认键短按 | 开始录音；录音中再次短按则结束并保存 |
| 确认键长按 | 播放选中的录音；播放中再次长按则停止 |
| 上 / 下短按 | 选择上一条 / 下一条录音 |
| 播放中上 / 下短按 | 音量增加 / 减少 10%，重启后保留 |
| 上键长按 | 删除选中录音；确认键删除，上/下键取消 |
| 下键长按 | 开启或关闭 Wi-Fi 导出与配置页面 |

## 离线与续传行为

录音停止后文件已经安全保存在本地，不依赖服务器响应。后台任务每 30 秒尝试一次：连接 Wi-Fi、检查带认证的 `/api/v1/status`、查询服务器已收到的字节偏移，然后从该位置继续 `PUT`。上传成功后本地文件仍会保留，只有用户手动删除才会释放空间。

设备使用 MAC 地址作为服务器目录名，并用永久递增的录音序号命名文件，避免本地删除后新录音覆盖服务器上的旧文件。

## 构建与刷机

推荐 ESP-IDF 5.5.3（清单兼容 5.5～6.0）：

```sh
FOLO_RECORDER_IDF_PATH=/path/to/esp-idf ./build.sh
```

输出：

- `build/folo_recorder_c3.bin`：应用镜像
- `build/folo_recorder_c3_0x0.bin`：从 `0x0` 刷入的完整镜像

刷完整镜像：

```sh
esptool.py --chip esp32c3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash 0x0 \
  build/folo_recorder_c3_0x0.bin
```

固件采用单 factory app：应用 1.56 MiB，录音 6.38 MiB。完整镜像会替换分区表；从其他布局切换时需先整片擦除，原录音和设置不会保留。

## 开发

```sh
python -m unittest server/test_server.py -v
docker compose config
idf.py build
```

CI 会执行服务端测试和 ESP32-C3 固件构建。编译通过不等于硬件验证；发布前仍应检查真实设备上的录音、播放、按键、热点页面、Wi-Fi 重连、HTTP/HTTPS 上传和中途断网续传。

本项目沿用上游项目的 [MIT License](LICENSE)。
