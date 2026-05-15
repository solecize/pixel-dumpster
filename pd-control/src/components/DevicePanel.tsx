import { useState, useEffect, useCallback } from "react";
import type { DiscoveredDevice, DeviceStatus, ContentEntry } from "../lib/types";
import { PanelLayoutSVG } from "./PanelLayoutSVG";
import {
  getDeviceStatus,
  getDeviceContent,
  devicePlay,
  deviceStop,
  uploadContentToDevice,
  getDeviceLayout,
  setDeviceLayout,
  previewDeviceLayout,
  startTestPattern,
  stopTestPattern,
} from "../lib/api";
import { testContent } from "../lib/testContent";

const TEST_PATTERNS = [
  { id: "color_test", label: "Color Test", desc: "Red / Green / Blue bands — best for checking colour order" },
  { id: "numbered_panels", label: "Numbered Panels", desc: "Panel 1, 2, 3… on each module" },
  { id: "checkerboard", label: "Checkerboard", desc: "Scrolling checker across all panels" },
  { id: "arrow_chain", label: "Arrow Chain", desc: "Arrows showing chain direction" },
  { id: "rgb_sweep", label: "RGB Sweep", desc: "Full-canvas colour sweep" },
  { id: "bouncing_ball", label: "Bouncing Ball", desc: "Ball crossing panel boundaries" },
];

interface DevicePanelProps {
  device: DiscoveredDevice;
}

export function DevicePanel({ device }: DevicePanelProps) {
  const [tab, setTab] = useState<"status" | "content" | "diagnostics" | "config">("status");
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [content, setContent] = useState<ContentEntry[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [uploadingTest, setUploadingTest] = useState<string | null>(null);
  const [layout, setLayout] = useState<Record<string, unknown> | null>(null);
  const [editLayout, setEditLayout] = useState(false);
  const [layoutForm, setLayoutForm] = useState({ panel_width: 64, panel_height: 32, panel_rows: 1, panel_cols: 1, chain_pattern: 0, panel_rotation_deg: 0, color_order: 0 });
  const [layoutSaving, setLayoutSaving] = useState(false);
  const [layoutSaved, setLayoutSaved] = useState<string | null>(null);
  const [activePattern, setActivePattern] = useState<string | null>(null);
  const [brightness, setBrightness] = useState(50);
  const [diagBusy, setDiagBusy] = useState(false);
  const [diagError, setDiagError] = useState<string | null>(null);

  const refreshStatus = useCallback(async () => {
    try {
      const s = await getDeviceStatus(device.ip, device.port);
      setStatus(s);
      setError(null);
    } catch (err) {
      setError(String(err));
    }
  }, [device.ip, device.port]);

  const refreshLayout = useCallback(async () => {
    try {
      const l = await getDeviceLayout(device.ip, device.port) as Record<string, unknown>;
      setLayout(l);
      setLayoutForm({
        panel_width: Number(l.panel_width) || 64,
        panel_height: Number(l.panel_height) || 32,
        panel_rows: Number(l.panel_rows) || 1,
        panel_cols: Number(l.panel_cols) || 1,
        chain_pattern: Number(l.chain_pattern) || 0,
        panel_rotation_deg: Number(l.panel_rotation_deg) || 0,
        color_order: Number(l.color_order) || 0,
      });
    } catch {
      /* non-fatal */
    }
  }, [device.ip, device.port]);

  const refreshContent = useCallback(async () => {
    try {
      const c = await getDeviceContent(device.ip, device.port);
      setContent(c.items || []);
    } catch (err) {
      setError(String(err));
    }
  }, [device.ip, device.port]);

  useEffect(() => {
    refreshStatus();
    refreshContent();
    refreshLayout();
    const interval = setInterval(refreshStatus, 3000);
    return () => clearInterval(interval);
  }, [refreshStatus, refreshContent, refreshLayout]);

  const handlePlay = async (path: string) => {
    try {
      await devicePlay(device.ip, device.port, path, "fade", 800);
      setTimeout(refreshStatus, 500);
    } catch (err) {
      setError(String(err));
    }
  };

  const handleStop = async () => {
    try {
      await deviceStop(device.ip, device.port);
      setTimeout(refreshStatus, 500);
    } catch (err) {
      setError(String(err));
    }
  };

  const handlePlayTest = async (testPath: string) => {
    setUploadingTest(testPath);
    try {
      // System paths are special and don't need upload
      if (testPath.startsWith("system/")) {
        await devicePlay(device.ip, device.port, testPath, "fade", 800);
      } else {
        // First try to play - if it fails, upload then play
        try {
          await devicePlay(device.ip, device.port, testPath, "fade", 800);
        } catch {
          // Content not on device, upload it
          await uploadContentToDevice(device.ip, device.port, testPath);
          // Wait a moment for upload to complete, then play
          await new Promise(resolve => setTimeout(resolve, 500));
          await devicePlay(device.ip, device.port, testPath, "fade", 800);
        }
      }
      setTimeout(refreshStatus, 500);
      setError(null);
    } catch (err) {
      setError(String(err));
    } finally {
      setUploadingTest(null);
    }
  };

  const handleTestStart = async (patternId: string) => {
    setDiagBusy(true);
    setDiagError(null);
    try {
      await startTestPattern(device.ip, device.port, patternId, brightness);
      setActivePattern(patternId);
    } catch (err) {
      setDiagError(String(err));
    } finally {
      setDiagBusy(false);
    }
  };

  const handleTestStop = async () => {
    setDiagBusy(true);
    setDiagError(null);
    try {
      await stopTestPattern(device.ip, device.port);
      setActivePattern(null);
    } catch (err) {
      setDiagError(String(err));
    } finally {
      setDiagBusy(false);
    }
  };

  const tabs = [
    { id: "status" as const, label: "Now Playing" },
    { id: "content" as const, label: "Content" },
    { id: "diagnostics" as const, label: "Diagnostics" },
    { id: "config" as const, label: "Config" },
  ];

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-xl font-bold">{device.name}</h2>
          <p className="text-sm text-gray-500">
            {device.ip}:{device.port}
            {device.txt.width && (
              <span className="ml-2">
                {device.txt.width}x{device.txt.height}
              </span>
            )}
          </p>
        </div>
        <span className="px-2 py-1 text-xs rounded bg-pd-green/20 text-pd-green">
          Device
        </span>
      </div>

      {error && (
        <div className="mb-4 p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
          {error}
        </div>
      )}

      <div className="flex gap-1 mb-6 border-b border-pd-border">
        {tabs.map((t) => (
          <button
            key={t.id}
            onClick={() => setTab(t.id)}
            className={`px-4 py-2 text-sm transition ${
              tab === t.id
                ? "border-b-2 border-pd-accent text-white"
                : "text-gray-500 hover:text-gray-300"
            }`}
          >
            {t.label}
          </button>
        ))}
      </div>

      {tab === "status" && (
        <div className="space-y-4">
          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <div className="flex items-center justify-between mb-3">
              <h3 className="font-semibold">Playback</h3>
              <button
                onClick={handleStop}
                className="px-3 py-1 text-sm bg-pd-red/20 text-pd-red hover:bg-pd-red/30 rounded transition"
              >
                Stop
              </button>
            </div>
            {status ? (
              <div className="space-y-2 text-sm">
                <div className="flex justify-between">
                  <span className="text-gray-500">Status</span>
                  <span>{status.playing ? "Playing" : "Stopped"}</span>
                </div>
                {status.path && (
                  <div className="flex justify-between">
                    <span className="text-gray-500">Path</span>
                    <span className="text-right truncate max-w-xs">
                      {status.path}
                    </span>
                  </div>
                )}
                {status.is_sequence && (
                  <div className="flex justify-between">
                    <span className="text-gray-500">Frame</span>
                    <span>
                      {status.current_frame}/{status.total_frames} @{" "}
                      {status.fps}fps
                    </span>
                  </div>
                )}
              </div>
            ) : (
              <p className="text-gray-500 text-sm">Loading...</p>
            )}
          </div>
        </div>
      )}

      {tab === "content" && (
        <div className="space-y-2">
          <div className="flex items-center justify-between mb-2">
            <h3 className="font-semibold">
              Content Library ({content.length})
            </h3>
            <button
              onClick={refreshContent}
              className="px-3 py-1 text-sm bg-pd-border hover:bg-gray-600 rounded transition"
            >
              Refresh
            </button>
          </div>
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
            {content.map((item) => (
              <button
                key={item.path}
                onClick={() => handlePlay(item.path)}
                className="bg-pd-panel border border-pd-border rounded-lg p-3 text-left hover:border-pd-accent/50 transition"
              >
                <div className="text-sm font-medium truncate">
                  {item.name}
                </div>
                <div className="text-xs text-gray-500 mt-1 truncate">
                  {item.path}
                </div>
                {item.is_sequence && (
                  <div className="text-xs text-pd-amber mt-1">
                    {item.frame_count} frames @ {item.fps}fps
                  </div>
                )}
              </button>
            ))}
          </div>
          {content.length === 0 && (
            <p className="text-gray-500 text-sm text-center py-8">
              No content found on device.
            </p>
          )}

          {/* Test Content Section */}
          <div className="mt-8 pt-6 border-t border-pd-border">
            <div className="flex items-center justify-between mb-3">
              <h3 className="font-semibold text-sm text-gray-400">
                Test Content
              </h3>
              <span className="text-xs text-gray-600">Built-in samples</span>
            </div>
            <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
              {testContent.map((item) => (
                <button
                  key={item.path}
                  onClick={() => handlePlayTest(item.path)}
                  disabled={uploadingTest === item.path}
                  className="bg-pd-panel/50 border border-pd-border/50 rounded-lg p-3 text-left hover:border-purple-500/50 transition disabled:opacity-50 disabled:cursor-wait"
                >
                  <div className="flex items-start justify-between mb-1">
                    <div className="text-sm font-medium truncate flex-1">
                      {item.name}
                    </div>
                    <span className="text-xs px-1.5 py-0.5 rounded bg-purple-600/20 text-purple-400 ml-2 flex-shrink-0">
                      {item.category}
                    </span>
                  </div>
                  <div className="text-xs text-gray-500 mb-1">
                    {item.description}
                  </div>
                  <div className="text-xs text-gray-600">
                    {item.path}
                  </div>
                  {uploadingTest === item.path && (
                    <div className="text-xs text-purple-400 mt-1">
                      Uploading...
                    </div>
                  )}
                </button>
              ))}
            </div>
          </div>
        </div>
      )}

      {tab === "diagnostics" && (
        <div className="space-y-4">
          {/* Configure Layout */}
          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <div className="flex items-center justify-between mb-3">
              <h3 className="font-semibold text-sm">Panel Layout</h3>
              <button
                onClick={() => { setEditLayout(!editLayout); setLayoutSaved(null); }}
                className="text-xs px-2 py-1 rounded bg-pd-bg border border-pd-border hover:border-gray-500"
              >
                {editLayout ? "Cancel" : "Edit"}
              </button>
            </div>

            {/* Layout SVG */}
            {layout && (
              <>
                <PanelLayoutSVG
                  panelWidth={Number(layout.panel_width) || 64}
                  panelHeight={Number(layout.panel_height) || 32}
                  panelRows={Number(layout.panel_rows) || 1}
                  panelCols={Number(layout.panel_cols) || 1}
                  chainPattern={Number(layout.chain_pattern) || 0}
                  panelRotationDeg={Number(layout.panel_rotation_deg) || 0}
                  testPattern={activePattern}
                />
                <div className="mt-2 mb-3 flex items-center gap-2">
                  <select
                    value={activePattern ?? ''}
                    onChange={e => e.target.value ? handleTestStart(e.target.value) : handleTestStop()}
                    disabled={diagBusy}
                    className="flex-1 bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none disabled:opacity-40"
                  >
                    <option value="">— No test pattern —</option>
                    {TEST_PATTERNS.map(p => (
                      <option key={p.id} value={p.id}>{p.label} — {p.desc}</option>
                    ))}
                  </select>
                  {activePattern && (
                    <button onClick={handleTestStop} disabled={diagBusy}
                      className="px-3 py-1.5 text-xs bg-pd-red/20 text-pd-red rounded border border-pd-red/30 hover:bg-pd-red/30 disabled:opacity-50">
                      Stop
                    </button>
                  )}
                </div>
              </>
            )}

            {/* Read-only summary */}
            {!editLayout && layout && (
              <div className="grid grid-cols-2 gap-x-6 gap-y-1.5 text-sm">
                <div className="text-gray-500">Panel size</div>
                <div>{String(layout.panel_width)}×{String(layout.panel_height)}</div>
                <div className="text-gray-500">Grid</div>
                <div>{String(layout.panel_rows)} row × {String(layout.panel_cols)} col</div>
                <div className="text-gray-500">Canvas</div>
                <div>{String(layout.matrix_width)}×{String(layout.matrix_height)}</div>
                <div className="text-gray-500">Chain pattern</div>
                <div>{["Horizontal","Serpentine TL↓","Serpentine TR↓","Serpentine BL↑","Serpentine BR↑","Zigzag TL↓","Zigzag TR↓","Zigzag BL↑","Zigzag BR↑"][Number(layout.chain_pattern)] ?? String(layout.chain_pattern)}</div>
                <div className="text-gray-500">Mount rotation</div>
                <div>{String(layout.panel_rotation_deg)}°</div>
                <div className="text-gray-500">Color order</div>
                <div>{["RGB","BGR","GRB","BRG"][Number(layout.color_order)] ?? String(layout.color_order)}</div>
              </div>
            )}
            {!editLayout && !layout && (
              <p className="text-gray-500 text-sm">Loading…</p>
            )}

            {/* Editable form */}
            {editLayout && (
              <div className="space-y-3">
                <div className="grid grid-cols-2 gap-3">
                  <div>
                    <label className="text-xs text-gray-500 block mb-1">Panel width (px)</label>
                    <input type="number" min={8} max={512} value={layoutForm.panel_width}
                      onChange={e => setLayoutForm(f => ({...f, panel_width: Number(e.target.value)}))}
                      className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                  </div>
                  <div>
                    <label className="text-xs text-gray-500 block mb-1">Panel height (px)</label>
                    <input type="number" min={8} max={512} value={layoutForm.panel_height}
                      onChange={e => setLayoutForm(f => ({...f, panel_height: Number(e.target.value)}))}
                      className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                  </div>
                  <div>
                    <label className="text-xs text-gray-500 block mb-1">Rows</label>
                    <input type="number" min={1} max={8} value={layoutForm.panel_rows}
                      onChange={e => setLayoutForm(f => ({...f, panel_rows: Number(e.target.value)}))}
                      className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                  </div>
                  <div>
                    <label className="text-xs text-gray-500 block mb-1">Cols</label>
                    <input type="number" min={1} max={8} value={layoutForm.panel_cols}
                      onChange={e => setLayoutForm(f => ({...f, panel_cols: Number(e.target.value)}))}
                      className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                  </div>
                </div>
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Chain pattern</label>
                  <select value={layoutForm.chain_pattern}
                    onChange={e => setLayoutForm(f => ({...f, chain_pattern: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none">
                    <option value={0}>Linear (row-by-row sequential)</option>
                    <option value={1}>Serpentine — top-left, snake down</option>
                    <option value={2}>Serpentine — top-right, snake down</option>
                    <option value={3}>Serpentine — bottom-left, snake up</option>
                    <option value={4}>Serpentine — bottom-right, snake up</option>
                    <option value={5}>Zigzag — top-left, down</option>
                    <option value={6}>Zigzag — top-right, down</option>
                    <option value={7}>Zigzag — bottom-left, up</option>
                    <option value={8}>Zigzag — bottom-right, up</option>
                  </select>
                </div>
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Panel mount rotation (per physical module)</label>
                  <select value={layoutForm.panel_rotation_deg}
                    onChange={e => setLayoutForm(f => ({...f, panel_rotation_deg: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none">
                    <option value={0}>0° (normal)</option>
                    <option value={90}>90° CW</option>
                    <option value={180}>180°</option>
                    <option value={270}>270° CW</option>
                  </select>
                </div>
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Color order</label>
                  <select value={layoutForm.color_order}
                    onChange={e => setLayoutForm(f => ({...f, color_order: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none">
                    <option value={0}>RGB</option>
                    <option value={1}>BGR</option>
                    <option value={2}>GRB</option>
                    <option value={3}>BRG</option>
                  </select>
                </div>
                <div className="text-xs text-gray-500 pt-1">
                  {(() => {
                    const rot = layoutForm.panel_rotation_deg;
                    const cw = (rot === 90 || rot === 270)
                      ? layoutForm.panel_height * layoutForm.panel_rows
                      : layoutForm.panel_width  * layoutForm.panel_cols;
                    const ch = (rot === 90 || rot === 270)
                      ? layoutForm.panel_width  * layoutForm.panel_cols
                      : layoutForm.panel_height * layoutForm.panel_rows;
                    return `Canvas will be ${cw}×${ch} px`;
                  })()}
                </div>
                {layoutSaved && <div className="text-xs text-pd-green">{layoutSaved}</div>}
                <div className="flex gap-2">
                  <button
                    onClick={async () => {
                      setLayoutSaved(null);
                      try {
                        const res = await previewDeviceLayout(device.ip, device.port, layoutForm) as { ok?: boolean; error?: string };
                        if (res && res.ok === false && res.error) {
                          setLayoutSaved(res.error);
                        } else {
                          setLayoutSaved("Preview applied — not saved yet");
                        }
                      } catch (err) {
                        setLayoutSaved(`Preview error: ${String(err)}`);
                      }
                    }}
                    className="flex-1 px-4 py-2 bg-blue-600 text-white text-sm rounded hover:bg-blue-500"
                  >
                    Preview
                  </button>
                  <button
                    onClick={async () => {
                      setLayoutSaved(null);
                      if (layout) {
                        setLayoutForm({
                          panel_width: Number(layout.panel_width) || 64,
                          panel_height: Number(layout.panel_height) || 32,
                          panel_rows: Number(layout.panel_rows) || 1,
                          panel_cols: Number(layout.panel_cols) || 1,
                          chain_pattern: Number(layout.chain_pattern) || 0,
                          panel_rotation_deg: Number(layout.panel_rotation_deg) || 0,
                          color_order: Number(layout.color_order) || 0,
                        });
                        try {
                          const res = await previewDeviceLayout(device.ip, device.port, {
                            panel_width: Number(layout.panel_width) || 64,
                            panel_height: Number(layout.panel_height) || 32,
                            panel_rows: Number(layout.panel_rows) || 1,
                            panel_cols: Number(layout.panel_cols) || 1,
                            chain_pattern: Number(layout.chain_pattern) || 0,
                            panel_rotation_deg: Number(layout.panel_rotation_deg) || 0,
                            color_order: Number(layout.color_order) || 0,
                          }) as { ok?: boolean; error?: string };
                          if (res && res.ok === false && res.error) {
                            setLayoutSaved(res.error);
                          } else {
                            setLayoutSaved("Reverted to saved config");
                          }
                        } catch (err) {
                          setLayoutSaved(`Revert error: ${String(err)}`);
                        }
                      }
                    }}
                    className="px-4 py-2 bg-pd-bg border border-pd-border text-white text-sm rounded hover:border-gray-500"
                  >
                    Revert
                  </button>
                </div>
                <button
                  onClick={async () => {
                    setLayoutSaving(true);
                    setLayoutSaved(null);
                    try {
                      await setDeviceLayout(device.ip, device.port, layoutForm);
                      setLayoutSaved("Saved — device is rebooting…");
                      setEditLayout(false);
                      setTimeout(refreshLayout, 8000);
                    } catch (err) {
                      setLayoutSaved(`Error: ${String(err)}`);
                    } finally {
                      setLayoutSaving(false);
                    }
                  }}
                  disabled={layoutSaving}
                  className="w-full px-4 py-2 bg-purple-600 text-white text-sm rounded hover:bg-purple-500 disabled:opacity-50"
                >
                  {layoutSaving ? "Saving…" : "Save & Reboot"}
                </button>
              </div>
            )}
          </div>

          {/* Brightness */}
          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <h3 className="font-semibold text-sm mb-3">Test Brightness</h3>
            <div className="flex items-center gap-3">
              <input
                type="range"
                min={5}
                max={100}
                step={5}
                value={brightness}
                onChange={(e) => setBrightness(Number(e.target.value))}
                className="flex-1 accent-purple-500"
              />
              <span className="text-sm w-10 text-right">{brightness}%</span>
            </div>
          </div>

          {/* Test patterns */}
          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <div className="flex items-center justify-between mb-3">
              <h3 className="font-semibold text-sm">Test Patterns</h3>
              {activePattern && (
                <button
                  onClick={handleTestStop}
                  disabled={diagBusy}
                  className="px-3 py-1 text-xs bg-pd-red/20 text-pd-red rounded hover:bg-pd-red/30 disabled:opacity-50"
                >
                  Stop Pattern
                </button>
              )}
            </div>

            {diagError && (
              <div className="mb-3 text-xs text-pd-red">{diagError}</div>
            )}

            <div className="space-y-2">
              {TEST_PATTERNS.map((p) => (
                <button
                  key={p.id}
                  onClick={() => handleTestStart(p.id)}
                  disabled={diagBusy}
                  className={`w-full text-left px-3 py-2.5 rounded border text-sm transition ${
                    activePattern === p.id
                      ? "bg-purple-600/30 border-purple-500"
                      : "bg-pd-bg border-pd-border hover:border-gray-500"
                  } disabled:opacity-50`}
                >
                  <div className="font-medium">{p.label}</div>
                  <div className="text-xs text-gray-500 mt-0.5">{p.desc}</div>
                </button>
              ))}
            </div>
          </div>
        </div>
      )}

      {tab === "config" && (
        <div className="space-y-4">
          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <h3 className="font-semibold text-sm mb-3">Display Layout</h3>
            <div className="space-y-3">
              <div className="grid grid-cols-2 gap-3">
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Panel width (px)</label>
                  <input type="number" min={8} max={512} value={layoutForm.panel_width}
                    onChange={e => setLayoutForm(f => ({...f, panel_width: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                </div>
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Panel height (px)</label>
                  <input type="number" min={8} max={512} value={layoutForm.panel_height}
                    onChange={e => setLayoutForm(f => ({...f, panel_height: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                </div>
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Rows</label>
                  <input type="number" min={1} max={8} value={layoutForm.panel_rows}
                    onChange={e => setLayoutForm(f => ({...f, panel_rows: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                </div>
                <div>
                  <label className="text-xs text-gray-500 block mb-1">Cols</label>
                  <input type="number" min={1} max={8} value={layoutForm.panel_cols}
                    onChange={e => setLayoutForm(f => ({...f, panel_cols: Number(e.target.value)}))}
                    className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none" />
                </div>
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Chain pattern</label>
                <select value={layoutForm.chain_pattern}
                  onChange={e => setLayoutForm(f => ({...f, chain_pattern: Number(e.target.value)}))}
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none">
                  <option value={0}>Linear (row-by-row sequential)</option>
                  <option value={1}>Serpentine — top-left, snake down</option>
                  <option value={2}>Serpentine — top-right, snake down</option>
                  <option value={3}>Serpentine — bottom-left, snake up</option>
                  <option value={4}>Serpentine — bottom-right, snake up</option>
                  <option value={5}>Zigzag — top-left, down</option>
                  <option value={6}>Zigzag — top-right, down</option>
                  <option value={7}>Zigzag — bottom-left, up</option>
                  <option value={8}>Zigzag — bottom-right, up</option>
                </select>
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Panel mount rotation</label>
                <select value={layoutForm.panel_rotation_deg}
                  onChange={e => setLayoutForm(f => ({...f, panel_rotation_deg: Number(e.target.value)}))}
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none">
                  <option value={0}>0° (normal)</option>
                  <option value={90}>90° CW</option>
                  <option value={180}>180°</option>
                  <option value={270}>270° CW</option>
                </select>
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Color order</label>
                <select value={layoutForm.color_order}
                  onChange={e => setLayoutForm(f => ({...f, color_order: Number(e.target.value)}))}
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none">
                  <option value={0}>RGB</option>
                  <option value={1}>BGR</option>
                  <option value={2}>GRB</option>
                  <option value={3}>BRG</option>
                </select>
              </div>
              <div className="text-xs text-gray-500 pt-1">
                {(() => {
                  const rot = layoutForm.panel_rotation_deg;
                  const cw = (rot === 90 || rot === 270)
                    ? layoutForm.panel_height * layoutForm.panel_rows
                    : layoutForm.panel_width  * layoutForm.panel_cols;
                  const ch = (rot === 90 || rot === 270)
                    ? layoutForm.panel_width  * layoutForm.panel_cols
                    : layoutForm.panel_height * layoutForm.panel_rows;
                  return `Canvas will be ${cw}×${ch} px`;
                })()}
              </div>
              {layoutSaved && <div className="text-xs text-pd-green">{layoutSaved}</div>}
              <div className="flex gap-2">
                <button
                  onClick={async () => {
                    setLayoutSaved(null);
                    try {
                      const res = await previewDeviceLayout(device.ip, device.port, layoutForm) as { ok?: boolean; error?: string };
                      if (res && res.ok === false && res.error) {
                        setLayoutSaved(res.error);
                      } else {
                        setLayoutSaved("Preview applied — not saved yet");
                      }
                    } catch (err) {
                      setLayoutSaved(`Preview error: ${String(err)}`);
                    }
                  }}
                  className="flex-1 px-4 py-2 bg-blue-600 text-white text-sm rounded hover:bg-blue-500"
                >
                  Preview
                </button>
                <button
                  onClick={async () => {
                    setLayoutSaved(null);
                    if (layout) {
                      setLayoutForm({
                        panel_width: Number(layout.panel_width) || 64,
                        panel_height: Number(layout.panel_height) || 32,
                        panel_rows: Number(layout.panel_rows) || 1,
                        panel_cols: Number(layout.panel_cols) || 1,
                        chain_pattern: Number(layout.chain_pattern) || 0,
                        panel_rotation_deg: Number(layout.panel_rotation_deg) || 0,
                        color_order: Number(layout.color_order) || 0,
                      });
                      try {
                        const res = await previewDeviceLayout(device.ip, device.port, {
                          panel_width: Number(layout.panel_width) || 64,
                          panel_height: Number(layout.panel_height) || 32,
                          panel_rows: Number(layout.panel_rows) || 1,
                          panel_cols: Number(layout.panel_cols) || 1,
                          chain_pattern: Number(layout.chain_pattern) || 0,
                          panel_rotation_deg: Number(layout.panel_rotation_deg) || 0,
                          color_order: Number(layout.color_order) || 0,
                        }) as { ok?: boolean; error?: string };
                        if (res && res.ok === false && res.error) {
                          setLayoutSaved(res.error);
                        } else {
                          setLayoutSaved("Reverted to saved config");
                        }
                      } catch (err) {
                        setLayoutSaved(`Revert error: ${String(err)}`);
                      }
                    }
                  }}
                  className="px-4 py-2 bg-pd-bg border border-pd-border text-white text-sm rounded hover:border-gray-500"
                >
                  Revert
                </button>
              </div>
              <button
                onClick={async () => {
                  setLayoutSaving(true);
                  setLayoutSaved(null);
                  try {
                    await setDeviceLayout(device.ip, device.port, layoutForm);
                    setLayoutSaved("Saved — device is rebooting…");
                    setTimeout(refreshLayout, 8000);
                  } catch (err) {
                    setLayoutSaved(`Error: ${String(err)}`);
                  } finally {
                    setLayoutSaving(false);
                  }
                }}
                disabled={layoutSaving}
                className="w-full px-4 py-2 bg-purple-600 text-white text-sm rounded hover:bg-purple-500 disabled:opacity-50"
              >
                {layoutSaving ? "Saving…" : "Save & Reboot"}
              </button>
            </div>
          </div>

          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <h3 className="font-semibold text-sm mb-3">Live Test Pattern</h3>
            <select
              value={activePattern ?? ""}
              onChange={async (e) => {
                const pattern = e.target.value;
                if (!pattern) {
                  await handleTestStop();
                } else {
                  await handleTestStart(pattern);
                }
              }}
              className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm focus:border-purple-500 focus:outline-none mb-3"
            >
              <option value="">None (stop test)</option>
              {TEST_PATTERNS.map((p) => (
                <option key={p.id} value={p.id}>{p.label}</option>
              ))}
            </select>
            <p className="text-xs text-gray-500">
              Select a pattern to see live feedback as you change layout settings above.
            </p>
          </div>
        </div>
      )}
    </div>
  );
}
