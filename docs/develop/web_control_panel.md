# Web 控制面板

固件在 Wi-Fi 驱动启动后提供端口 80 的 HTTP 服务。STA 联网后访问 `http://szpi.local/`（或设备 IPv4 地址）；hostname 以 NVS 实际值为准。当前只有 Overview 与 Settings 两个导航项，使用英文文案。

- `/`：运行时间、内部 RAM / PSRAM 可用量、网络、固件版本与运行 OTA 槽、相机预览状态 / FPS、SD 状态与容量。每 3 秒读取实际服务快照，不查询硬件驱动。
- `/settings`：hostname（保存后重启生效）、亮度 10–100%、扬声器音量 0–100%、麦克风增益 0–100%。通过既有应用服务应用并保存 NVS；不可用服务禁用对应控件。亮度由 UI owner 执行，HTTP 不调用 LVGL。响应 202 表示请求已接受，尚不代表硬件应用或 NVS 写入完成；写入故障由既有服务日志报告。
- `/setup`：独立配网页面，控制面板没有入口。仅进入设备 Network 的热点二维码页面才激活其会话/API；退出或超时停用。AP 根地址及 captive 探测地址跳转到 `/setup`。扫描选择行为保持原有规则。

## 构建

需要 Node.js 22.12+，前端依赖版本锁定在 `web/package-lock.json`。先生成前端，再构建固件：

```sh
cd web
npm ci
npm run build
cd ..
idf.py build
```

Vite 与 vite-plugin-singlefile 把 JS/CSS（包括独立配网页面）合并到 `web/dist/index.html`。CMake 用现有 Python 脚本生成确定性 gzip 并作为二进制嵌入 szpi_app；HTTP 设置 `Content-Encoding: gzip`。不需要 Flash 文件系统。`dist` 不提交；缺少文件时 CMake 明确报错。修改前端或原配网 HTML 后必须重新运行前端构建。

开发预览可运行 `npm run dev`，但它没有设备 API，不会伪造设备状态。后端验证须使用板上固件。

## 归属与接口

`szpi_http` 只拥有共享 HTTP transport、路由分发和配网回调生命周期；依赖方向为 szpi_app / szpi_wifi → szpi_http → esp_http_server。szpi_app/web_service 生成控制面板快照并桥接设置。szpi_wifi 保留 AP 会话、随机口令、候选凭据、DNS 和配网校验。

- `GET /api/device`：状态快照，无密码。
- `GET /api/control-session`：本次启动的随机控制令牌。
- `POST /api/settings`：`application/json`，`{"key":"brightness","value":50}`；key 可为 brightness / speaker / microphone，另支持 `{"key":"hostname","value":"szpi-next"}`。hostname 使用既有 1–32 字符 DNS 标签校验，保存成功才接受请求，仅写 NVS，不调用运行时 hostname 更新；下次启动读取并应用 DHCP / mDNS。`settings.hostname` 为保存值，顶层 `hostname` 为当前运行值，两者不同显示重启提示。请求带 `X-Control-Token`，限制 127 字节，拒绝无效范围 / 非整数 / 额外字段。
- 原配网接口统一为 `/api/setup/session`、`status`、`scan`、`connect`，保留 AP 本地地址、会话令牌和 origin 校验；STA 不允许配网 API。

只接受设备 IPv4 / hostname.local 的 Host；设置写入核对令牌与 Origin，不开放 CORS。控制令牌用于同源请求校验，不是账号登录；当前服务面向可信局域网，无 HTTPS 或账号系统。

HTTP 使用 ESP-IDF 内部依赖任务，栈 6144 字节，最多 3 个客户端，接收 / 发送超时 2 秒；不新增项目任务。静态 mutex 属于 transport、生命周期与服务器相同，串行保护配网回调；清除回调最多等待 4500ms，确保在途调用结束后再清理配网状态。设置请求体处理预算 4 秒。Wi-Fi owner 不持服务锁等待 transport。

## 验证

前端单文件构建通过（约 17.9kB，gzip 6.59kB）。桌面使用明确的示例状态检查两页导航、滑条编辑与保存失败反馈，以及 `/setup` 初始未选择网络、表单隐藏；另以示例的当前 / 保存 hostname 不同验证重启提示，并验证非法 hostname 在前端拒绝提交；不代表板上数据与持久化验收。

ESP-IDF 6.1 / esp32s3 固件构建通过。板上仍需验证 STA 的 IP / mDNS 访问、亮度和音量实际应用及重启恢复、AP captive 跳转、扫描 / 连接、快速退出配网和并发客户端。
