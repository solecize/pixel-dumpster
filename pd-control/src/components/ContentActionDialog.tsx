import { useEffect, useRef, useState } from "react";

type DialogMode =
  | { kind: "rename"; itemName: string; initial: string }
  | { kind: "fps"; itemName: string; initial: number }
  | { kind: "seq-upload"; folderName: string; initialFps: number }
  | { kind: "delete"; itemName: string };

interface ContentActionDialogProps {
  mode: DialogMode;
  busy?: boolean;
  onCancel: () => void;
  onSubmit: (value: string) => void;
}

export function ContentActionDialog({
  mode,
  busy = false,
  onCancel,
  onSubmit,
}: ContentActionDialogProps) {
  const [value, setValue] = useState(
    mode.kind === "delete"
      ? ""
      : mode.kind === "fps" || mode.kind === "seq-upload"
        ? String(mode.kind === "fps" ? mode.initial : mode.initialFps)
        : mode.initial
  );
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    inputRef.current?.focus();
    inputRef.current?.select();
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape" && !busy) onCancel();
    };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
  }, [busy, onCancel]);

  const title =
    mode.kind === "rename"
      ? `Rename “${mode.itemName}”`
      : mode.kind === "fps"
        ? `FPS for “${mode.itemName}”`
        : mode.kind === "seq-upload"
          ? `Upload sequence “${mode.folderName}”`
          : `Delete “${mode.itemName}”?`;

  const submitLabel =
    mode.kind === "rename"
      ? "Rename"
      : mode.kind === "fps"
        ? "Save"
        : mode.kind === "seq-upload"
          ? "Upload"
          : "Trash";

  const needsValue = mode.kind !== "delete";

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 p-4"
      onMouseDown={(e) => {
        if (e.target === e.currentTarget && !busy) onCancel();
      }}
    >
      <form
        className="w-full max-w-sm rounded-lg border border-pd-border bg-pd-panel p-4 shadow-xl space-y-3"
        onSubmit={(e) => {
          e.preventDefault();
          if (busy) return;
          onSubmit(mode.kind === "delete" ? "1" : value.trim());
        }}
      >
        <h3 className="font-semibold text-sm">{title}</h3>
        {mode.kind === "delete" ? (
          <p className="text-sm text-gray-400">
            This removes the file or sequence from the device. This cannot be
            undone.
          </p>
        ) : mode.kind === "seq-upload" ? (
          <div className="space-y-2">
            <p className="text-sm text-gray-400">
              Numbered PNGs in the folder become an image sequence.
            </p>
            <label className="block space-y-1">
              <span className="text-xs text-gray-500">
                Frames per second (1–120)
              </span>
              <input
                ref={inputRef}
                type="number"
                min={1}
                max={120}
                value={value}
                disabled={busy}
                onChange={(e) => setValue(e.target.value)}
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </label>
          </div>
        ) : (
          <label className="block space-y-1">
            <span className="text-xs text-gray-500">
              {mode.kind === "fps" ? "Frames per second (1–120)" : "New name"}
            </span>
            <input
              ref={inputRef}
              type={mode.kind === "fps" ? "number" : "text"}
              min={mode.kind === "fps" ? 1 : undefined}
              max={mode.kind === "fps" ? 120 : undefined}
              value={value}
              disabled={busy}
              onChange={(e) => setValue(e.target.value)}
              className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
            />
          </label>
        )}
        <div className="flex justify-end gap-2 pt-1">
          <button
            type="button"
            disabled={busy}
            onClick={onCancel}
            className="px-3 py-1.5 text-sm bg-pd-border hover:bg-gray-600 rounded transition disabled:opacity-50"
          >
            Cancel
          </button>
          <button
            type="submit"
            disabled={busy || (needsValue && !value.trim())}
            className={`px-3 py-1.5 text-sm rounded transition disabled:opacity-50 ${
              mode.kind === "delete"
                ? "bg-pd-red/20 text-pd-red hover:bg-pd-red/30"
                : "bg-pd-accent hover:bg-indigo-600 text-white"
            }`}
          >
            {busy ? "Working…" : submitLabel}
          </button>
        </div>
      </form>
    </div>
  );
}
