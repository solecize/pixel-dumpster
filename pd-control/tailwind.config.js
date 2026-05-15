/** @type {import('tailwindcss').Config} */
export default {
  content: ["./index.html", "./src/**/*.{js,ts,jsx,tsx}"],
  theme: {
    extend: {
      colors: {
        "pd-dark": "#0f1117",
        "pd-panel": "#1a1d27",
        "pd-bg": "#252836",
        "pd-border": "#2a2d3a",
        "pd-accent": "#6366f1",
        "pd-green": "#22c55e",
        "pd-red": "#ef4444",
        "pd-amber": "#f59e0b",
      },
    },
  },
  plugins: [],
};
