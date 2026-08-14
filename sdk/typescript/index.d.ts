import type { ChildProcessWithoutNullStreams } from "node:child_process";

export interface SnowConnectOptions {
  executable?: string;
  workspace?: string;
  dataRoot?: string;
  configDirName?: string;
  rawTrace?: boolean;
  requestTimeoutMs?: number;
}

export interface TurnOptions {
  model?: string;
  maxSteps?: number;
  modelParameters?: Record<string, unknown>;
  attachments?: readonly SnowAttachment[];
  signal?: AbortSignal;
}

export interface SnowAttachment {
  name: string;
  content: string;
  sha256?: string;
  path?: string;
  bytes?: number;
}

export interface SessionSummary {
  session_id: string;
  parent_session_id?: string;
  created_at: string;
  closed_at?: string;
  header: Record<string, unknown>;
  last_seq: number;
  closed: boolean;
}

export interface TurnResult {
  run_id: string;
  turn_id: string;
  reason: string;
  final_text: string;
  last_seq: number;
}

export class SnowSession {
  readonly id: string;
  turn(message: string, options?: TurnOptions): Promise<TurnResult>;
  steer(message: string): Promise<number>;
  events(after?: number): Promise<Record<string, unknown>[]>;
  transcript(): Promise<Record<string, unknown>[]>;
  replay(level?: "R0" | "R1" | "R2"): Promise<Record<string, unknown>>;
  fork(metadata?: Record<string, unknown>, seedSeq?: number): Promise<SnowSession>;
  close(): Promise<void>;
}

export class SnowClient extends EventTarget {
  static connect(options?: SnowConnectOptions): Promise<SnowClient>;
  readonly process: ChildProcessWithoutNullStreams;
  readonly capabilities: readonly string[];
  request(method: string, params?: Record<string, unknown>, options?: {
    timeoutMs?: number;
    signal?: AbortSignal;
  }): Promise<unknown>;
  createSession(metadata?: Record<string, unknown>): Promise<SnowSession>;
  resumeSession(sessionId: string, after?: number): Promise<SnowSession>;
  listSessions(limit?: number): Promise<SessionSummary[]>;
  respondApproval(approvalId: string, approved: boolean): Promise<boolean>;
  readArtifact(reference: { sha256: string; bytes?: number; media_type?: string },
               offset?: number, limit?: number): Promise<Record<string, unknown>>;
  close(): Promise<void>;
}
