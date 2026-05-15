import { useState, useEffect, useMemo, useRef, memo } from "react";

interface PanelLayoutSVGProps {
  panelWidth: number;
  panelHeight: number;
  panelRows: number;
  panelCols: number;
  chainPattern: number;
  panelRotationDeg: number;
  testPattern?: string | null;
}

function getChainIndex(
  r: number,
  c: number,
  rows: number,
  cols: number,
  pattern: number
): number {
  switch (pattern) {
    case 0:
      return r * cols + c;
    case 1:
      return r * cols + (r % 2 === 0 ? c : cols - 1 - c);
    case 2:
      return r * cols + (r % 2 === 0 ? cols - 1 - c : c);
    case 3: {
      const rb = rows - 1 - r;
      return rb * cols + (rb % 2 === 0 ? c : cols - 1 - c);
    }
    case 4: {
      const rb = rows - 1 - r;
      return rb * cols + (rb % 2 === 0 ? cols - 1 - c : c);
    }
    case 5:
      return c * rows + (c % 2 === 0 ? r : rows - 1 - r);
    case 6: {
      const cc = cols - 1 - c;
      return cc * rows + (cc % 2 === 0 ? r : rows - 1 - r);
    }
    case 7:
      return c * rows + (c % 2 === 0 ? rows - 1 - r : r);
    case 8: {
      const cc = cols - 1 - c;
      return cc * rows + (cc % 2 === 0 ? rows - 1 - r : r);
    }
    default:
      return r * cols + c;
  }
}

/* 3×5 pixel-font bitmap for digits 0-9.
   Each digit: 5 rows of 3 bits. 1 = lit, 0 = off. */
const DIGIT_MAP: number[][] = [
  [0b111, 0b101, 0b101, 0b101, 0b111], // 0
  [0b010, 0b110, 0b010, 0b010, 0b111], // 1
  [0b111, 0b001, 0b111, 0b100, 0b111], // 2
  [0b111, 0b001, 0b111, 0b001, 0b111], // 3
  [0b101, 0b101, 0b111, 0b001, 0b001], // 4
  [0b111, 0b100, 0b111, 0b001, 0b111], // 5
  [0b111, 0b100, 0b111, 0b101, 0b111], // 6
  [0b111, 0b001, 0b001, 0b001, 0b001], // 7
  [0b111, 0b101, 0b111, 0b101, 0b111], // 8
  [0b111, 0b101, 0b111, 0b001, 0b111], // 9
];

/* Background fill for a pixel (ball overlay is handled separately).
   vx = visual x-coordinate for rgb_sweep; vw = visual width. */
function getPixelColor(
  px: number,
  displayW: number,
  gx: number,
  gy: number,
  pattern: string | null | undefined,
  vx: number,
  vw: number
): string {
  switch (pattern) {
    case 'color_test': {
      const t = px / displayW;
      if (t < 1 / 3) return '#CC2020';
      if (t < 2 / 3) return '#20AA20';
      return '#2020CC';
    }
    case 'checkerboard':
      return (gx + gy) % 2 === 0 ? '#FFFFFF' : '#111111';
    case 'rgb_sweep': {
      const t = vw > 1 ? vx / (vw - 1) : 0;
      if (t <= 0.5) {
        const u = t * 2;
        return `rgb(${Math.round(255 * (1 - u))},${Math.round(255 * u)},0)`;
      }
      const u = (t - 0.5) * 2;
      return `rgb(0,${Math.round(255 * (1 - u))},${Math.round(255 * u)})`;
    }
    case 'bouncing_ball':
      return '#1a1a2e';
    default:
      return '#808080';
  }
}

function rotatePoint(
  x: number,
  y: number,
  deg: number,
  cx: number,
  cy: number
): [number, number] {
  const rad = (deg * Math.PI) / 180;
  const cos = Math.cos(rad);
  const sin = Math.sin(rad);
  const dx = x - cx;
  const dy = y - cy;
  return [cx + dx * cos - dy * sin, cy + dx * sin + dy * cos];
}

/* Map original pixel (gx, gy) → visual x-index given the normalised SVG rotation.
   normSvgRot = ((-panelRotationDeg) % 360 + 360) % 360. */
function getVisualX(
  gx: number,
  gy: number,
  totalDisplayW: number,
  totalDisplayH: number,
  normSvgRot: number
): number {
  switch (normSvgRot) {
    case 90:  return totalDisplayH - 1 - gy;
    case 180: return totalDisplayW - 1 - gx;
    case 270: return gy;
    default:  return gx;
  }
}

/* Map visual (vx, vy) → original pixel (gx, gy) given the normalised SVG rotation. */
function fromVisual(
  vx: number,
  vy: number,
  totalDisplayW: number,
  totalDisplayH: number,
  normSvgRot: number
): [number, number] {
  switch (normSvgRot) {
    case 90:  return [vy,                       totalDisplayH - 1 - vx];
    case 180: return [totalDisplayW - 1 - vx,   totalDisplayH - 1 - vy];
    case 270: return [totalDisplayW - 1 - vy,   vx];
    default:  return [vx, vy];
  }
}

export const PanelLayoutSVG = memo(function PanelLayoutSVG({
  panelWidth,
  panelHeight,
  panelRows,
  panelCols,
  chainPattern,
  panelRotationDeg,
  testPattern,
}: PanelLayoutSVGProps) {
  const rows = Math.max(1, panelRows);
  const cols = Math.max(1, panelCols);

  /* Scale display resolution while preserving aspect ratio, capped at 64 */
  const maxDisplayPx = 64;
  const aspectScale = Math.min(maxDisplayPx / panelWidth, maxDisplayPx / panelHeight, 1);
  const displayW = Math.max(1, Math.round(panelWidth * aspectScale));
  const displayH = Math.max(1, Math.round(panelHeight * aspectScale));

  const gap = 6;
  const vw = 400;
  const vh = 300;
  const pad = 16;
  const availW = vw - pad * 2;
  const availH = vh - pad * 2;

  /* When the display is rotated 90°/270°, rows/cols swap visually, so
     size the panels to fit within the transposed available space. */
  const isTransposed = panelRotationDeg === 90 || panelRotationDeg === 270 || panelRotationDeg === -90;
  const sizeAvailW = isTransposed ? availH : availW;
  const sizeAvailH = isTransposed ? availW : availH;

  /* Scale to fit all panels at their actual aspect ratio */
  const maxScaleW = (sizeAvailW - gap * (cols - 1)) / (panelWidth * cols);
  const maxScaleH = (sizeAvailH - gap * (rows - 1)) / (panelHeight * rows);
  const scale = Math.min(maxScaleW, maxScaleH);
  const drawW = panelWidth * scale;
  const drawH = panelHeight * scale;

  const totalW = cols * drawW + (cols - 1) * gap;
  const totalH = rows * drawH + (rows - 1) * gap;
  const offX = (vw - totalW) / 2;
  const offY = (vh - totalH) / 2;

  const pxSize = drawW / displayW;
  const totalDisplayW = cols * displayW;
  const totalDisplayH = rows * displayH;

  /* The SVG rotates by the NEGATIVE of the hardware angle so the coordinate
     convention matches the firmware (firmware 90° = CCW in screen space). */
  const svgRotDeg = -panelRotationDeg;
  const normSvgRot = ((svgRotDeg % 360) + 360) % 360;

  /* Visual (post-rotation) display dimensions */
  const visualW = isTransposed ? totalDisplayH : totalDisplayW;
  const visualH = isTransposed ? totalDisplayW : totalDisplayH;

  /* --- State ---------------------------------------------------------- */

  /* Ball position stored in visual space so it bounces like the hardware */
  const [ball, setBall] = useState({ x: 0, y: 0, vx: 1.5, vy: 1.0 });

  useEffect(() => {
    if (testPattern !== 'bouncing_ball') return;
    const id = setInterval(() => {
      setBall(b => {
        let { x, y, vx, vy } = b;
        x += vx;
        y += vy;
        if (x <= 0 || x >= visualW - 1) { vx = -vx; x = Math.max(0, Math.min(visualW - 1, x)); }
        if (y <= 0 || y >= visualH - 1) { vy = -vy; y = Math.max(0, Math.min(visualH - 1, y)); }
        return { x, y, vx, vy };
      });
    }, 80);
    return () => clearInterval(id);
  }, [testPattern, visualW, visualH]);

  /* Ref for the pixel <g> so rgb_sweep can animate via direct DOM mutation,
     bypassing React reconciliation entirely (zero-flicker approach). */
  const pixelGroupRef = useRef<SVGGElement>(null);

  useEffect(() => {
    if (testPattern !== 'rgb_sweep') return;
    const vw = visualW;
    /* Pre-compute visual X for every pixel in the same iteration order as
       staticPixels (r → c → py → px), so index i aligns with DOM child i. */
    const vxs: number[] = [];
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        for (let py = 0; py < displayH; py++) {
          for (let px = 0; px < displayW; px++) {
            const gx = c * displayW + px;
            const gy = r * displayH + py;
            vxs.push(getVisualX(gx, gy, totalDisplayW, totalDisplayH, normSvgRot));
          }
        }
      }
    }
    let offset = 0;
    const id = setInterval(() => {
      const group = pixelGroupRef.current;
      if (!group) return;
      const children = group.children;
      const len = Math.min(children.length, vxs.length);
      for (let i = 0; i < len; i++) {
        const vx = vxs[i];
        /* Right-to-left scroll: (vx - offset) so red shifts left as offset grows */
        const shifted = ((vx - offset) % vw + vw) % vw;
        const t = vw > 1 ? shifted / (vw - 1) : 0;
        let fill: string;
        if (t <= 0.5) {
          const u = t * 2;
          fill = `rgb(${Math.round(255 * (1 - u))},${Math.round(255 * u)},0)`;
        } else {
          const u = (t - 0.5) * 2;
          fill = `rgb(0,${Math.round(255 * (1 - u))},${Math.round(255 * u)})`;
        }
        (children[i] as SVGRectElement).setAttribute('fill', fill);
      }
      offset = (offset + 1) % Math.max(1, vw);
    }, 80);
    return () => clearInterval(id);
  }, [testPattern, rows, cols, displayW, displayH, totalDisplayW, totalDisplayH, normSvgRot, visualW]);

  /* --- Memoised computations ------------------------------------------ */

  const { grid, posByIndex } = useMemo(() => {
    const g: number[][] = [];
    const p: (null | { r: number; c: number })[] = Array(rows * cols).fill(null);
    for (let r = 0; r < rows; r++) {
      g[r] = [];
      for (let c = 0; c < cols; c++) {
        const idx = getChainIndex(r, c, rows, cols, chainPattern);
        g[r][c] = idx;
        p[idx] = { r, c };
      }
    }
    return { grid: g, posByIndex: p };
  }, [rows, cols, chainPattern]);

  /* Static pixel background layer — recomputes only when layout / pattern / sweep changes */
  const staticPixels = useMemo(() => {
    const border = pxSize / 12;
    const els: JSX.Element[] = [];
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        const panelX = offX + c * (drawW + gap);
        const panelY = offY + r * (drawH + gap);
        for (let py = 0; py < displayH; py++) {
          for (let px = 0; px < displayW; px++) {
            const gx = c * displayW + px;
            const gy = r * displayH + py;
            const vx = getVisualX(gx, gy, totalDisplayW, totalDisplayH, normSvgRot);
            const fill = getPixelColor(px, displayW, gx, gy, testPattern, vx, visualW);
            els.push(
              <rect
                key={`px-${r}-${c}-${py}-${px}`}
                x={panelX + px * pxSize + border}
                y={panelY + py * pxSize + border}
                width={pxSize - 2 * border}
                height={pxSize - 2 * border}
                fill={fill}
                stroke="#000"
                strokeWidth={border}
              />
            );
          }
        }
      }
    }
    return els;
  }, [rows, cols, displayW, displayH, drawW, drawH, offX, offY, pxSize,
      totalDisplayW, totalDisplayH, normSvgRot, visualW, testPattern]);

  /* Ball overlay — only 9 iterations (3×3 neighborhood), recomputes on ball tick */
  const ballPixels = useMemo(() => {
    if (testPattern !== 'bouncing_ball') return null;
    const border = pxSize / 12;
    const els: JSX.Element[] = [];
    const bvx = Math.round(ball.x);
    const bvy = Math.round(ball.y);
    for (let dvx = -1; dvx <= 1; dvx++) {
      for (let dvy = -1; dvy <= 1; dvy++) {
        const vx = bvx + dvx;
        const vy = bvy + dvy;
        if (vx < 0 || vx >= visualW || vy < 0 || vy >= visualH) continue;
        const [gx, gy] = fromVisual(vx, vy, totalDisplayW, totalDisplayH, normSvgRot);
        if (gx < 0 || gx >= totalDisplayW || gy < 0 || gy >= totalDisplayH) continue;
        const pc = Math.floor(gx / displayW);
        const px = gx % displayW;
        const pr = Math.floor(gy / displayH);
        const py = gy % displayH;
        if (pc >= cols || pr >= rows) continue;
        const panelX = offX + pc * (drawW + gap);
        const panelY = offY + pr * (drawH + gap);
        els.push(
          <rect
            key={`ball-${vx}-${vy}`}
            x={panelX + px * pxSize + border}
            y={panelY + py * pxSize + border}
            width={pxSize - 2 * border}
            height={pxSize - 2 * border}
            fill="#FFFFFF"
            stroke="#000"
            strokeWidth={border}
          />
        );
      }
    }
    return els.length > 0 ? <>{els}</> : null;
  }, [testPattern, ball, rows, cols, displayW, displayH, drawW, drawH, offX, offY,
      pxSize, totalDisplayW, totalDisplayH, normSvgRot, visualW, visualH]);

  /* Panel numbers — upright in screen space regardless of rotation.
     Uses negated angle so the corner lookup matches the negated SVG transform. */
  const numbers = useMemo(() => {
    const els: JSX.Element[] = [];
    const normRot = ((-panelRotationDeg % 360) + 360) % 360;
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        const panelX = offX + c * (drawW + gap);
        const panelY = offY + r * (drawH + gap);
        const idx = grid[r][c];
        const numStr = String(idx + 1);
        const numDigitW = 3;
        const numSpacing = 1;
        const numTotalW = numStr.length * numDigitW + (numStr.length - 1) * numSpacing;
        const numInset = 2 * pxSize;
        let srcUrX: number, srcUrY: number;
        if (normRot === 90)       { srcUrX = panelX;         srcUrY = panelY; }
        else if (normRot === 180) { srcUrX = panelX;         srcUrY = panelY + drawH; }
        else if (normRot === 270) { srcUrX = panelX + drawW; srcUrY = panelY + drawH; }
        else                      { srcUrX = panelX + drawW; srcUrY = panelY; }
        const [urX, urY] = rotatePoint(srcUrX, srcUrY, -panelRotationDeg, vw / 2, vh / 2);
        const numStartX = urX - numInset - numTotalW * pxSize;
        const numStartY = urY + numInset;
        for (let i = 0; i < numStr.length; i++) {
          const digit = parseInt(numStr[i], 10);
          const digitMap = DIGIT_MAP[digit];
          const digitX = numStartX + i * (numDigitW + numSpacing) * pxSize;
          for (let row = 0; row < 5; row++) {
            const bits = digitMap[row];
            for (let col = 0; col < 3; col++) {
              if (bits & (1 << (2 - col))) {
                els.push(
                  <use
                    key={`n-${r}-${c}-${i}-${row}-${col}`}
                    href="#pixel-on"
                    x={digitX + col * pxSize}
                    y={numStartY + row * pxSize}
                    width={pxSize}
                    height={pxSize}
                  />
                );
              }
            }
          }
        }
      }
    }
    return els;
  }, [rows, cols, drawW, drawH, offX, offY, pxSize, grid, panelRotationDeg]);

  /* Chain-order arrows */
  const arrows = useMemo(() => {
    const els: JSX.Element[] = [];
    for (let i = 0; i < rows * cols - 1; i++) {
      const from = posByIndex[i];
      const to = posByIndex[i + 1];
      if (!from || !to) continue;
      const fx = offX + from.c * (drawW + gap) + drawW / 2;
      const fy = offY + from.r * (drawH + gap) + drawH / 2;
      const tx = offX + to.c * (drawW + gap) + drawW / 2;
      const ty = offY + to.r * (drawH + gap) + drawH / 2;
      const midX = (fx + tx) / 2;
      const midY = (fy + ty) / 2;
      let rotation = 0;
      if (tx > fx) rotation = 0;
      else if (tx < fx) rotation = 180;
      else if (ty > fy) rotation = 90;
      else if (ty < fy) rotation = -90;
      const arrowSize = Math.min(gap * 1.5, Math.min(drawW, drawH) * 0.35, 14);
      els.push(
        <g key={`a-${i}`} transform={`translate(${midX},${midY}) rotate(${rotation})`}>
          <use
            href="#arrow-right"
            x={-arrowSize / 2}
            y={-arrowSize / 2}
            width={arrowSize}
            height={arrowSize}
          />
        </g>
      );
    }
    return els;
  }, [rows, cols, drawW, drawH, offX, offY, posByIndex]);

  /* Rotation indicator */
  let rotationEl: JSX.Element | null = null;
  if (panelRotationDeg !== 0) {
    const rotSize = 22;
    const rx = vw - rotSize / 2 - 8;
    const ry = rotSize / 2 + 8;
    rotationEl = (
      <g transform={`translate(${rx},${ry})`}>
        <use
          href="#rotation"
          x={-rotSize / 2}
          y={-rotSize / 2}
          width={rotSize}
          height={rotSize}
        />
        <text x={0} y={-rotSize / 2 - 3} textAnchor="middle" fontSize={9} fill="#006837">
          {panelRotationDeg}°
        </text>
      </g>
    );
  }

  const dimLabel = `${panelWidth}×${panelHeight} px × ${cols}×${rows} grid`;

  return (
    <div className="w-full">
      <svg
        viewBox={`0 0 ${vw} ${vh}`}
        className="w-full h-auto rounded-lg"
        style={{ background: '#0f1117', border: '1px solid #2a2d3a' }}
      >
        <defs>
          <symbol id="pixel-off" viewBox="0 0 6 6">
            <rect x="0.5" y="0.5" width="5" height="5" fill="#808080" />
            <path d="M5,1v4H1V1H5 M6,0H0v6h6V0L6,0z" />
          </symbol>
          <symbol id="pixel-on" viewBox="0 0 6 6">
            <rect x="0.5" y="0.5" width="5" height="5" fill="#FFFFFF" />
            <path d="M5,1v4H1V1H5 M6,0H0v6h6V0L6,0z" />
          </symbol>
          <symbol id="arrow-right" viewBox="0 0 16 16">
            <polygon fill="#ED1C24" points="6.378,4.965 9.414,8 6.378,11.035" />
            <path
              fill="#7B250B"
              d="M6.879,6.172L8.707,8L6.879,9.828V6.172 M5.879,3.757v8.485L10.121,8L5.879,3.757L5.879,3.757z"
            />
          </symbol>
          <symbol id="rotation" viewBox="0 0 16 16">
            <path
              fill="none"
              stroke="#006837"
              strokeMiterlimit="10"
              d="M11.139,8.799c0-2.209-1.791-4-4-4s-4,1.791-4,4s1.791,4,4,4"
            />
            <polygon fill="#00FF00" points="8.068,8.701 12.361,4.408 12.361,8.701" />
            <path
              fill="#006837"
              d="M11.861,5.615v2.586H9.275L11.861,5.615 M12.861,3.201l-6,6h6V3.201L12.861,3.201z"
            />
          </symbol>
        </defs>

        <rect x={0} y={0} width={vw} height={vh} fill="#0f1117" />

        <g transform={panelRotationDeg !== 0 ? `rotate(${svgRotDeg},${vw / 2},${vh / 2})` : undefined}>
          <g ref={pixelGroupRef}>
            {staticPixels}
            {ballPixels}
          </g>
          {arrows}
        </g>
        {numbers}
        {rotationEl}

        <text x={vw / 2} y={vh - 8} textAnchor="middle" fontSize={10} fill="#94a3b8">
          {dimLabel}
        </text>
      </svg>
    </div>
  );
});
