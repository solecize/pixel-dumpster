/**
 * Control-plane event log: correlated steps across WiFi / BLE / USB.
 * When tracing is off, emit is a no-op. Events are also appended to a
 * durable NDJSON file via the Tauri backend when tracing is on.
 */

import { appendTraceEvent } from "./api";

export type TraceLevel = "debug" | "info" | "warn" | "error";
export type TracePhase =
  | "intent"
  | "transport"
  | "request"
  | "response"
  | "state"
  | "error";

export type TraceTransport = "wifi" | "bluetooth" | "usb";

export interface PdTraceEvent {
  id: string;
  opId: string;
  ts: number;
  level: TraceLevel;
  phase: TracePhase;
  action: string;
  transport?: TraceTransport;
  summary: string;
  detail?: unknown;
}

const MAX_EVENTS = 500;
const PREF_ENABLED = "pd.eventTracingEnabled";
const PREF_POLLS = "pd.eventTracingIncludePolls";

type Listener = () => void;

const listeners = new Set<Listener>();
let events: PdTraceEvent[] = [];
let seq = 0;
let activeOpId: string | null = null;

function readBool(key: string, fallback: boolean): boolean {
  try {
    const v = localStorage.getItem(key);
    if (v === null) return fallback;
    return v === "1" || v === "true";
  } catch {
    return fallback;
  }
}

function writeBool(key: string, value: boolean): void {
  try {
    localStorage.setItem(key, value ? "1" : "0");
  } catch {
    /* ignore */
  }
}

/** Default on while debugging control-plane issues. */
let tracingEnabled = readBool(PREF_ENABLED, true);
let includePolls = readBool(PREF_POLLS, false);

function notify(): void {
  for (const l of listeners) l();
}

function nextId(prefix: string): string {
  seq += 1;
  return `${prefix}${Date.now().toString(36)}_${seq.toString(36)}`;
}

export function isTracingEnabled(): boolean {
  return tracingEnabled;
}

export function setTracingEnabled(enabled: boolean): void {
  tracingEnabled = enabled;
  writeBool(PREF_ENABLED, enabled);
  notify();
}

export function isIncludePolls(): boolean {
  return includePolls;
}

export function setIncludePolls(enabled: boolean): void {
  includePolls = enabled;
  writeBool(PREF_POLLS, enabled);
  notify();
}

export function getTraceEvents(): readonly PdTraceEvent[] {
  return events;
}

export function clearTraceEvents(): void {
  events = [];
  notify();
}

export function subscribeTrace(listener: Listener): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function getActiveOpId(): string | null {
  return activeOpId;
}

export function beginOp(action: string): string {
  const opId = nextId("op_");
  activeOpId = opId;
  emitTrace({
    opId,
    level: "info",
    phase: "intent",
    action,
    summary: `begin ${action}`,
  });
  return opId;
}

export function endOp(opId: string): void {
  if (activeOpId === opId) activeOpId = null;
}

/** Emit a correlated event. No-op when tracing is disabled (except errors still drop when off). */
export function emitTrace(
  partial: Omit<PdTraceEvent, "id" | "ts" | "opId"> & {
    opId?: string;
    /** When false, skip durable file append (already written by Rust http_trace). */
    persist?: boolean;
  }
): void {
  if (!tracingEnabled) return;

  const action = partial.action;
  const isPoll = action === "poll_status" || action === "poll_list";
  if (isPoll && !includePolls) {
    /* Background polls are suppressed unless include-polls is on.
     * Errors always pass through. */
    if (partial.level !== "error" && partial.phase !== "error") return;
  }

  const ev: PdTraceEvent = {
    id: nextId("ev_"),
    opId: partial.opId ?? activeOpId ?? "orphan",
    ts: Date.now(),
    level: partial.level,
    phase: partial.phase,
    action: partial.action,
    transport: partial.transport,
    summary: partial.summary,
    detail: partial.detail,
  };

  events = [...events.slice(-(MAX_EVENTS - 1)), ev];
  notify();

  if (partial.persist === false) return;

  /* Fire-and-forget durable append (Rust writer + rotate). */
  void appendTraceEvent({
    id: ev.id,
    opId: ev.opId,
    ts: ev.ts,
    level: ev.level,
    phase: ev.phase,
    action: ev.action,
    transport: ev.transport,
    summary: ev.summary,
    detail: ev.detail,
  }).catch(() => undefined);
}

/** Run an async control operation under a shared opId. */
export async function tracedOp<T>(
  action: string,
  fn: (opId: string) => Promise<T>
): Promise<T> {
  const opId = beginOp(action);
  try {
    const result = await fn(opId);
    emitTrace({
      opId,
      level: "info",
      phase: "response",
      action,
      summary: `${action} ok`,
    });
    return result;
  } catch (err) {
    emitTrace({
      opId,
      level: "error",
      phase: "error",
      action,
      summary: `${action} failed: ${String(err)}`,
      detail: { error: String(err) },
    });
    throw err;
  } finally {
    endOp(opId);
  }
}

export function formatTraceForCopy(list: readonly PdTraceEvent[]): string {
  return list
    .map((e) => {
      const t = new Date(e.ts).toISOString();
      const tr = e.transport ? ` [${e.transport}]` : "";
      const detail =
        e.detail === undefined
          ? ""
          : `\n  detail: ${JSON.stringify(e.detail)}`;
      return `${t} ${e.opId} ${e.level} ${e.phase} ${e.action}${tr} — ${e.summary}${detail}`;
    })
    .join("\n");
}

/** Ingest a wire event from the Tauri backend (HTTP layer). */
export function ingestBackendTrace(raw: Record<string, unknown>): void {
  if (!tracingEnabled) return;
  const action = String(raw.action ?? "http");
  const isPoll =
    action.includes("status") ||
    action.includes("/api/status") ||
    action.includes("/api/content") && String(raw.method ?? "") === "GET";
  if (isPoll && !includePolls && raw.level !== "error") {
    const path = String(raw.path ?? raw.url ?? "");
    if (path.includes("/api/status") || path.endsWith("/api/content")) {
      return;
    }
  }

  emitTrace({
    opId: (raw.opId as string) || getActiveOpId() || "wire",
    level: (raw.level as TraceLevel) || "info",
    phase: (raw.phase as TracePhase) || "request",
    action,
    transport: "wifi",
    summary: String(raw.summary ?? action),
    detail: raw.detail ?? raw,
    persist: false, /* already appended by Rust http_trace */
  });
}
