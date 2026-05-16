# Development Process

How pixel-dumpster was built using AI-assisted iterative development.

---

## Overview

I work inside Windsurf IDE, almost entirely in **Planning Mode**, using multiple AI models in a deliberate pipeline: GPT-4 for open-ended exploration, then Claude 3.5 Sonnet or Opus for specifications and code generation. This isn't "vibe coding" — it's structured, audited, and iteratively validated.

---

## The Pipeline

### 1. Brainstorming with GPT-4

I start with thought experiments. GPT-4 is good at surfacing edge cases, exploring "what if" scenarios, and challenging assumptions before I commit to an architecture. This is intentionally low-stakes — I'm not asking for code, just exploring the problem space.

### 2. Concept Document

Once the direction is clear, I write a living project document (e.g., `documentation/pixel-dumpster.md`) and iterate line-by-line, adding constraints, edge cases, and behavioral notes. The model helps expand sections, but I integrate everything back into a single source of truth.

### 3. Technical Specification with Claude

I switch to Claude 3.5 Sonnet or Opus to generate a detailed spec from the concept doc — APIs, data structures, component boundaries, state machines. This is where precision matters, and Claude is better at maintaining consistency across a large specification than GPT-4.

### 4. Q&A Validation

Before writing any code, I enter a question-and-answer session with the model about the spec. I mentally test each step, verify assumptions, and surface hidden dependencies. For example: "What happens if the WiFi connection drops mid-wizard?" or "How does the transition engine handle a new play command while a transition is already running?"

### 5. Pivot Handling

Hardware doesn't always cooperate with elegant software abstractions. The ESP32/HUB75 pipeline, for instance, doesn't reliably hot-reload display configuration — it often requires a reboot. Rather than forcing an unsafe runtime reinit that produces blank or garbled output, I direct the model to:
- Add explicit warnings in the code
- Design the UX to set expectations ("Settings will apply after reboot")
- Store pending config separately from active config

### 6. Implementation & Broadening

Code is generated and integrated. Here's where I watch carefully for the model getting stuck on *my specific* hardware config. I'm testing with 7 panels at 64×224 resolution, but other users should bring whatever HUB75 panels they have. I explicitly audit for:
- Hardcoded dimensions that should be dynamic
- Assumptions about scan mode or shift driver
- Panel chain logic that only works for my count

### 7. Code Audit

I ask Claude to audit the implementation for:
- Dead ends and unreachable states
- Hardcoded values that should be configurable
- Places where the model confused "what works on my desk" with "what the software should do universally"
- Missing error handling or fallback paths

### 8. Final Q&A

One more round of targeted questions to confirm the goals are met before committing. This catches the last 10% of issues — race conditions, memory leaks, or UI states that don't match the backend.

---

## Documentation

All docs in `documentation/` are written and refined through the same iterative process. They are **not** auto-generated dumps. They are living specifications that evolve with the code, and they are treated as first-class artifacts — every behavioral decision is recorded there so future changes don't regress past work.

---

## Tools Used

| Stage | Tool |
|-------|------|
| IDE | Windsurf (Planning Mode primary), Cursor (secondary) |
| Brainstorming | GPT-4 |
| Specification | Claude 3.5 Sonnet / Opus |
| Code Generation | Claude 3.5 Sonnet / Opus |
| Audit & Review | Claude 3.5 Sonnet / Opus |
| Version Control | Git + GitHub |

---

*This document is a living description of the workflow. It evolves as the project and tooling evolve.*
