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
  const sessions = await client.listSessions();
  if (!sessions.some(value => value.session_id === session.id))
    throw new Error("created session missing from session.list");
  const turn = await session.turn("hello", {
    attachments: [{ name: "context.txt", content: "typescript sdk context" }]
  });
  if (turn.reason !== "completed") throw new Error(JSON.stringify(turn));
  const events = await session.events();
  const types = events.map(event => event.type);
  for (const required of ["session/header", "request/header", "assistant/message", "turn/end"])
    if (!types.includes(required)) throw new Error(`missing ${required}`);
  const user = events.find(event => event.type === "user/message");
  if (user?.data?.attachments?.[0]?.name !== "context.txt")
    throw new Error("attachment missing from trajectory");
  const replay = await session.replay("R2");
  if (!replay.control.length) throw new Error("empty replay control plane");
} finally {
  await client.close();
  await rm(workspace, { recursive: true, force: true });
}
console.log("typescript_golden: ok");
