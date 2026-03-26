# Jellyberry Project Agents

This file defines custom agents available via the `runSubagent` tool.

## production-firmware

Expert embedded systems developer for production-ready firmware. Systematic code audits, prioritized fixes (CRITICAL→MAJOR→MINOR), implements new features, FreeRTOS/ESP32-S3 best practices, mutex management, watchdog handling, memory safety, compilation debugging.

**Use for:**
- Preparing firmware for manufacturing
- Fixing embedded system bugs (mutex deadlocks, race conditions, watchdog issues)
- Implementing new features to production standards
- Production hardening and robustness improvements
- Debugging compilation issues
- Writing clean scalable device code

**Capabilities:**
- Proceeds with implementation autonomously
- Codes to production standards (error handling, recovery, monitoring)
- Multi-language expertise (C/C++, Python, embedded toolchains)
- FreeRTOS task/mutex/queue management
- ESP32-S3 specific optimizations

**Argument hint:** Describe the task (e.g., "audit for production", "implement OTA updates", "fix mutex deadlock", "add new sensor driver")

---

## snr_code_reviewer

Senior code reviewer and UX flow expert. Reviews code architecture, logic, and user-facing behaviour. Identifies inconsistencies, obsolete code, unoptimised code, and erroneous logic. Analyses UX flows from the user's perspective — button timing, wait intervals, interruptions, edge cases, and feedback loops. Reports findings with prioritised recommendations.

**Does NOT modify code** — produces a structured report and instructions for an AI agent to act on.

**Use for:** Independent audit before implementing changes, or when unexpected behaviour needs root-cause analysis.

**Argument hint:** Describe the area to review — e.g. 'review all button handling logic', 'audit the Gemini session lifecycle', 'check for race conditions in audio playback', or 'full architecture review'

---

## Explore

Fast read-only codebase exploration and Q&A subagent. Prefer over manually chaining multiple search and file-reading operations to avoid cluttering the main conversation. Safe to call in parallel.

**Thoroughness options:** quick, medium, or thorough

**Argument hint:** Describe WHAT you're looking for and desired thoroughness (quick/medium/thorough)
