// magpie 异步网关请求钩子 —— 同时把「状态机 + HTTP 服务」内联进来。
//
// 这样桌宠只需要 1 个进程（pet-c/pet.exe），不再需要单独的 server.mjs 进程（省 ~104 MB）。
//
// 边界说明（为什么窗口本身不能内联）：
//   - 页面插件跑在 magpie 窗口里的沙盒 iframe（sandbox="allow-scripts"、connect-src 'none'），
//     碰不到桌面，也创建不了置顶透明窗口
//   - 网关中间件跑在 magpie 进程的 moejs 里，没有 fetch / 定时器 / Node API
//   - 只有「异步网关请求钩子」跑在 Bun 插件宿主里，有完整 Node/Bun API → 能自己起 HTTP 服务
//   所以：状态机可以内联，原生置顶透明窗口必须单独进程。（magpie 没有提供创建 OS 窗口的扩展点）

import { createPetCore } from "./pet-core.mjs";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const PORT = Number(process.env.PET_PORT ?? 8789);
const TIMEOUT_MS = 250;
const HERE = dirname(fileURLToPath(import.meta.url));

/* ---------------- 伴随 magpie 启动 ----------------
 * 插件宿主在启动时就会 import 这个文件（不是等第一个请求），
 * 所以模块加载 = magpie 已就绪，正好把窗口拉起来。
 *
 *   --parent <本进程 pid>  pet.exe 每秒查一次，宿主没了就自己退出
 *   --lock pet.lock        单实例，热重载/重复加载不会出现两只
 *
 * 关掉：PET_NO_SPAWN=1；换可执行文件：PET_EXE=<path>。 */
function launchPet() {
  if (process.env.PET_NO_SPAWN === "1") {
    console.log("[pet] PET_NO_SPAWN=1，不启动窗口");
    return;
  }
  const exe = process.env.PET_EXE
    || join(HERE, "pet-c", process.platform === "win32" ? "pet.exe" : "pet");
  if (!existsSync(exe)) {
    console.warn(`[pet] 找不到 ${exe}，先跑 pet-c/build.bat 或 pet-c 下的 make`);
    return;
  }
  const args = [
    "--sheet", process.env.PET_SHEET || "pets/deepseek-girl/spritesheet.png",
    "--host", "127.0.0.1",
    "--port", String(PORT),
    "--pos", "pet-pos.txt",
    "--lock", "pet.lock",
    "--parent", String(process.pid),
  ];
  try {
    const child = spawn(exe, args, { cwd: HERE, stdio: "ignore", windowsHide: true });
    child.on("error", (e) => console.warn(`[pet] 启动失败: ${e.message}`));
    child.unref();          /* 不拖住宿主退出；回收交给 --parent 看门狗 */
    console.log(`[pet] 已拉起 ${exe} (pid ${child.pid})，监听宿主 ${process.pid}`);
  } catch (e) {
    console.warn(`[pet] spawn 异常: ${e?.message ?? e}`);
  }
}

// 内联核心：模块加载时就起服务
const core = createPetCore({ inline: true, port: PORT });
core.listen();

// 端口就绪后再拉窗口（pet.exe 启动后立刻就会轮询 /pet.txt）
launchPet();

const DEFAULT_SERVER = `http://127.0.0.1:${PORT}/event`;
const seen = new Map();

function endpoint(options) {
  const url = options && typeof options.serverUrl === "string" ? options.serverUrl : DEFAULT_SERVER;
  return url || DEFAULT_SERVER;
}

function baseEvent(kind, ctx) {
  return {
    kind,
    ts: Date.now(),
    session: (ctx?.session ?? "") || "_",
    agent: ctx?.agent ?? "",
    protocol: ctx?.protocol ?? "",
    model: ctx?.model ?? "",
    stream: !!ctx?.stream,
    path: ctx?.path ?? "",
    cwd: ctx?.cwd ?? "",
    query: (ctx?.query ?? "").slice(0, 200),
  };
}

function summarizeBody(body) {
  const out = {};
  try {
    if (!body || typeof body !== "object") return out;
    if (Array.isArray(body.tools)) out.tools = body.tools.length;
    const msgs = body.messages ?? body.input;
    if (Array.isArray(msgs)) {
      out.turns = msgs.length;
      const last = msgs[msgs.length - 1];
      if (last && typeof last === "object") {
        const role = last.role ?? last.type ?? "";
        if (role === "tool" || role === "tool_result" || role === "function") out.toolResult = true;
        if (Array.isArray(last.content)) {
          out.toolResult =
            out.toolResult ||
            last.content.some?.((c) => c && (c.type === "tool_result" || c.type === "tool_use"));
        }
      }
    }
    if (body.max_tokens || body.max_output_tokens) out.maxTokens = body.max_tokens ?? body.max_output_tokens;
  } catch {
    // 摘要失败不影响请求
  }
  return out;
}

function stopOf(reply) {
  if (!reply || typeof reply !== "object") return undefined;
  if (reply.stop_reason) return reply.stop_reason;
  if (reply.finish_reason) return reply.finish_reason;
  const c = Array.isArray(reply.choices) ? reply.choices[0] : null;
  if (c && typeof c === "object") return c.finish_reason ?? c.stop_reason;
  return undefined;
}

// 优先走进程内（零开销）；只有显式配了外部 serverUrl 才走 HTTP
function deliver(ev, options, signal) {
  const url = endpoint(options);
  const inline = url === DEFAULT_SERVER && !(options && options.forceHttp);
  if (inline) {
    try {
      core.push(ev);
    } catch {
      // 不能让桌宠的问题影响真实请求
    }
    return Promise.resolve();
  }
  return postOut(url, ev, signal);
}

async function postOut(url, payload, signal) {
  try {
    const timeout = AbortSignal.timeout?.(TIMEOUT_MS);
    const sig = timeout && signal ? AbortSignal.any([signal, timeout]) : (timeout ?? signal);
    await fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(payload),
      signal: sig,
    });
  } catch {
    // 桌宠没开、端口没监听、超时 —— 一律静默
  }
}

export async function onRequest(body, ctx) {
  try {
    const session = (ctx?.session ?? "") || "_";
    const meta = summarizeBody(body);
    const prev = seen.get(session);
    const round = prev ? prev.seq + 1 : 1;
    seen.set(session, { seq: round, startedAt: Date.now() });
    if (seen.size > 200) {
      const oldest = [...seen.entries()].sort((a, b) => a[1].startedAt - b[1].startedAt)[0];
      seen.delete(oldest[0]);
    }
    await deliver({ ...baseEvent("start", ctx), round, ...meta }, ctx?.options, ctx?.signal);
  } catch {
    // 保留原请求
  }
  return undefined; // 不修改请求
}

export async function onResponse(reply, ctx) {
  try {
    const session = (ctx?.session ?? "") || "_";
    const prev = seen.get(session);
    await deliver(
      {
        ...baseEvent("end", ctx),
        round: prev?.seq ?? 1,
        elapsedMs: prev ? Date.now() - prev.startedAt : undefined,
        usage: reply && typeof reply === "object" ? (reply.usage ?? undefined) : undefined,
        stop: stopOf(reply),
      },
      ctx?.options,
      ctx?.signal,
    );
  } catch {
    // 尽力投递
  }
}

// 便于调试/自测
export const _internal = { core };
