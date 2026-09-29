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

## Final Result

Implemented as planned: compile-time selectable TX policy (`TX_POLICY_BURST`), default airtime-budget behavior moved unchanged into `AirtimeBudgetTxPolicy`, new `BurstTxPolicy` with chaining that never switches the radio to RX mid-burst, quiet period `TX_BURST_QUIET_MS` between bursts, oversized-packet drop, `PacketManager::peekNextOutbound()`, and googletest coverage for both policies and both Dispatcher wirings. Tests and builds are unverified per the no-build constraint; hardware validation pending.

## Review
