import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { resolve } from "node:path";

const frameLimit = 8 * 1024 * 1024;

export class SnowSession {
  constructor(client, id) {
    this.client = client;
    this.id = id;
  }

  async turn(message, options = {}) {
    const onAbort = () => {
      void this.client.request("turn.cancel", { session_id: this.id },
        { timeoutMs: 2_000 }).catch(() => {});
    };
    options.signal?.addEventListener("abort", onAbort, { once: true });
    try {
      return await this.client.request("turn.start", {
        session_id: this.id,
        message,
        model: options.model ?? "",
        max_steps: options.maxSteps ?? 32,
        model_parameters: options.modelParameters ?? {}
      }, { signal: options.signal });
    } finally {
      options.signal?.removeEventListener("abort", onAbort);
    }
  }

  async steer(message) {
    const value = await this.client.request("turn.steer", {
      session_id: this.id, message
    });
    return value.seq;
  }

  events(after = 0) {
    return this.client.request("session.events", {
      session_id: this.id, after
    });
  }
  transcript() {
    return this.client.request("session.transcript", { session_id: this.id });
  }
  replay(level = "R2") {
    return this.client.request("session.replay", {
      session_id: this.id, level
    });
  }
  async fork(metadata = {}, seedSeq = 0) {
    const value = await this.client.request("session.fork", {
      session_id: this.id, metadata, seed_seq: seedSeq
    });
    return new SnowSession(this.client, value.session_id);
  }
  async close() {
    await this.client.request("session.close", { session_id: this.id });
  }
}

export class SnowClient extends EventTarget {
  static async connect(options = {}) {
    const client = new SnowClient(options);
    await client.#initialize();
    return client;
  }

  constructor(options) {
    super();
    this.options = {
      ...options,
      workspace: resolve(options.workspace ?? process.cwd()),
      configDirName: options.configDirName ?? ".snow",
      requestTimeoutMs: options.requestTimeoutMs ?? 300_000
    };
    if (!/^[A-Za-z0-9._-]+$/.test(this.options.configDirName) ||
        this.options.configDirName === "." ||
        this.options.configDirName === "..") {
      throw new Error("configDirName must be one safe directory name");
    }
    const executable = this.options.executable ??
      (process.platform === "win32" ? "snow.exe" : "snow");
    const args = ["serve", "--workspace", this.options.workspace,
      "--config-dir-name", this.options.configDirName];
    if (this.options.dataRoot)
      args.push("--data-root", resolve(this.options.dataRoot));
    if (this.options.rawTrace) args.push("--raw-trace");
    this.process = spawn(executable, args, {
      stdio: ["pipe", "pipe", "pipe"], windowsHide: true
    });
    this.pending = new Map();
    this.nextId = 1;
    this.capabilities = [];
    this.stderr = [];
    createInterface({ input: this.process.stdout, crlfDelay: Infinity })
      .on("line", line => this.#receive(line));
    createInterface({ input: this.process.stderr, crlfDelay: Infinity })
      .on("line", line => {
        this.stderr.push(line);
        if (this.stderr.length > 200) this.stderr.shift();
      });
    this.process.once("exit", (code, signal) => {
      const error = new Error(`Snow exited (${code ?? signal ?? "unknown"})`);
      error.diagnostics = [...this.stderr];
      for (const value of this.pending.values()) value.reject(error);
      this.pending.clear();
      this.dispatchEvent(new CustomEvent("exit", {
        detail: { code, signal, diagnostics: error.diagnostics }
      }));
    });
  }

  async #initialize() {
    const value = await this.request("initialize", {
      protocol_min: 1, protocol_max: 1,
      client: { name: "@tokmon/snow", version: "1.0.0" }
    });
    if (value.selected_protocol !== 1)
      throw new Error("Snow protocol negotiation failed");
    this.capabilities = Object.freeze([...value.capabilities]);
  }

  #receive(line) {
    if (!line || Buffer.byteLength(line) > frameLimit) return;
    let message;
    try { message = JSON.parse(line); } catch { return; }
    if (message.id !== undefined && message.id !== null) {
      const pending = this.pending.get(String(message.id));
      if (!pending) return;
      this.pending.delete(String(message.id));
      clearTimeout(pending.timer);
      if (message.error) {
        const error = new Error(message.error.message ?? "Snow request failed");
        error.code = message.error.code;
        error.data = message.error.data;
        pending.reject(error);
      } else pending.resolve(message.result);
      return;
    }
    if (message.method) {
      this.dispatchEvent(new CustomEvent(message.method, {
        detail: message.params ?? {}
      }));
      this.dispatchEvent(new CustomEvent("notification", { detail: message }));
    }
  }

  request(method, params = {}, options = {}) {
    if (options.signal?.aborted)
      return Promise.reject(options.signal.reason ?? new Error("Aborted"));
    if (this.pending.size >= 64)
      return Promise.reject(new Error("Snow client backpressure limit reached"));
    const id = this.nextId++;
    const timeoutMs = options.timeoutMs ?? this.options.requestTimeoutMs;
    return new Promise((resolvePromise, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(String(id));
        reject(new Error(`Snow request timed out: ${method}`));
      }, timeoutMs);
      const abort = () => {
        clearTimeout(timer);
        this.pending.delete(String(id));
        reject(options.signal.reason ?? new Error("Aborted"));
      };
      options.signal?.addEventListener("abort", abort, { once: true });
      this.pending.set(String(id), {
        timer,
        resolve: value => {
          options.signal?.removeEventListener("abort", abort);
          resolvePromise(value);
        },
        reject: error => {
          options.signal?.removeEventListener("abort", abort);
          reject(error);
        }
      });
      const frame = JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n";
      if (Buffer.byteLength(frame) > frameLimit) {
        clearTimeout(timer);
        this.pending.delete(String(id));
        reject(new Error("Snow request exceeds the 8 MiB frame limit"));
        return;
      }
      this.process.stdin.write(frame, error => {
        if (error) {
          clearTimeout(timer);
          this.pending.delete(String(id));
          reject(error);
        }
      });
    });
  }

  async createSession(metadata = {}) {
    const value = await this.request("session.create", { metadata });
    return new SnowSession(this, value.session_id);
  }
  async resumeSession(sessionId, after = 0) {
    await this.request("session.resume", { session_id: sessionId, after });
    return new SnowSession(this, sessionId);
  }
  async respondApproval(approvalId, approved) {
    const value = await this.request("approval.respond", {
      approval_id: approvalId, approved
    });
    return value.resolved;
  }
  readArtifact(reference, offset = 0, limit = 256 * 1024) {
    return this.request("artifact.read", { ...reference, offset, limit });
  }
  async close() {
    if (this.process.exitCode !== null) return;
    this.process.stdin.end();
    await new Promise(resolvePromise => {
      const timer = setTimeout(() => {
        this.process.kill();
        resolvePromise();
      }, 2_000);
      this.process.once("exit", () => {
        clearTimeout(timer);
        resolvePromise();
      });
    });
  }
}
