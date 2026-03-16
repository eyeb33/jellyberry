# Memory Persistence Plan

> **Status: Deferred** — implement as an isolated sprint once hardware assembly is stable and end-to-end device is reliable. All changes are server-only (`server/main.ts`), no firmware touchpoints.

---

## The Problem

Gemini Live sessions have a hard 10-minute limit. The server already handles this gracefully with a proactive 9-minute renewal, but every renewal is a fresh Gemini session — conversational context is lost silently. Jellyberry forgets your name, preferences, and anything discussed in prior sessions.

---

## Approach: Inject Memory Into Each New Session

We cannot extend the session, so instead we rebuild context on every reconnect by injecting a small memory block into Gemini's system prompt. Memory is stored in **Deno KV** — SQLite locally today, migrates to Deno Deploy's hosted KV unchanged when moving to cloud.

### Cost
~150 tokens injected per session renewal. At 2hrs active use/day (~13 renewals): **~$0.0003/day**. Negligible.

---

## Two-Tier Design

### Hot Tier — always injected (~150 token cap)
- A curated facts object: name, key preferences, current projects/context
- Stored at KV key `["memory", "facts"]`
- Injected as a `"Memory:"` block in `systemInstruction` on every `setupMessage`
- `readHotMemory()` enforces a hard character cap (~600 chars) to prevent token creep
- If KV is empty (first boot), block is omitted — graceful fallback

### Cold Tier — on-demand only (zero cost unless called)
- 7-day rolling session summaries stored at `["memory", "sessions", ISO_DATE]`
- NOT injected automatically
- Retrieved only when Gemini calls the `recall_sessions` tool (e.g. user says "remember when we talked about...")

---

## New Tools

### `store_memory`
| Parameter | Type | Values |
|---|---|---|
| `category` | enum | `"user_fact"` / `"preference"` / `"note"` |
| `value` | string | The fact to store |

- Gemini calls this naturally when it learns something worth keeping (name, habits, preferences, ongoing projects)
- Should NOT be called for transient info (today's weather, current timer state)
- Handler calls `updateFact(category, value)` → upserts into hot tier KV

### `recall_sessions`
| Parameter | Type | Default |
|---|---|---|
| `days_back` | int (optional) | 3 |

- Gemini calls this when past-session context is explicitly relevant
- Handler calls `readColdMemory(daysBack)` → returns formatted session summaries
- Zero token cost in normal conversation

---

## Session Summary Generation
- Server maintains a lightweight transcript log per session (text from `modelTurn` parts)
- On `proactiveRenew()`, if `userSpokeThisTurn` was true this session:
  - POST to Gemini REST text API (non-live) with transcript
  - Request a 2-sentence summary of what was discussed + any notable facts
  - Store under `["memory", "sessions", ISO_DATE]` (cold tier)
  - Prune entries older than 7 days

---

## Implementation Steps

### Phase 1 — Storage layer (foundation for all phases)
1. Add Deno KV init: `const kv = await Deno.openKv()`
2. `readHotMemory(): Promise<string>` — reads facts object, formats as bullets, caps at ~600 chars
3. `updateFact(category, value)` — upserts into `["memory", "facts"]`
4. `readColdMemory(daysBack: number): Promise<string>` — reads last N days of session summaries

### Phase 2 — Context injection *(depends on Phase 1)*
5. In `connectToGemini()`, call `readHotMemory()` before building `setupMessage`
6. If non-empty, append as `"Memory:"` block in `systemInstruction`

### Phase 3 — `store_memory` tool *(parallel with Phase 2)*
7. Add `store_memory` to tool registry in `setupMessage`
8. Add handler to `toolHandlers` map
9. Update system prompt: instruct Jellyberry to call it naturally for persistent facts, not transient state

### Phase 4 — `recall_sessions` tool *(parallel with Phase 2/3)*
10. Add `recall_sessions` to tool registry
11. Add handler calling `readColdMemory()`

### Phase 5 — Session summary generation *(depends on Phase 1)*
12. Add transcript log (text from `modelTurn` parts) to server session state
13. In `proactiveRenew()`: summarise via Gemini REST if session had user speech
14. Store in cold tier KV, prune >7 days old

---

## Relevant Files
- `server/main.ts` — all changes (setupMessage, toolHandlers, connectToGemini, proactiveRenew)
- `server/deno.json` — verify KV permissions if needed

## Verification
1. Tell Jellyberry your name + a preference
2. Wait for 9-min proactive renewal → ask "do you remember my name?" — should answer correctly
3. Kill and restart the server process entirely → ask again — should still know (KV persists on disk)
4. Check cold-tier session summaries accumulate correctly in KV

---

## Cloud Migration Path
Deno KV is the right storage choice specifically because `Deno.openKv()` is identical locally and on Deno Deploy. When the server moves to cloud, this feature requires zero storage-layer changes.

## Privacy Note
All memory stored as plain text. When moving to cloud, consider whether encryption at rest is needed for the facts/sessions KV entries.
