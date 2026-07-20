import { useEffect, useState } from "react";
import {
  clearTraceEvents,
  formatTraceForCopy,
  getTraceEvents,
  ingestBackendTrace,
  isIncludePolls,
  isTracingEnabled,
  setIncludePolls,
  setTracingEnabled,
  subscribeTrace,
  type PdTraceEvent,
} from "../lib/eventLog";
import { revealControlEventLog, setHttpTraceEnabled } from "../lib/api";
import { controlFetchDeviceLog } from "../lib/deviceControl";
import type { DiscoveredDevice } from "../lib/types";
import type { TransportLinks } from "../lib/transport";
import { preferredControlTransport } from "../lib/deviceControl";
import { listen } from "@tauri-apps/api/event";

function levelClass(level: PdTraceEvent["level"]): string {
  switch (level) {
    case "error":
      return "text-pd-red";
    case "warn":
      return "text-pd-amber";
    case "debug":
      return "text-gray-500";
    default:
      return "text-gray-300";
  }
}

function phaseBadge(phase: PdTraceEvent["phase"]): string {
  switch (phase) {
    case "error":
      return "bg-pd-red/20 text-pd-red";
    case "request":
      return "bg-sky-900/40 text-sky-300";
    case "response":
      return "bg-emerald-900/40 text-emerald-300";
    case "state":
      return "bg-violet-900/40 text-violet-300";
    case "transport":
      return "bg-gray-700 text-gray-300";
    default:
      return "bg-pd-border text-gray-400";
  }
}

interface EventLogPanelProps {
  device?: DiscoveredDevice | null;
  links?: TransportLinks;
}

export function EventLogPanel({ device = null, links }: EventLogPanelProps) {
  const [enabled, setEnabled] = useState(isTracingEnabled);
  const [polls, setPolls] = useState(isIncludePolls);
  const [events, setEvents] = useState(() => [...getTraceEvents()].reverse());
  const [expanded, setExpanded] = useState<string | null>(null);
  const [open, setOpen] = useState(true);
  const [copied, setCopied] = useState(false);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);

  useEffect(() => {
    return subscribeTrace(() => {
      setEnabled(isTracingEnabled());
      setPolls(isIncludePolls());
      setEvents([...getTraceEvents()].reverse());
    });
  }, []);

  useEffect(() => {
    /* Keep Rust HTTP emit gate in sync with the UI pref on mount. */
    void setHttpTraceEnabled(isTracingEnabled()).catch(() => undefined);
  }, []);

  useEffect(() => {
    let unlisten: (() => void) | undefined;
    listen<Record<string, unknown>>("pd-trace", (ev) => {
      ingestBackendTrace(ev.payload ?? {});
    }).then((fn) => {
      unlisten = fn;
    });
    return () => {
      unlisten?.();
    };
  }, []);

  const onToggleEnabled = async (next: boolean) => {
    setTracingEnabled(next);
    setEnabled(next);
    try {
      await setHttpTraceEnabled(next);
    } catch {
      /* UI store still updated */
    }
  };

  const onCopy = async () => {
    const text = formatTraceForCopy(getTraceEvents());
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      /* ignore */
    }
  };

  const onReveal = async () => {
    setBusy(true);
    setNotice(null);
    try {
      const path = await revealControlEventLog();
      setNotice(`Log: ${path}`);
    } catch (err) {
      setNotice(`Reveal failed: ${String(err)}`);
    } finally {
      setBusy(false);
    }
  };

  const onFetchDeviceLog = async () => {
    if (!device || !links) {
      setNotice("Select a device first.");
      return;
    }
    if (preferredControlTransport(links) !== "wifi") {
      setNotice("Device log requires WiFi.");
      return;
    }
    setBusy(true);
    setNotice(null);
    try {
      const lines = await controlFetchDeviceLog(device, links);
      setNotice(`Fetched ${lines.length} device log line(s).`);
    } catch (err) {
      setNotice(`Fetch failed: ${String(err)}`);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="bg-pd-panel rounded-lg border border-pd-border">
      <div className="flex items-center justify-between gap-2 px-4 py-3 border-b border-pd-border flex-wrap">
        <button
          type="button"
          className="font-semibold text-sm flex items-center gap-2"
          onClick={() => setOpen((v) => !v)}
        >
          <span className="text-gray-500">{open ? "▼" : "▶"}</span>
          Event Log
          <span className="text-xs font-normal text-gray-500">
            ({events.length})
          </span>
        </button>
        <div className="flex items-center gap-3 text-xs flex-wrap">
          <label className="flex items-center gap-1.5 text-gray-400">
            <input
              type="checkbox"
              checked={enabled}
              onChange={(e) => void onToggleEnabled(e.target.checked)}
            />
            Tracing
          </label>
          <label className="flex items-center gap-1.5 text-gray-400">
            <input
              type="checkbox"
              checked={polls}
              disabled={!enabled}
              onChange={(e) => {
                setIncludePolls(e.target.checked);
                setPolls(e.target.checked);
              }}
            />
            Include polls
          </label>
          <button
            type="button"
            onClick={() => void onReveal()}
            disabled={busy}
            className="px-2 py-1 rounded bg-pd-border hover:bg-gray-600 disabled:opacity-50"
          >
            Reveal log
          </button>
          <button
            type="button"
            onClick={() => void onFetchDeviceLog()}
            disabled={busy || !device}
            className="px-2 py-1 rounded bg-pd-border hover:bg-gray-600 disabled:opacity-50"
            title="GET /api/log from the device (WiFi)"
          >
            Fetch device log
          </button>
          <button
            type="button"
            onClick={onCopy}
            className="px-2 py-1 rounded bg-pd-border hover:bg-gray-600"
          >
            {copied ? "Copied" : "Copy"}
          </button>
          <button
            type="button"
            onClick={() => clearTraceEvents()}
            className="px-2 py-1 rounded bg-pd-border hover:bg-gray-600"
          >
            Clear
          </button>
        </div>
      </div>

      {notice && (
        <p className="px-4 py-2 text-xs text-gray-400 border-b border-pd-border break-all">
          {notice}
        </p>
      )}

      {open && (
        <div className="max-h-72 overflow-y-auto font-mono text-[11px] leading-snug">
          {events.length === 0 ? (
            <p className="px-4 py-6 text-gray-500 text-sm font-sans">
              {enabled
                ? "No events yet. Play, change FPS, upload, or delete to see the control chain."
                : "Tracing is off."}
            </p>
          ) : (
            <ul className="divide-y divide-pd-border/60">
              {events.map((e) => {
                const time = new Date(e.ts).toLocaleTimeString();
                const opShort = e.opId.replace(/^op_/, "").slice(-8);
                const isOpen = expanded === e.id;
                return (
                  <li key={e.id}>
                    <button
                      type="button"
                      className="w-full text-left px-3 py-1.5 hover:bg-pd-bg/80 flex gap-2 items-start"
                      onClick={() =>
                        setExpanded((cur) => (cur === e.id ? null : e.id))
                      }
                    >
                      <span className="text-gray-500 shrink-0 w-[4.5rem]">
                        {time}
                      </span>
                      <span className="text-gray-600 shrink-0 w-16">
                        {opShort}
                      </span>
                      <span
                        className={`shrink-0 px-1 rounded ${phaseBadge(e.phase)}`}
                      >
                        {e.phase}
                      </span>
                      <span className={`min-w-0 break-all ${levelClass(e.level)}`}>
                        {e.transport ? `[${e.transport}] ` : ""}
                        {e.action}: {e.summary}
                      </span>
                    </button>
                    {isOpen && e.detail !== undefined && (
                      <pre className="px-3 pb-2 text-gray-500 whitespace-pre-wrap break-all">
                        {JSON.stringify(e.detail, null, 2)}
                      </pre>
                    )}
                  </li>
                );
              })}
            </ul>
          )}
        </div>
      )}
    </div>
  );
}
