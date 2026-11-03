# magpie 桌面宠物

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![build](https://github.com/xuncv/magpie-pet/actions/workflows/build.yml/badge.svg)](https://github.com/xuncv/magpie-pet/actions/workflows/build.yml)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)
![C11](https://img.shields.io/badge/C-11-informational)

跟着 magpie 网关的实时请求变动作的桌面宠物：无边框、逐像素透明、永远置顶、不占任务栏、
可拖动、透明处点击穿透。

单进程 C 实现，Windows / Linux(X11) / macOS 三套窗口后端，无第三方运行时依赖。
可执行文件 230 KB，常驻内存 ~21 MB，空闲 CPU ~2%。

<p>
  <img src="pets/deepseek-girl/preview/idle.gif" width="128" alt="idle">
  <img src="pets/deepseek-girl/preview/think.gif" width="128" alt="思考">
  <img src="pets/deepseek-girl/preview/work.gif" width="128" alt="干活">
  <img src="pets/deepseek-girl/preview/done.gif" width="128" alt="完成">
</p>

任何 Agent（Claude Code / Codex / pi / OpenCode…）经过 magpie 网关的每次调用，
都会变成它的动作和气泡。

## 特性

- **姿态跟着会话表走**：有活干 → 思考/干活；有没关掉的完成项 → 完成（一直播到你点 ×）；
  列表空了 → 等待。
- **气泡是通知，不是状态**：每条消息带寿命，到点自己淡出（淡入 140ms / 淡出 280ms + 向上飘 6px）。
  进行中常驻、完成停 3.5s、失败停 9s（期间不被别的消息挤掉）、菜单提示 2s 左右。
- **两行气泡**：状态行（色点 + agent + 状态）+ 内容行（当前请求 / 最近结果）。
  状态行跟着消息走，所以第二条会话的完成提示不会顶着第一条会话的名字。
- **会话列表**：悬停宠物弹出，贴着宠物、宠物原地不动；点行固定，点行尾 × 关闭。
- **右键菜单**：置顶 / 点击穿透 / 调整大小 150%~50% / 重载素材 / 回到右下角 / 退出。

## 架构

```
Agent 请求
   │
   ▼  HTTP
magpie 网关 (127.0.0.1:3425)
   │  异步网关请求钩子（跑在 magpie 的 Bun 插件宿主里）
   ▼
pet.hooks.js ──▶ 127.0.0.1:8789/pet.txt
                    │  纯文本 key=value，桌宠 500ms 轮询
                    ▼
              pet-c/pet.exe   原生窗口 + 软件合成
                    └─ pets/<名字>/spritesheet.png + layout.txt
```

状态机内联在插件宿主里（`pet-core.mjs`），除了桌宠窗口本身不需要额外进程。

## 快速开始

### 依赖

| 平台 | 需要 |
|---|---|
| Windows | [MinGW-w64](https://www.mingw-w64.org/) gcc |
| Linux | `build-essential libx11-dev libxext-dev` |
| macOS | Xcode 命令行工具 |

外加 magpie 本体（提供网关和插件宿主）。

### 编译

```powershell
pet-c\build.bat              # Windows → pet-c\pet.exe
make -C pet-c linux          # Linux   → pet-c/pet
make -C pet-c macos          # macOS   → pet-c/pet
```

### 装进 magpie

把本目录加进 `~/.config/magpie/plugins.json`：

```json
{
  "plugins": [
    { "spec": "D:\\path\\to\\magpie-pet" }
  ]
}
```

重启 magpie，桌宠自动出现。插件宿主加载 `pet.hooks.js` 时会拉起
`pet.exe --parent <宿主 pid> --lock pet.lock`，宿主退出桌宠自己退出，重复启动靠锁去重。

不装 magpie 单独看效果：`.\启动桌宠.bat`

### 命令行

| 参数 | 说明 |
|---|---|
| `--sheet <png>` | 图集路径；同目录有 `layout.txt` 就用它，否则按内置 Codex 9 轨规格读 |
| `--port <n>` | 事件服务端口，默认 8789 |
| `--pos <file>` | 位置文件（`x,y,scale`），空则落在右下角 |
| `--lock <file>` | 单实例锁 |
| `--parent <pid>` | 父进程消失时自动退出 |

## 皮肤

当前皮肤 `pets/deepseek-girl`：4 条动作轨，单元格 128×179，图集 6.8 MB，每轨一轮 4 秒。

| 姿态 | 动作轨 | 图集行 |
|---|---|---|
| 干活 | `running` | 2（抱笔记本） |
| 思考 | `review` | 1（头顶 ?） |
| 完成 | `jumping` | 3（举 ✓ 牌） |
| 等待 | `waiting` | 0（复用 idle） |

皮肤没有 `failed` 轨时，失败归到完成动作，失败原因看气泡和列表。

### 自己做一套

拍四段绿幕视频（站着 / 头顶 ? / 抱笔记本 / 举 ✓），放进 `sources/video/`，然后：

```powershell
.\tools\video-bake.ps1                      # 输出 pets\deepseek-girl（含 GIF 预览）
.\tools\video-bake.ps1 -Cell 160x225        # 换单元格尺寸
.\tools\video-bake.ps1 -Every 1             # 不隔帧（帧数翻倍）
```

脚本负责抠绿幕、裁到并集包围盒、缩到单元格、隔帧取样。参数和注意事项写在脚本头部。
需要 ffmpeg：在 PATH 里，或设置 `FFMPEG=<ffmpeg 路径>`。

其它素材格式：Codex Custom Pet 包用 `tools/import-pet.ps1` 导入；现成 PNG 图集补帧用
`tools/interp-bake.ps1`。

## 协议

桌宠每 500ms `GET /pet.txt`，纯文本：

```
seq=12
state=busy
phase=tool
active=1
agent=claude
model=opus-4.6
round=3
bubble=收到：「重构路由表」
bubble_kind=live
bubble_left=0
bubble_agent=claude
bubble_state=thinking
s=<id>|<agent>|<model>|<state>|<round>|<elapsed>|<tin>|<tout>|<credits>|<query>
```

`s=` 每行一个会话，`state` ∈ `thinking` / `tool` / `done` / `failed`。

`bubble` 是「有寿命的通知」，不是常驻状态：

| 字段 | 说明 |
|---|---|
| `bubble` | 气泡内容；空 = 没有要显示的消息（桌宠会淡出收掉） |
| `bubble_kind` | `live` 进行中（不过期）/ `ok` 完成 3.5s / `err` 失败 9s / `info` 提示 2.6s |
| `bubble_left` | 还剩多少毫秒淡出；`0` = 不过期 |
| `bubble_agent` / `bubble_state` | 这条消息属于谁；桌宠用它画状态行，空则不画 |

桌宠侧不依赖服务端“记得清空”：同一段文字反复轮询到不会续命，`bubble_left` 到点自己淡出。
旧内核只发 `bubble=` 也能用（有人正在跑就按 `live` 处理，否则给 3.5s 寿命）。

| 端点 | 用途 |
|---|---|
| `POST /event` | 投递事件（钩子用） |
| `GET /dismiss?id=<会话 id>` | 关闭一个会话（桌宠点 × 时发） |
| `GET /events` | SSE 实时推送 |
| `GET /health` | 健康检查 |

## 配置

### 环境变量

| 变量 | 作用 |
|---|---|
| `PET_PORT=8789` | 事件服务端口 |
| `PET_SHEET=<path>` | 换皮肤 |
| `PET_SCALE=1.0` | 启动缩放（1.0 = 图集原生像素） |
| `PET_SPEED=2.0` | 整体快慢放 |
| `PET_DEBUG=<path>` | 把切帧/事件写进日志 |
| `PET_USAGE_FILE=<path>` | 覆盖旁听的 `usage.jsonl` |
| `PET_NO_SPAWN=1` | 只要状态机、不弹窗口 |
| `PET_EXE=<path>` | 指定桌宠可执行文件位置 |

### 操作

| 操作 | 效果 |
|---|---|
| 左键拖 | 移动 |
| 左键点一下 | 播一次完成动作 |
| 悬停 | 展开会话列表（气泡让位） |
| 右键 | 弹出菜单（鼠标离开 5 秒自动关闭） |

尺寸和位置写进 `pet-pos.txt`，下次启动沿用。

## 已知限制

- 钩子有 1 秒预算，投递必须短超时 + 出错静默，不能拖慢真实请求；`onResponse` 尽力投递，可能丢
- magpie 自己的内部请求（会话标题、路由分类器）不触发钩子
- Linux/macOS 后端按规范实现但**未实机验证**：X11 没有气泡文字（不引入 freetype），
  Wayland 下 `override_redirect` + XShape 不生效（需要 `wlr-layer-shell`，未实现）
- 内存主要是图集（49×4 格 × 128×179 × 4B ≈ 17.9 MB），要再降只能减帧数或缩格子

## 目录

```
magpie-pet/
├─ pet-c/                    桌宠本体（C，跨平台）
│  ├─ pet.c                  核心：图集/状态机/合成/HTTP 轮询/菜单
│  ├─ platform.h             后端抽象（21 个接口）
│  ├─ platform_win32.c       Windows（UpdateLayeredWindow）
│  ├─ platform_x11.c         Linux（ARGB visual + XShape）
│  ├─ platform_cocoa.m       macOS（NSWindow + hitTest:）
│  ├─ http_get.h             极简 HTTP/1.1 GET（含 chunked）
│  ├─ third_party/stb_image.h  PNG 解码（public domain）
│  └─ build.bat / Makefile
├─ pets/deepseek-girl/       当前皮肤 + GIF 预览
├─ tools/                    video-bake / interp-bake / import-pet / shot
├─ pet.hooks.js              magpie 钩子 + 内联事件服务
├─ pet-core.mjs              状态机 + HTTP 服务
├─ server.mjs                独立跑法入口（不装 magpie 时调试用）
├─ package.json              插件声明
└─ LICENSE                   MIT
```

## 许可

代码以 MIT 发布，见 [LICENSE](LICENSE)。

- `pet-c/third_party/stb_image.h` 是 public domain（见文件头）
- `pets/` 下的皮肤素材仅作演示随仓库提供，可自由替换成你自己的素材
