import { SnowClient } from "../typescript/index.mjs";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

const workspace = await mkdtemp(join(tmpdir(), "snow-ts-"));
const client = await SnowClient.connect({
  executable: process.env.SNOW_EXE,
  workspace,
  configDirName: ".sdk-test",
  dataRoot: join(workspace, "data"),
  requestTimeoutMs: 10_000
});
try {
  const session = await client.createSession({ fixture: "typescript" });
  const turn = await session.turn("hello");
  if (turn.reason !== "completed") throw new Error(JSON.stringify(turn));
  const events = await session.events();
  const types = events.map(event => event.type);
  for (const required of ["session/header", "request/header", "assistant/message", "turn/end"])
    if (!types.includes(required)) throw new Error(`missing ${required}`);
  const replay = await session.replay("R2");
  if (!replay.control.length) throw new Error("empty replay control plane");
} finally {
  await client.close();
  await rm(workspace, { recursive: true, force: true });
}
console.log("typescript_golden: ok");
