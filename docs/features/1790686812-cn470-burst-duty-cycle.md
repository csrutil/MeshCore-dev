# CN470 burst duty-cycle TX policy

## Goal

Add a selectable TX policy for CN470 builds:

- Send queued packets back-to-back, without switching the radio to RX between them, while the total TX airtime of the burst is ≤ `TX_BURST_MAX_MS` (default 1000 ms).
- When the next packet does not fit, stop TX for `TX_BURST_QUIET_MS` (X) and switch to RX. Then start the next burst.
- When the queue empties before the limit, switch to RX.
- Keep the existing airtime-budget behavior as the default policy. Select the policy at compile time with a `#define`.

Out of scope: runtime policy switching, CLI/pref changes, RF-region detection, the regulatory value of X.

## Plan

### 2026-09-29 21:00 +08 — Claude (with TSAO) — claude-opus-5-5 — medium

#### Current state (verified in source)

- `src/Dispatcher.cpp` has one TX limiter: a token bucket. It refills at `1/(1+getAirtimeBudgetFactor())` over `getDutyCycleWindowMs()` (1 h). It is checked in `checkSend()` and debited in `loop()` when a send completes.
- `loop()` calls `checkRecv()` before `checkSend()`. `checkRecv()` → `RadioLibWrapper::recvRaw()` calls `startReceive()` when `state != STATE_RX`. After a TX, `onSendFinished()` sets `STATE_IDLE`. So today the radio always returns to RX between packets.
- `getNextOutbound(now)` pops the highest-priority packet whose scheduled time has passed. The priority is lost after the pop. `StaticPoolPacketManager` is the only `PacketManager` implementation.
- `Mesh::getRetransmitDelay()` = `rng(0..4) × (airtime × 52/50 / 2)`, which can be 0. `getDirectRetransmitDelay()` = 0.
- RSSI/CAD LBT is off by default (`getInterferenceThreshold()` = 0, `getCADEnabled()` = false).
- `getRemainingTxBudget()` has no callers outside `Dispatcher`.
- The code has no RF-region concept. `RegionMap` is flood scope, not RF region.
- Host tests: `[env:native]` in `platformio.ini`, googletest, with `build_src_filter` listing the sources.

#### Decisions

1. **Burst accounting.** `burst_used` counts estimated airtime. It goes back to 0 only after ≥ X ms without TX. When the queue empties early, the radio switches to RX. A packet that arrives later and still fits (`burst_used + a <= B`) is sent without waiting. If it does not fit, TX waits until `last_tx_end + X`. Any run of TX with gaps < X therefore stays ≤ B. (Assumption proposed by Claude. TSAO has not confirmed it explicitly.)
2. **Retransmit delays are kept.** A packet is only taken when its scheduled time has passed. The queue is peeked again with the current `now` before each chained packet, so a packet whose delay ends during a burst joins it. Reason: with LBT off by default, the random flood delay is the only thing that stops neighbouring repeaters from transmitting at the same time.
3. **Policy adapter.** A `TxPolicy` interface. `AirtimeBudgetTxPolicy` contains today's logic unchanged. `BurstTxPolicy` is the new rule.
4. **Defines.** `TX_POLICY_BURST` selects the burst policy. `TX_BURST_MAX_MS` defaults to 1000. `TX_BURST_QUIET_MS` has no default: `#error` if `TX_POLICY_BURST` is defined without it.

#### Design

`src/TxPolicy.h` / `src/TxPolicy.cpp` (core, because `Dispatcher` uses it; no Arduino dependencies, so it can be tested on the host). Draft interface; adjust during implementation if needed and record why:

```cpp
class TxPolicy {
public:
  virtual void begin(unsigned long now) = 0;
  // May a new TX of this airtime start now? If not, set retry_at.
  virtual bool canStart(unsigned long now, uint32_t pkt_airtime, uint32_t mtu_airtime,
                        unsigned long& retry_at) = 0;
  virtual void onTxDone(unsigned long now, uint32_t est_airtime, uint32_t actual_ms) = 0;
  // Send this next ready packet now, without switching to RX?
  virtual bool canChain(uint32_t pkt_airtime) const { return false; }
  virtual unsigned long remainingTxMs(unsigned long now) const = 0;
};
```

- `AirtimeBudgetTxPolicy`: moves `tx_budget_ms`, `last_budget_update`, `updateTxBudget()` and the `next_tx_time` calculation out of `Dispatcher` without changing their logic (including `MIN_TX_BUDGET_RESERVE_MS`, `MIN_TX_BUDGET_AIRTIME_DIV`, the MTU-based check, and the debit by measured `t`). It reads the airtime factor and window at runtime (the pref can change), for example through arguments or a small callback to the `Dispatcher` virtuals `getAirtimeBudgetFactor()` / `getDutyCycleWindowMs()`. `canChain()` returns false.
- `BurstTxPolicy`: state `burst_used_ms`, `last_tx_end`. Counts estimated airtime (`getEstAirtimeFor(len)`). `canStart()` resets `burst_used_ms` when `now - last_tx_end >= X`, allows TX when `burst_used_ms + a <= B`, otherwise sets `retry_at = last_tx_end + X`. `canChain(a)` is true when `burst_used_ms + a <= B`. Use wrap-safe `millis` arithmetic.

`Dispatcher`:

- Holds a `TxPolicy*` to a static/member instance chosen by the `#define`. No heap allocation and no change to subclass constructors.
- `checkSend()`: peek the next ready packet and compute its length/airtime. Ask `canStart()`. If false, set `next_tx_time = retry_at`. A packet whose own airtime is > B can never be sent: pop it, log it, release it.
- `loop()`, TX complete: call `onTxDone()`. Then peek the next ready packet. If `canChain()` is true, call `onSendFinished()`, then send it right away (skip the `isReceiving()` check) and `return` before `checkRecv()`. Otherwise go back to RX as today.
- The `isReceiving()` channel check runs only before the first packet of a burst.
- `getRemainingTxBudget()` calls `remainingTxMs()`.
- TX timeout inside a burst: end the burst, switch to RX, start the quiet period X.
- Share the packet-serialization code between the first send and chained sends. Do not duplicate it.

`PacketManager` / `StaticPoolPacketManager`: add `peekNextOutbound(now)`. It returns the same packet as `getNextOutbound(now)` and leaves it in the queue.

#### Validation

- Host tests in `test/test_tx_policy/` (googletest, `[env:native]`; add `src/TxPolicy.cpp` to `build_src_filter`):
  - Budget policy: same results as the current formulas for refill, cap at the window maximum, and `retry_at`.
  - Burst policy: chain until B, then wait X. A gap ≥ X resets the counter; a gap < X does not. A short burst followed by a packet that fits sends it without waiting. A single packet with airtime > B is rejected. `millis` wrap-around.
- `Dispatcher` chaining (no RX between packets): `Dispatcher.cpp` is not in the native build and there is no radio mock. Add a test only if it needs few new mocks; otherwise record a proof gap.
- TSAO asked not to build. Tests are written but not run; results are `unverified` until TSAO runs `pio test -e native` and a CN470 target build.
- Hardware (TSAO): check with an SDR or the TX pin that bursts are ≤ 1000 ms, that silence after a burst is ≥ X, and that nothing is received during a burst.

#### Risks

- At SF10 / BW250, a packet of maximum length takes about 1.15 s (estimate, not checked on hardware). With the burst policy, such packets are dropped. A CN470 preset must keep the maximum-length packet under B.
- Moving the budget into a policy changes the path every build uses. The budget-policy tests guard against this.
- The radio cannot receive during a burst (≤ B). This is well under the 8 s non-RX watchdog.

### 2026-09-29 21:35 +08 — Claude (with TSAO) — claude-opus-5-5 — medium — Revision 1: runtime `tx_policy` CLI config

Why: the burst policy is not only for CN470. TSAO also wants it for US, so one firmware must support both policies with values set per node. TSAO decisions: select the policy by the CLI config `tx_policy`; set it by CLI; no lock define.

Replaces decision 4 (compile-time `#define`s). Decisions 1–3 stay.

Behavior:

- CLI (in `CommonRadioPrefs::handleCommand`, next to `af` / `dutycycle`):
  - `get tx_policy` → `> budget` or `> burst <max_ms> <quiet_ms>`.
  - `set tx_policy budget`.
  - `set tx_policy burst <max_ms> <quiet_ms>`. Both values are required (there is no safe default for the quiet time). `max_ms` 1..8000 (above 8000 every burst trips the 8 s non-RX watchdog flag). `quiet_ms` ≥ 1. Error reply on bad input, the same style as `set af`.
- Saved prefs: `tx_policy` (uint8: 0 = budget, 1 = burst), `burst_max_ms`, `burst_quiet_ms`, via `def(...)` in both `NodePrefs` copies (`src/helpers/CommonCLI.h`, `examples/companion_radio/NodePrefs.h`). Defaults: budget, 1000, 0. A prefs file without these keys must load as budget (verify how `ConfigSerializer` handles missing keys). Do not add them to the legacy binary loader.
- `CommonRadioPrefs` gets getter/setter pairs for the three values, implemented in both `RadioPrefs`.
- `Dispatcher`: remove `TX_POLICY_BURST` / `TX_BURST_MAX_MS` / `TX_BURST_QUIET_MS` and the `#error`. Add virtuals `getTxPolicyMode()` (default budget), `getBurstMaxTxMs()`, `getBurstQuietMs()`. Hold both policy objects. When the mode changes and no send is in flight, switch `tx_policy` and call `begin(now)` on the new policy. `BurstTxPolicy` reads B and X at runtime through `TxPolicyEnv` (like the budget policy reads `af`), not constructor constants. The oversized-packet drop uses the runtime B and only applies in burst mode.
- Examples: override the three virtuals from `_prefs` wherever `getAirtimeBudgetFactor()` is overridden from `NodePrefs` (repeater, room server, sensor, companion). `simple_secure_chat` has its own prefs without `CommonRadioPrefs`: leave it on the default budget policy.
- Docs: add the new CLI commands to `docs/cli_commands.md` next to `af` / `dutycycle`.

Known effects:

- Switching to budget calls `begin()`, which gives a full budget at once (the same as at boot).
- Changing B or X while in burst mode applies from the next check. If `burst_used` is already above a smaller new B, TX waits for the quiet period.
- US values (burst limit, quiet time, or other rules such as dwell time) are not checked. The config only provides the mechanism.

Validation:

- One `[env:native]` run covers both policies (remove `[env:native_burst]`, since the policy is no longer compile-time).
- Tests: policy tests with runtime B/X; Dispatcher in budget and burst mode; switching mode at runtime (no switch while a send is in flight); CLI parse and validation for `get/set tx_policy`; prefs round-trip and a prefs file without the new keys, if the native env can reach these classes with few new mocks, otherwise a recorded proof gap.
- TSAO asked not to build: all results `unverified` until `pio test -e native` and firmware builds run.

## Work Log

### 2026-09-29 21:10 +08 — Claude — unknown — not specified

Changes:

- `src/TxPolicy.h` / `src/TxPolicy.cpp` (new): `TxPolicy` interface, `AirtimeBudgetTxPolicy` (token-bucket logic moved verbatim from `Dispatcher`: refill, cap at window max, MTU-based check, debit by measured airtime, `MIN_TX_BUDGET_RESERVE_MS` / `MIN_TX_BUDGET_AIRTIME_DIV`), and `BurstTxPolicy` (`burst_used_ms` / `last_tx_end`, quiet-period reset, `canChain`, `onTxAborted` ends the burst and starts the quiet period).
- `src/Dispatcher.h` / `src/Dispatcher.cpp`: policy selected at compile time. `#if defined(TX_POLICY_BURST)` member `burst_policy` + `tx_policy` pointer, else `budget_policy` (no heap, no subclass-ctor change; ctor member-init list picks one). `checkSend()` now peeks the next ready packet via `peekNextOutbound()`, drops (pop + log + release) packets whose own airtime exceeds `TX_BURST_MAX_MS` (burst builds only), asks `canStart()` and applies `retry_at` to `next_tx_time`; serialization moved into new private `startOutboundSend(Packet*)` shared by the first send and chained sends. `loop()` TX-complete path calls `onTxDone()`, then if `canChain()` pops the next ready packet and starts it and returns before `checkRecv()` (which would restart Rx); the `isReceiving()`/CAD check stays only in `checkSend()`, i.e. before the first packet of a burst. TX timeout calls `onTxAborted()`. `getRemainingTxBudget()` delegates to the policy.
- `src/helpers/StaticPoolPacketManager.*`: `PacketManager::peekNextOutbound(now)` (pure virtual, only implementer in repo is `StaticPoolPacketManager`); `PacketQueue::get()` refactored onto a new `bestIndex(now)` used by both `get()` and `peek()` — identical selection, priority ties resolved the same way (first match wins).
- `platformio.ini`: `src/TxPolicy.cpp`, `src/Dispatcher.cpp`, `src/helpers/StaticPoolPacketManager.cpp` added to `[env:native]` `build_src_filter` (Dispatcher compiled WITHOUT the burst define, so the default wiring gets host-compiled and smoke-tested); new `[env:native_burst]` (extends the same sources, adds `-D TX_POLICY_BURST -D TX_BURST_QUIET_MS=200`, `test_filter = test_tx_policy`) for the burst-mode Dispatcher tests.
- `test/test_tx_policy/test_tx_policy.cpp` (new): budget-policy tests (initial budget, refill rate, cap at window max, MTU retry_at, debit by measured airtime, reserve delay, refill wrap, `canChain` false), burst-policy tests (chain until B then wait X, gap ≥ X resets / gap < X does not, fitting packet sends without waiting, packet above B rejected, remaining falls to 0, timeout ends burst + quiet period, millis wrap-around), Dispatcher tests (default: single send then back to Rx, timeout without debit; burst: 3-packet chain with no Rx between sends, chain stops at burst max then quiet period, oversized packet dropped and returned to pool, TX timeout ends burst).

Non-obvious decisions / deviations from the plan:

1. Interface: timestamps changed from the draft's `unsigned long` to `uint32_t` (also `retry_at`/`remainingTxMs`). Reason: `unsigned long` is 64-bit on the native test host but 32-bit on every firmware target; with `uint32_t` the wrap-around tests exercise the same arithmetic as the firmware. `onTxDone` takes an extra `unsigned long& next_tx_time` out-parameter because the budget policy owns the `next_tx_time` calculation (moved out of `Dispatcher` as planned) but the field stays in `Dispatcher`. Added `onTxAborted(now)` to the interface (default no-op) so a TX timeout can end the burst and start the quiet period without touching the budget policy's behavior.
2. `AirtimeBudgetTxPolicy` reads factor/window at runtime through a small `TxPolicyEnv` interface (the two `Dispatcher` virtuals), which `Dispatcher` inherits privately so its public surface is unchanged.
3. `checkSend()` order is preserved: update-budget and MTU check happen before the `next_tx_time` gate, as today. Because budget refill is time-proportional and capped, deferring `updateBudget()` until the gate passes yields the same result as calling it every iteration.
4. `PacketManager::peekNextOutbound()` was added as a pure virtual (repo has exactly one implementer); if out-of-repo `PacketManager` implementations exist they would need the new method.
5. The burst drop check runs only in `checkSend()` (first packet of a burst); chained sends can never include an oversized packet because `canChain(a)` is false when `a > B`.
6. New `[env:native_burst]` test env is a plan addition (plan assumed one native env); needed because the burst Dispatcher path is compile-time selected and the default env must keep testing the default wiring.

Proof gaps (all `unverified` — TSAO asked not to build or run tests):

- `pio test -e native` and `pio test -e native_burst` not run.
- No firmware target compiled: the default (non-burst) firmware build path is compile-checked only by reading, not by a build.
- Hardware checks from the plan (SDR / TX pin: burst ≤ 1000 ms, silence ≥ X, no RX during burst) not performed.
- Dispatcher `#error` guard for missing `TX_BURST_QUIET_MS` not exercised.

### 2026-09-29 21:25 +08 — Claude — claude-opus-5-5 — medium

Review fixes (from review-cn470-burst, verified by hand arithmetic):

- `TxAirtimeBudget.RefillHandlesMillisWrap`: `now` was `0x90` (elapsed 160, refill 80 < MTU/2, so both assertions would fail). Changed to `0x150` (elapsed 352, refill 176), which matches the comment.
- `TxBurst.HandlesMillisWrapAround`: expected `retry_at` was `0xFFFFFFC4`; `0xFFFFFF00 + 200` is `0xFFFFFFC8`.
- Test helper `queuePacket()` now zeroes the payload bytes that `startOutboundSend()` copies.
- Log tags in `startOutboundSend()` now name that function.

Known behavior, not changed: when `canStart()` fails, `next_tx_time = retry_at`. If a smaller, higher-priority packet that fits is queued before `retry_at`, it still waits until `retry_at`, and then a new burst starts. This only delays TX, so the burst limit still holds. Decision 1 ("a later packet that fits is sent without waiting") holds when the burst ended because the queue was empty, since no `retry_at` is set then. The same staleness exists in the budget path today.

Not changed (review nits): `PacketQueue::bestIndex()` is public; `TxPolicy` has no virtual destructor (never deleted through a base pointer); `TX_BURST_MAX_MS > 8000` would set the non-RX watchdog error flag.

### 2026-09-29 21:45 +08 — Claude — claude-opus-5-5 — medium

Renamed `PacketManager::peekNextOutbound()` → `findNextOutbound()` and `PacketQueue::peek()` → `find()` (TSAO: `peek` read as "pick from the queue"). "find" does not change the queue; "get" removes. Behavior unchanged. Earlier entries keep the old name.

### 2026-09-29 21:50 +08 — Claude — unknown — not specified

Implemented Revision 1 (runtime policy selection) on top of the compile-time version. TSAO asked not to build; all results `unverified`.

Changes:

- `src/TxPolicy.h` / `src/TxPolicy.cpp`: removed `TX_POLICY_BURST` / `TX_BURST_MAX_MS` / `TX_BURST_QUIET_MS` and the `#error`. Added `TX_POLICY_MODE_BUDGET` / `TX_POLICY_MODE_BURST` (0/1). `TxPolicyEnv` gained `getBurstMaxTxMs()` / `getBurstQuietMs()` (pure virtual, like `getAirtimeBudgetFactor()`). `BurstTxPolicy` now takes a `TxPolicyEnv&` and reads B and X at runtime through private helpers `maxMs()` / `quietMs()`; the constructor no longer takes constants.
- `src/Dispatcher.h` / `src/Dispatcher.cpp`: both policy objects are always held; ctor inits `tx_policy = &budget_policy`. New protected virtuals `getTxPolicyMode()` (default budget), `getBurstMaxTxMs()` (default 1000), `getBurstQuietMs()` (default 0); the latter two also override the new `TxPolicyEnv` pure virtuals, so `BurstTxPolicy` picks up subclass overrides. New private `updateTxPolicy()` runs at the top of `checkSend()`: when `getTxPolicyMode()` no longer matches the active policy and `outbound == NULL`, it switches `tx_policy` and calls `begin(now)`. The oversized-packet drop now uses the runtime `getBurstMaxTxMs()` and is gated on `tx_policy == &burst_policy` (burst mode only). Default budget behaviour is unchanged: with the default virtuals, `updateTxPolicy()` never fires and the drop never triggers.
- `src/helpers/CommonRadioPrefs.h` / `.cpp`: getter/setter pairs `get/setTxPolicyMode`, `get/setBurstMaxTxMs`, `get/setBurstQuietMs` (pure virtual, implemented in both `RadioPrefs`). New CLI commands in `handleCommand` next to `af`/`dutycycle`: `get tx_policy` → `> budget` or `> burst <max_ms> <quiet_ms>`; `set tx_policy budget`; `set tx_policy burst <max_ms> <quiet_ms>` (both values required, max 1-8000, quiet >= 1; error replies in the `set af` style). Uses `mesh::Utils::parseTextParts` like `set radio`.
- `src/helpers/CommonCLI.h` (`NodePrefs`) and `examples/companion_radio/NodePrefs.h`: fields `tx_policy` (uint8, default 0), `burst_max_ms` (uint32, default 1000), `burst_quiet_ms` (uint32, default 0); `def("tx_policy"...`, `def("burst_max_ms"...`, `def("burst_quiet_ms"...` added to `RadioPrefs::structure()` right after `af`; getter/setter overrides. NOT added to the legacy binary loader in `CommonCLI::loadPrefsInt()`.
- Missing-key evidence (ConfigSerializer): in `ConfigSerializer::def(key, T& value)` (READ branch, ConfigSerializer.cpp), the value is only assigned inside `if (_context->keyMatch(_depth, key))`, and `loadSerial()` re-runs `structure()` only when a `TOK_VALUE` is parsed. A prefs file that contains none of the new keys therefore never assigns them, and the in-memory defaults (budget / 1000 / 0) survive the load. Covered by the `MissingTxPolicyKeysKeepDefaults` test.
- Examples: the three virtuals are overridden from `_prefs` next to `getAirtimeBudgetFactor()` in `examples/simple_repeater/MyMesh.h`, `examples/simple_room_server/MyMesh.h`, `examples/simple_sensor/SensorMesh.h/.cpp`, and `examples/companion_radio/MyMesh.h`. `simple_secure_chat` left on the default budget policy (its prefs are not `CommonRadioPrefs`).
- `docs/cli_commands.md`: new section "View or change the TX policy" between `af` and `int.thresh`, matching the existing format.
- `platformio.ini`: removed `[env:native_burst]` and its comment; one `[env:native]` now covers both policies. Added `../src/helpers/CommonRadioPrefs.cpp` and `../src/helpers/TxtDataHelpers.cpp` to the filter so the CLI can be host-tested.
- `test/mocks/target.h` (new): minimal `WRAPPER_CLASS radio_driver` stub for `CommonRadioPrefs.cpp` on the host. `test/mocks/Arduino.h`: added `constrain`, `ltoa` and `using std::abs` (needed by `CommonRadioPrefs.cpp` / `TxtDataHelpers.cpp` on the host).
- `test/test_tx_policy/test_tx_policy.cpp`: burst policy tests now construct `BurstTxPolicy(env)` with runtime B/X; new `ReadsLimitsFromEnvAtRuntime` (B and X changes take effect immediately); removed all `TX_POLICY_BURST` conditionals. Dispatcher burst tests set a runtime `tx_policy_mode` field on `TestDispatcher` (which overrides `getTxPolicyMode()`); new tests `OversizedPacketSentInBudgetMode`, `ModeChangeDuringSendAppliesAfterCompletion` (switch waits until no send is in flight; the completing send follows the old policy, and the burst chain only starts from the next `checkSend`), and `SwitchingBackToBudgetStartsWithFullBudget`. New `TxPolicyCli` tests: get default, `set ... budget`, `set ... burst <max> <quiet>` (values + get reply), missing quiet value, max above 8000, quiet 0, unknown policy.
- `test/test_companion_node_prefs/test_companion_node_prefs.cpp`: `MissingTxPolicyKeysKeepDefaults` (file without the new keys keeps budget/1000/0) and `TxPolicyKeysRoundTrip` (save/load of all three keys through the companion `NodePrefs`).

Non-obvious decisions / deviations from Revision 1:

1. The runtime mode-switch check lives at the top of `checkSend()` (not in `loop()`): `checkSend()` is only reached when no send is in flight anyway, so the `outbound == NULL` guard is kept for explicitness and the switch happens exactly where the policy is first used. A mode change made while a burst send is in flight therefore applies from the next `checkSend()`; the completion/chaining path of the in-flight send still follows the old policy. This matches "no switch while a send is in flight".
2. The oversized drop is gated on the active policy pointer (`tx_policy == &burst_policy`) rather than on `getTxPolicyMode()`: at `checkSend()` time `updateTxPolicy()` has already applied any pending switch (there is no send in flight), so the two are equivalent and the pointer reflects what will actually run.
3. New `TxPolicyEnv` members are pure virtuals (consistent with the two existing ones); consequence: any out-of-repo `TxPolicyEnv` implementation would need the two new methods, and the test mock was updated.
4. CLI tests required hosting `CommonRadioPrefs.cpp` (needs `target.h` -> new `test/mocks/target.h` with a `radio_driver` stub) and `TxtDataHelpers.cpp` (needs `ltoa`/`abs` on the host -> added to `test/mocks/Arduino.h`). Three small mock additions; judged within "few new mocks".
5. Docs version note says "Added in firmware v1.18.0" — an assumption: the tree still says `v1.17.1`; no changelog states the next version.
6. Prefs round-trip/missing-key tests cover the companion `NodePrefs` copy (already host-reachable, no new mocks needed); the `CommonCLI.h` copy uses the same `ConfigSerializer` mechanism but is not itself host-tested (its header pulls in the full CLI stack).

Proof gaps (all `unverified` — TSAO asked not to build or run tests):

- `pio test -e native` not run; the new/changed tests and the newly host-compiled sources (`CommonRadioPrefs.cpp`, `TxtDataHelpers.cpp`, `test/mocks/target.h`, `constrain`/`ltoa`/`abs` in `test/mocks/Arduino.h`) are compile- and run-unverified.
- No firmware target compiled; firmware builds unverified.
- `CommonCLI.h` `NodePrefs` prefs round-trip itself not host-tested (mechanism verified statically + via the companion copy test).
- Hardware checks (SDR / TX pin: burst <= B, silence >= X, no RX during burst) not performed; legacy `/com_prefs` upgrade path not exercised.


### 2026-09-29 22:00 +08 — Claude — claude-opus-5-5 — medium

Review fixes for Revision 1 (from review-tx-policy-cli, confirmed by reading the code):

- Native link: `CommonRadioPrefs.cpp` is linked into every `[env:native]` test binary, but `radio_driver` was only defined in `test_tx_policy.cpp`. The other test binaries would fail to link. `test/mocks/target.h` now defines it as a C++17 `inline` variable; the definition in the test file is removed.
- `set tx_policy`: the argument copy into `tmp[64]` used `strcpy` from a command buffer of up to 160 chars. It now uses `StrHelper::strncpy`.
- `set tx_policy burst`: `atol()` turned `-1` or an overflowing number into a huge `uint32_t` quiet time (TX stops for weeks without an error). Both values now go through `parseMillis()`: plain decimal digits only, range checked while parsing. `quiet_ms` is now limited to 1–3600000 (1 h, the same as the budget window; my choice, there is no source for a limit). Tests and `docs/cli_commands.md` updated.

Not changed (review nits): the JSON prefs keys are not validated on load (the same as every other JSON pref); `next_tx_time` is not cleared on a policy switch (only delays the first TX); C-style casts in `updateTxPolicy()`; the watchdog comment at exactly 8000 ms.

## Final Result

Implemented as planned, now under Revision 1: the TX policy is selected at runtime by the CLI config `tx_policy` (0 = budget, default, 1 = burst) with `burst_max_ms` / `burst_quiet_ms` prefs; `Dispatcher` holds both policy objects and switches (with `begin(now)`) when the mode changes and no send is in flight; `BurstTxPolicy` reads its limits at runtime through `TxPolicyEnv`; the oversized-packet drop uses the runtime limit in burst mode only; both `NodePrefs` copies persist the three keys with defaults budget / 1000 / 0 and older prefs files keep those defaults; CLI `get/set tx_policy` documented in `docs/cli_commands.md`; no compile-time defines remain. Default behaviour (budget, no config) matches the code at 37a71d93. Googletest coverage for both policies, runtime mode switching, the CLI parsing and the prefs round-trip/missing-key cases. Tests and builds are unverified per the no-build constraint; hardware validation pending.

## Review

### 2026-09-29 21:25 +08 — review-cn470-burst (subagent) — glm-5.3-flash — high

Findings: 2 blocking (wrong constants in the two millis-wrap tests), 1 should-fix (stale `next_tx_time` after a failed `canStart()`), 6 nits. Blocking findings and 2 nits fixed in the Work Log entry above; the should-fix is documented as known behavior.

Reviewed: full diff 37a71d93..HEAD, plan/work doc, old `Dispatcher` at 37a71d93, `TxPolicy.*`, `StaticPoolPacketManager.*`, `RadioLibWrappers.cpp` and all `mesh::Radio` subclasses, tests, `platformio.ini`, mocks.
Checks: line-by-line old/new comparison of `Dispatcher` paths; hand execution of all 20 tests (18 pass on paper before fixes; the 2 failing ones fixed); radio state trace through chaining; include resolution for the native envs; `#error` guard reachability. Nothing compiled or run.
Gaps: `pio test -e native`, `pio test -e native_burst`, firmware target builds, hardware burst timing, ESPNOW radio under burst mode, out-of-repo `PacketManager` implementations. Review fixes were not re-reviewed by a subagent.
Result: UNVERIFIED

### 2026-09-29 22:00 +08 — review-tx-policy-cli (subagent) — glm-5.3-flash — high

Findings: 1 blocking (native link of `radio_driver`), 2 should-fix (`strcpy` overflow, `atol` wrap of negative `quiet_ms`), 4 nits. Blocking and should-fix items fixed in the Work Log entry above; nits recorded there.

Reviewed: `git diff da38efe5..HEAD` with the full feature diff where needed, Revision 1, `ConfigSerializer`, both `NodePrefs`, legacy loaders, CLI dispatch, examples, mocks, `platformio.ini`, `docs/cli_commands.md`.
Checks: default budget path compared line by line with 37a71d93; switching and prefs missing-key behavior traced; CLI parsing traced. Nothing compiled or run.
Gaps: `pio test -e native`, firmware builds, hardware timing. The fixes were not reviewed again by a subagent.
Result: UNVERIFIED
