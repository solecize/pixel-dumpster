import type { DiscoveredDevice } from "../lib/types";

type NavTab = "content" | "device-setup" | "flash" | "pi-setup";

interface SidebarProps {
  devices: DiscoveredDevice[];
  selected: DiscoveredDevice | null;
  onSelect: (device: DiscoveredDevice) => void;
  onSetView: (view: NavTab) => void;
  activeView: NavTab;
}

const TABS: { id: NavTab; label: string; icon: React.ReactNode }[] = [
  {
    id: "content",
    label: "Content",
    icon: (
      <svg xmlns="http://www.w3.org/2000/svg" className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/>
      </svg>
    ),
  },
  {
    id: "device-setup",
    label: "Device Setup",
    icon: (
      <svg xmlns="http://www.w3.org/2000/svg" className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.1a2 2 0 0 1-1-1.72v-.51a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>
      </svg>
    ),
  },
  {
    id: "flash",
    label: "Flash ESP32",
    icon: (
      <svg xmlns="http://www.w3.org/2000/svg" className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/>
      </svg>
    ),
  },
  {
    id: "pi-setup",
    label: "Pi Setup",
    icon: (
      <svg xmlns="http://www.w3.org/2000/svg" className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <rect x="2" y="3" width="20" height="14" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/>
      </svg>
    ),
  },
];

export function Sidebar({ devices: _devices, selected, onSelect: _onSelect, onSetView, activeView }: SidebarProps) {
  return (
    <aside className="w-64 bg-pd-panel border-r border-pd-border flex flex-col select-none">
      {/* Header */}
      <div className="p-4 border-b border-pd-border">
        <h1 className="text-lg font-bold tracking-tight">Pixel Dumpster</h1>
        <p className="text-xs text-gray-500 mt-0.5">Control Center</p>
      </div>

      {/* Navigation */}
      <nav className="flex-1 p-2 space-y-0.5">
        {TABS.map((tab) => (
          <button
            key={tab.id}
            onClick={() => onSetView(tab.id)}
            className={`w-full flex items-center gap-3 px-3 py-2.5 text-sm rounded-lg transition ${
              activeView === tab.id
                ? "bg-pd-accent/15 text-white border-l-2 border-pd-accent"
                : "text-gray-400 hover:bg-pd-border/40 hover:text-gray-200"
            }`}
          >
            {tab.icon}
            <span className="font-medium">{tab.label}</span>
          </button>
        ))}
      </nav>

      {/* Device quick-picker */}
      <div className="p-3 border-t border-pd-border">
        <div className="text-xs font-semibold text-gray-500 uppercase tracking-wider mb-2">
          Selected Device
        </div>
        {selected ? (
          <button
            onClick={() => onSetView("content")}
            className="w-full text-left px-3 py-2 rounded-lg bg-pd-bg border border-pd-border hover:border-pd-accent/50 transition"
          >
            <div className="flex items-center gap-2">
              <span className="w-2 h-2 rounded-full bg-pd-green" />
              <span className="text-sm font-medium truncate">{selected.name}</span>
            </div>
            <div className="text-xs text-gray-500 mt-0.5 ml-4">
              {selected.ip}:{selected.port}
            </div>
          </button>
        ) : (
          <div className="px-3 py-2 text-xs text-gray-600">
            No device selected
          </div>
        )}
      </div>

      {/* Footer */}
      <div className="p-3 border-t border-pd-border text-xs text-gray-600 text-center">
        v0.2.0
      </div>
    </aside>
  );
}
