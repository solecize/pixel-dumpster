import { useEffect, useRef, useState } from "react";
import { MoreVertical } from "lucide-react";
import type { ContentEntry } from "../lib/types";
import type { ContentPlayMode } from "../lib/transitions";

interface ContentItemMenuProps {
  item: ContentEntry;
  playMode?: ContentPlayMode;
  onPlayModeChange?: (item: ContentEntry, mode: ContentPlayMode) => void;
  onRename: (item: ContentEntry) => void;
  onDelete: (item: ContentEntry) => void;
  onChangeFps: (item: ContentEntry) => void;
}

function isSequence(item: ContentEntry): boolean {
  return Boolean(item.is_sequence) || (item.frame_count ?? 0) > 1;
}

export function ContentItemMenu({
  item,
  playMode = "animation",
  onPlayModeChange,
  onRename,
  onDelete,
  onChangeFps,
}: ContentItemMenuProps) {
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const seq = isSequence(item);

  useEffect(() => {
    if (!open) return;
    const onDoc = (e: MouseEvent) => {
      if (!rootRef.current?.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", onDoc);
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("mousedown", onDoc);
      document.removeEventListener("keydown", onKey);
    };
  }, [open]);

  return (
    <div
      ref={rootRef}
      className="relative shrink-0"
      onClick={(e) => e.stopPropagation()}
      onKeyDown={(e) => e.stopPropagation()}
    >
      <button
        type="button"
        aria-label={`Actions for ${item.name}`}
        aria-expanded={open}
        aria-haspopup="menu"
        onClick={(e) => {
          e.preventDefault();
          e.stopPropagation();
          setOpen((v) => !v);
        }}
        className="p-1.5 rounded text-gray-500 hover:text-gray-200 hover:bg-pd-border/50"
      >
        <MoreVertical className="w-4 h-4" />
      </button>
      {open && (
        <div
          role="menu"
          className="absolute right-0 top-full mt-1 z-40 min-w-[11rem] rounded-md border border-pd-border bg-pd-panel shadow-lg py-1 text-xs"
        >
          {seq && onPlayModeChange && (
            <div className="px-3 py-2 border-b border-pd-border/60">
              <div className="text-[10px] uppercase tracking-wide text-gray-500 mb-1.5">
                Play as
              </div>
              <div className="flex rounded-md overflow-hidden border border-pd-border">
                <button
                  type="button"
                  role="menuitemradio"
                  aria-checked={playMode === "animation"}
                  className={`flex-1 px-2 py-1 text-left ${
                    playMode === "animation"
                      ? "bg-pd-accent text-white"
                      : "hover:bg-pd-border/50 text-gray-300"
                  }`}
                  onClick={(e) => {
                    e.stopPropagation();
                    onPlayModeChange(item, "animation");
                  }}
                >
                  Animation
                </button>
                <button
                  type="button"
                  role="menuitemradio"
                  aria-checked={playMode === "sprite"}
                  className={`flex-1 px-2 py-1 text-left border-l border-pd-border ${
                    playMode === "sprite"
                      ? "bg-pd-accent text-white"
                      : "hover:bg-pd-border/50 text-gray-300"
                  }`}
                  onClick={(e) => {
                    e.stopPropagation();
                    onPlayModeChange(item, "sprite");
                  }}
                >
                  Sprites
                </button>
              </div>
            </div>
          )}
          <button
            type="button"
            role="menuitem"
            className="w-full px-3 py-1.5 text-left hover:bg-pd-border/50"
            onClick={(e) => {
              e.stopPropagation();
              setOpen(false);
              onRename(item);
            }}
          >
            Rename…
          </button>
          {seq && (
            <button
              type="button"
              role="menuitem"
              className="w-full px-3 py-1.5 text-left hover:bg-pd-border/50"
              onClick={(e) => {
                e.stopPropagation();
                setOpen(false);
                onChangeFps(item);
              }}
            >
              Change FPS…
            </button>
          )}
          <button
            type="button"
            role="menuitem"
            className="w-full px-3 py-1.5 text-left text-pd-red hover:bg-pd-red/15"
            onClick={(e) => {
              e.stopPropagation();
              setOpen(false);
              onDelete(item);
            }}
          >
            Trash
          </button>
        </div>
      )}
    </div>
  );
}
