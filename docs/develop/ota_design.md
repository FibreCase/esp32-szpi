# OTA 设计与实现

## 配置和入口

- OTA 由 `CONFIG_SZPI_OTA_ENABLED` 控制，默认启用。
- `CONFIG_SZPI_OTA_URL` 是完整 ESP-IDF 应用镜像 URL，默认 `http://localhost/ota/firmware.bin`。可在 `idf.py menuconfig` 的 **SZPI application** 中更改。
- 用户从 Settings → About 点击 `Install update` 手动发起。该入口要求 Wi-Fi 已拿到网络地址；UI 显示连接、下载进度、失败错误码或重启状态。
- Linux 模拟器只展示 About 页面，不运行 OTA。

## 下载与切换

`szpi_app` runtime 为 OTA 创建独立静态任务及状态互斥锁。UI 回调只提交请求，不执行网络或 Flash 操作。OTA 任务通过 `esp_https_ota` 下载到 ESP-IDF 选择的非运行 OTA 分区，检查镜像描述中的项目名，等待传输完成并由 OTA API 校验镜像后调用 finish。失败则 abort 部分镜像，保留当前启动槽并发布错误；成功后更新状态并重启。

升级 URL 可使用 HTTP 或 HTTPS。HTTPS 配置使用 ESP x509 证书 bundle。HTTP 不提供服务器身份认证或传输加密，因此生产部署应改用 HTTPS；固件真实性还依赖 Secure Boot 等平台配置，本项目当前未启用 Secure Boot。

## 试运行和回滚

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 已启用。新分区首次启动时，启动装配等待 UI 完成首帧显示，最多 30 秒；成功后调用 `esp_ota_mark_app_valid_cancel_rollback()`。核心启动失败或等待超时会调用 `esp_ota_mark_app_invalid_rollback_and_reboot()`。若试运行期间在健康确认前掉电或复位，bootloader 会按回滚策略恢复先前有效镜像。

OTA 仅更新应用镜像，不更新 bootloader 或分区表。要在已有设备启用 bootloader 回滚，需先通过串口完整烧录一次启用了 rollback 的 bootloader、分区表和应用；本次实现未对设备执行烧录。

## 部署限制

默认 URL 中的 `localhost` 指设备自身，而不是开发电脑。板上 OTA 前应在 `menuconfig` 将 URL 改成设备可路由访问的服务器地址，并在该路径提供与当前芯片目标兼容的完整应用 `.bin` 镜像。镜像大小必须适配单个 8128 KiB OTA 槽。当前实现会拒绝项目名不匹配的镜像；版本策略、签名验证、HTTP 鉴权和下载断点续传尚未实现。

配置或源码变更须执行 ESP-IDF 6.1 `idf.py build`。构建通过只证明固件可编译；URL 服务端、Wi-Fi 网络、下载中断处理和实际双槽回滚仍需板上验证。
