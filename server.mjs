// 独立运行的事件服务（可选）。
// 现在状态机已经内联进 pet.hooks.js（跑在 magpie 的 Bun 插件宿主里），
// 正常情况下不需要这个文件。保留它是为了：
//   - 不启动 magpie 时调试桌宠
//   - 插件宿主没起来时的兜底
//
// 运行：bun server.mjs   （端口被内联版占住时会安静退出）

import { createPetCore } from "./pet-core.mjs";

const core = createPetCore({ inline: false });
core.listen();

process.on("SIGINT", () => {
  core.close();
  process.exit(0);
});
