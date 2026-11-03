// 桌宠的状态机 + HTTP 服务（共享核心）
//
// 同一份代码有两种跑法：
//   1) 内联：被 pet.hooks.js import，跑在 magpie 的 Bun 插件宿主里 —— 不需要额外进程
//   2) 独立：被 server.mjs import，当成单独进程跑 —— 用于不启动 magpie 时的调试
//
// 端口被占用时不报错退出，只是不提供服务（例如内联已经占住了 8789）。
//
// 对外协议（/pet.txt，纯文本 key=value，一行一条）：
//   seq=12
//   state=busy|idle          宠物整体动画状态
//   phase=start|tool|end|error|cancel
//   active=2                 运行中的会话数
//   bubble=...               气泡文字（有生命周期，过期就是空）
//   bubble_kind=live|info|ok|err
//   bubble_left=<ms>         还剩多久淡出（0 = 常驻，live 用）
//   bubble_agent=...         气泡属于谁（画状态行用，空则不画）
//   s=<id>|<agent>|<model>|<state>|<round>|<elapsed>|<tin>|<tout>|<credits>|<query>
//   （按「失败 > 运行中 > 刚完成」排序，最多 8 条）

import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

// 气泡不是状态，是通知：每种消息有自己的停留时间，到点由桌宠淡出。
// live 不过期（进行中一直显示），ok/err 留够看清的时间。
const BUBBLE_TTL = { live: 0, info: 2600, ok: 3500, err: 9000 };
const BUBBLE_DEFAULT_TTL = 3500;
const MAX_KEEP = 12;           // 会话表最多留几条（超了删最旧的已结束）
const IDLE_WATCHDOG_MS = 180000;
const MAX_ROWS = 8;

const USAGE_FILE =
  process.env.PET_USAGE_FILE || path.join(os.homedir(), ".config", "magpie", "usage.jsonl");

// 截断到 n 个**字符**（不是字节/UTF-16 码元）：JS 的 slice 会把 emoji 的
// 代理对切开，到了 C 侧就变成乱码。Array.from 按码点切，安全。
const clip = (s, n) => {
  const a = Array.from(s);
  return a.length <= n ? s : a.slice(0, n).join("");
};

const clean = (s, n) =>
  clip(String(s ?? "").replace(/[|\r\n]+/g, " "), n);

export function createPetCore(options = {}) {
  const port = Number(options.port ?? process.env.PET_PORT ?? 8789);
  const host = options.host ?? process.env.PET_HOST ?? "127.0.0.1";
  const log = options.log ?? ((...a) => console.log(...a));

  /** @type {Map<string, {id:string, agent:string, model:string, query:string,
   *   state:string, round:number, startedAt:number, lastAt:number,
   *   elapsed:number, tin:number, tout:number, credits:number, finishedAt:number}>} */
  const sessions = new Map();

  const clients = new Set();
  let lastEvent = null;
  let seq = 0;

  /* 当前气泡：{ text, kind, key, at, ttl, agent, state }
   * key 是会话 id —— 同一会话的新消息直接覆盖自己；
   * 错误提示在 TTL 内不会被别的消息挤掉，免得失败原因一闪而过。 */
  let bubble = null;
  let bubbleWasAlive = false;
  // 初始快照：会话表是空的，就是等待态（以前写死 idle，Codex 多轨图集上看不到等待动作）
  let lastSnapshot = { state: "waiting", anim: "waiting", active: 0, bubble: null, phase: "", rows: [] };

  // ---------------- 会话表 ----------------
  function getSess(id, create) {
    let s = sessions.get(id);
    if (!s && create) {
      s = {
        id,
        agent: "",
        model: "",
        query: "",
        state: "thinking",
        round: 0,
        startedAt: Date.now(),
        lastAt: Date.now(),
        elapsed: 0,
        tin: 0,
        tout: 0,
        credits: 0,
        finishedAt: 0,
      };
      sessions.set(id, s);
    }
    return s;
  }

  const isRunning = (s) => s.state === "thinking" || s.state === "tool";

  // 失败 > 运行中（先开始的在前）> 刚完成（新的在前）
  function sortedRows() {
    const all = [...sessions.values()];
    const rank = (s) => (s.state === "failed" ? 0 : isRunning(s) ? 1 : 2);
    all.sort((a, b) => {
      const ra = rank(a), rb = rank(b);
      if (ra !== rb) return ra - rb;
      if (ra === 1) return a.startedAt - b.startedAt;
      return b.finishedAt - a.finishedAt;
    });
    return all.slice(0, MAX_ROWS);
  }

  // 宠物姿态完全由会话表决定：
  //   有活干 → 干活（tool=用工具，thinking=思考）
  //   没活但列表里还挂着没关的完成项 → 一直庆祝
  //   列表空了（任务都关掉了）→ 等待
  function overall() {
    const rows = [...sessions.values()];
    if (rows.length === 0) return { state: "waiting", phase: "" };
    if (rows.some((s) => s.state === "tool")) return { state: "busy", phase: "tool" };
    if (rows.some((s) => s.state === "thinking")) return { state: "busy", phase: "start" };
    if (rows.some((s) => s.state === "failed")) return { state: "failed", phase: "error" };
    if (rows.some((s) => s.state === "done")) return { state: "done", phase: "end" };
    return { state: "waiting", phase: "" };
  }

  function bubbleAlive(now = Date.now()) {
    if (!bubble) return false;
    return !bubble.ttl || now < bubble.at + bubble.ttl;
  }

  // 只改状态，不广播；调用方统一 emitState（一条事件只推一次）
  function say(text, kind = "info", key = "", meta = {}) {
    const t = String(text ?? "").replace(/\s+/g, " ").trim();
    if (!t) return;
    const now = Date.now();
    if (bubble && bubble.key !== key && bubble.kind === "err" && bubbleAlive(now) && kind !== "err")
      return;
    bubble = {
      text: t,
      kind,
      key,
      at: now,
      ttl: BUBBLE_TTL[kind] ?? BUBBLE_DEFAULT_TTL,
      agent: meta.agent ?? "",
      state: meta.state ?? "",
    };
  }

  // 会话被关掉时，属于它的气泡一起收掉
  function hush(key) {
    if (bubble && (!key || bubble.key === key)) bubble = null;
  }

  function snapshot() {
    const rows = sortedRows();
    const o = overall();
    const running = [...sessions.values()].filter(isRunning).length;
    const b = bubbleAlive() ? bubble : null;
    return {
      type: "state",
      state: running === 0 ? "idle" : "busy",
      anim: o.state,
      phase: o.phase,
      active: running,
      current: rows[0] ?? null,
      rows,
      bubble: b ? b.text : null,
      bubbleKind: b ? b.kind : "",
      bubbleLeft: b && b.ttl ? Math.max(0, b.at + b.ttl - Date.now()) : 0,
      bubbleAgent: b ? b.agent : "",
      bubbleState: b ? b.state : "",
      at: Date.now(),
    };
  }

  function broadcast(payload) {
    const line = `data: ${JSON.stringify(payload)}\n\n`;
    for (const res of clients) {
      try {
        res.write(line);
      } catch {
        clients.delete(res);
      }
    }
  }

  function emitState(extra = {}) {
    seq += 1;
    lastSnapshot = { ...snapshot(), ...extra, seq, at: Date.now() };
    broadcast({ ...lastSnapshot, type: "state" });
  }

  function petText() {
    const s = lastSnapshot;
    const cur = s.current ?? {};
    // 用实时气泡算，而不是 lastSnapshot：没人 emitState 时也不会一直挂着旧消息
    const b = bubbleAlive() ? bubble : null;
    const lines = [
      `seq=${s.seq ?? 0}`,
      `state=${s.anim ?? "idle"}`,
      `phase=${s.phase ?? ""}`,
      `active=${s.active ?? 0}`,
      `agent=${clean(cur.agent, 24)}`,
      `model=${clean(cur.model, 40)}`,
      `round=${cur.round ?? 0}`,
      `bubble=${clean(b?.text ?? "", 120)}`,
      `bubble_kind=${b?.kind ?? ""}`,
      `bubble_left=${b && b.ttl ? Math.max(0, b.at + b.ttl - Date.now()) : 0}`,
      `bubble_agent=${clean(b?.agent ?? "", 24)}`,
      `bubble_state=${clean(b?.state ?? "", 16)}`,
    ];
    for (const r of s.rows ?? []) {
      const el = isRunning(r) ? Date.now() - r.startedAt : r.elapsed || 0;
      lines.push(
        `s=${clean(r.id, 47)}|${clean(r.agent, 20)}|${clean(r.model, 32)}|${r.state}|${r.round}|${el}` +
          `|${r.tin}|${r.tout}|${r.credits.toFixed(2)}|${clean(r.query, 90)}`
      );
    }
    return lines.join("\n") + "\n";
  }

  // 气泡文案 + 类型。状态/agent 放到 bubble_agent/bubble_state 里走结构化字段，
  // 内容行只放「发生了什么」，避免两行信息重复。
  // 进行中的文案不带秒数：每 250ms 变一次宽度会让气泡一直抖。
  function bubbleFor(s) {
    if (s.state === "failed") return { text: s.err || "未知错误", kind: "err" };
    if (s.state === "done") {
      const p = [];
      if (s.elapsed) p.push(s.elapsed < 1000 ? `${s.elapsed}ms` : `${(s.elapsed / 1000).toFixed(1)}s`);
      const tok = s.tin + s.tout;
      if (tok) p.push(`${tok} tok`);
      return { text: p.length ? `用时 ${p.join(" · ")}` : "搞定！", kind: "ok" };
    }
    if (s.state === "tool") return { text: s.query ? clip(s.query, 28) : "正在处理…", kind: "live" };
    if (s.query) return { text: `收到：「${clip(s.query, 28)}」`, kind: "live" };
    return { text: "思考中…", kind: "live" };
  }

  function sayFor(s) {
    const b = bubbleFor(s);
    say(b.text, b.kind, s.id, { agent: s.agent, state: s.state });
  }

  // ---------------- 事件入口 ----------------
  // 没有 session 的事件统一归到固定哨兵：start/end 能配对，桌宠拿到的 id 也非空，
  // 能像普通会话一样点 × 关掉（空 id 在 C 侧会被当成无效参数直接丢掉）。
  const sidOf = (ev) => ev?.session || "_";

  function onStart(ev) {
    const id = sidOf(ev);
    const s = getSess(id, true);
    s.agent = ev.agent || s.agent;
    s.model = ev.model || s.model;
    s.query = ev.query || s.query;
    s.round = ev.round ?? (s.round || 1);
    s.state = "thinking";
    s.startedAt = Date.now();
    s.lastAt = s.startedAt;
    s.finishedAt = 0;
    sayFor(s);
    emitState();
  }

  function onEnd(ev) {
    const id = sidOf(ev);
    let s = sessions.get(id);
    if (!s) {
      /* 没收到 start 的 end：带身份信息才补一行，纯匿名事件不造幽灵行 */
      if (!ev.agent && !ev.model && !ev.query) return;
      s = getSess(id, true);
    }
    if (!s.agent && ev.agent) s.agent = ev.agent;
    if (!s.model && ev.model) s.model = ev.model;
    if (!s.query && ev.query) s.query = ev.query;
    const elapsed = ev.elapsedMs ?? Date.now() - s.startedAt;
    const u = ev.usage ?? {};
    const out = u.output_tokens ?? u.outputTokens ?? u.completion_tokens ?? 0;
    const inp = u.input_tokens ?? u.inputTokens ?? u.prompt_tokens ?? 0;

    const stop = String(ev.stop ?? "");
    const toolStop =
      stop === "tool_calls" || stop === "tool_use" || stop === "function_call" || stop === "tool_call";
    if (toolStop) {
      s.state = "tool";
      s.startedAt = Date.now();      // 下一轮从新计时
      s.elapsed = elapsed;
      if (inp) s.tin += inp;
      if (out) s.tout += out;
      sayFor(s);
      emitState();
      return;
    }

    s.state = "done";
    s.elapsed = elapsed;
    s.finishedAt = Date.now();
    if (inp) s.tin += inp;
    if (out) s.tout += out;
    sayFor(s);
    emitState();
    // 气泡的寿命写在消息里（bubble_left），桌宠自己淡出；这里不再 setTimeout
  }

  function onError(ev) {
    const id = sidOf(ev);
    let s = sessions.get(id);
    if (!s) {
      if (!ev.agent && !ev.model && !ev.query) return;
      s = getSess(id, true);
    }
    if (!s.agent && ev.agent) s.agent = ev.agent;
    if (!s.model && ev.model) s.model = ev.model;
    if (!s.query && ev.query) s.query = ev.query;
    s.state = "failed";
    s.err = `${describeError(Number(ev.code) || 0, ev.err)}${ev.code ? " " + ev.code : ""}`;
    s.finishedAt = Date.now();
    sayFor(s);
    emitState();
  }

  function onCancel(ev) {
    const id = sidOf(ev);
    sessions.delete(id);
    hush(id);
    say("已取消", "info", id, { agent: ev.agent || "", state: "" });
    emitState();
  }

  // ---------------- 错误/取消：旁听 magpie 的 usage.jsonl ----------------
  const seenRids = new Set();
  let usageStarted = false;

  function describeError(status, err) {
    const e = String(err || "").toLowerCase();
    if (status === 429 || e.includes("rate limit") || e.includes("too many")) return "限流";
    if (status === 401 || status === 403) return "登录/权限";
    if (status === 404) return "模型不可用";
    if (status === 408 || e.includes("timeout") || e.includes("deadline")) return "超时";
    if (status >= 500) return "服务端";
    if (status === 400) return "请求被拒";
    return "失败";
  }

  function readUsageTail(maxBytes = 96 * 1024) {
    let fd;
    try {
      const st = fs.statSync(USAGE_FILE);
      const start = Math.max(0, st.size - maxBytes);
      const len = st.size - start;
      const buf = Buffer.alloc(len);
      fd = fs.openSync(USAGE_FILE, "r");
      fs.readSync(fd, buf, 0, len, start);
      return buf.toString("utf8").split("\n");
    } catch {
      return [];
    } finally {
      if (fd !== undefined) try { fs.closeSync(fd); } catch {}
    }
  }

  function scanUsage() {
    let changed = false;
    for (const line of readUsageTail()) {
      if (!line || line[0] !== "{") continue;
      let row;
      try {
        row = JSON.parse(line);
      } catch {
        continue;
      }
      const rid = row.rid || row.route_id;
      if (!rid || seenRids.has(rid)) continue;
      seenRids.add(rid);
      if (seenRids.size > 400) seenRids.delete(seenRids.values().next().value);
      if (!usageStarted) continue;
      if (row.agent === "magpie" || row.kind || row.source) continue;
      const ep = String(row.ep || "");
      if (ep && !ep.startsWith("/v1/")) continue;

      const status = Number(row.status) || 0;
      const sid = row.session || row.native_session || "";

      if (status === 499 || String(row.err || "").toLowerCase().includes("canceled")) {
        if (sid && sessions.has(sid)) { sessions.delete(sid); hush(sid); changed = true; }
        continue;
      }
      if (status >= 400) {
        if (sid) {
          const s = getSess(sid, true);
          s.state = "failed";
          s.err = describeError(status, row.err);
          s.finishedAt = Date.now();
          sayFor(s);            // 旁听到的失败也弹一次气泡，别只靠钩子
          changed = true;
        }
        continue;
      }
      // 成功：补 tokens / 费用（钩子拿不到这些）
      if (sid && sessions.has(sid)) {
        const s = sessions.get(sid);
        const before = s.tin + s.tout;
        s.tin += Number(row.in) || 0;
        s.tout += Number(row.out) || 0;
        s.credits += Number(row.credits) || 0;
        if (s.tin + s.tout !== before) changed = true;
      }
    }
    if (changed) emitState({});
  }

  let usageTimer = null;
  function startUsageWatcher() {
    scanUsage();
    usageStarted = true;
    usageTimer = setInterval(() => { try { scanUsage(); } catch {} }, 700);
    if (usageTimer.unref) usageTimer.unref();
  }

  // 清理：已结束的行不自动消失（用户手动点 × 关掉），
  // 只处理三件事：卡死的运行中会话、总量超限时删最旧的已结束行、气泡到点
  const sweeper = setInterval(() => {
    const now = Date.now();
    let changed = false;
    for (const [id, s] of sessions) {
      if (isRunning(s) && now - s.startedAt > IDLE_WATCHDOG_MS) {
        sessions.delete(id);
        hush(id);
        changed = true;
      }
    }
    if (sessions.size > MAX_KEEP) {
      const finished = [...sessions.entries()]
        .filter(([, s]) => !isRunning(s))
        .sort((a, b) => a[1].finishedAt - b[1].finishedAt);
      let over = sessions.size - MAX_KEEP;
      for (const [id] of finished) {
        if (over-- <= 0) break;
        sessions.delete(id);
        hush(id);
        changed = true;
      }
    }
    // 气泡过期时推一次状态，SSE 客户端才能把最后那条抹掉
    const alive = bubbleAlive(now);
    if (bubbleWasAlive && !alive) { bubble = null; changed = true; }
    bubbleWasAlive = alive;
    if (changed) emitState();
  }, 1000);
  if (sweeper.unref) sweeper.unref();

  // ---------------- HTTP ----------------
  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url ?? "/", `http://${host}:${port}`);

    if (req.method === "GET" && url.pathname === "/events") {
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
        connection: "keep-alive",
        "access-control-allow-origin": "*",
      });
      res.write(`data: ${JSON.stringify(snapshot())}\n\n`);
      clients.add(res);
      const ping = setInterval(() => { try { res.write(": ping\n\n"); } catch {} }, 15000);
      if (ping.unref) ping.unref();
      req.on("close", () => { clearInterval(ping); clients.delete(res); });
      return;
    }

    if (req.method === "GET" && url.pathname === "/pet.txt") {
      res.writeHead(200, {
        "content-type": "text/plain; charset=utf-8",
        "cache-control": "no-store",
        "access-control-allow-origin": "*",
      });
      res.end(petText());
      return;
    }

    if (req.method === "GET" && url.pathname === "/health") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({
        ok: true,
        inline: !!options.inline,
        sessions: sessions.size,
        seq,
        last: lastEvent,
      }));
      return;
    }

    if (req.method === "GET" && url.pathname === "/dismiss") {
      /* 桌宠点 × 时调这里，把会话从表里拿掉（只影响显示，不影响真实请求） */
      const id = url.searchParams.get("id") || "";
      const had = sessions.delete(id);
      hush(id);
      if (had) emitState();
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, removed: had }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/event") {
      let raw = "";
      for await (const chunk of req) {
        raw += chunk;
        if (raw.length > 256 * 1024) break;
      }
      let ev = {};
      try {
        ev = JSON.parse(raw || "{}");
      } catch {
        res.writeHead(400).end("bad json");
        return;
      }
      lastEvent = { ...ev, at: Date.now() };
      if (ev.kind === "end") onEnd(ev);
      else if (ev.kind === "error") onError(ev);
      else if (ev.kind === "cancel") onCancel(ev);
      else onStart(ev);
      res.writeHead(204, { "access-control-allow-origin": "*" }).end();
      return;
    }

    if (req.method === "POST" && url.pathname === "/test") {
      onStart({ kind: "start", session: "demo-" + Date.now(), agent: "demo", query: "手动测试一下" });
      res.writeHead(200, { "content-type": "application/json" }).end(JSON.stringify(snapshot()));
      return;
    }

    res.writeHead(404).end("not found");
  });

  let listenFailed = false;
  server.on("error", (err) => {
    listenFailed = true;
    if (err && err.code === "EADDRINUSE") log(`pet core: ${host}:${port} 已被占用，本实例不提供服务`);
    else log("pet core 监听失败: " + (err && err.message));
  });

  function listen() {
    if (listenFailed) return false;
    server.listen(port, host, () => {
      log(`pet core on http://${host}:${port}  (inline=${!!options.inline})`);
      startUsageWatcher();
    });
    return true;
  }

  function close() {
    try { server.close(); } catch {}
    try { if (usageTimer) clearInterval(usageTimer); } catch {}
    try { clearInterval(sweeper); } catch {}
  }

  return {
    listen,
    close,
    onStart,
    onEnd,
    emitState,
    snapshot,
    get seq() { return seq; },
    push(ev) {
      lastEvent = { ...ev, at: Date.now() };
      if (ev.kind === "end") onEnd(ev);
      else if (ev.kind === "error") onError(ev);
      else if (ev.kind === "cancel") onCancel(ev);
      else onStart(ev);
    },
  };
}
