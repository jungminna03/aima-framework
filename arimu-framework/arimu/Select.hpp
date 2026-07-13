#pragma once

// arimu/Select.hpp — generic "selection-under-context" machinery.
//
// A game-agnostic distillation of a pattern that recurs across gameplay systems:
//   OPTIONS (a table of candidates) + an EVALUATOR that reads FACTS from a
//   CONTEXT + a PICKER that folds per-option verdicts into one answer.
// The only thing that varies between concrete systems (a skill bid, a dialogue
// gate, a quest turn-in, an unlock check) is the SCOPE of the context a fact is
// read from — an entity's local components vs a world-global store. By hiding
// that scope behind one keyed accessor, all of them become the same two calls
// with a different Context instance.
//
// This is NOT tied to the Arimu ECS (it never touches App/World/System). It is a
// standalone, header-only, allocation-free, compile-time utility: every function
// is an inline template instantiated per-TU, exporting no symbols across a module
// boundary. It depends on NOTHING but <concepts>. The user supplies the Key type,
// a Context type that resolves a Key to a float, and (for scoring) an Options
// table + a scorer callable.
//
// Requires C++20 (concepts), which the framework already mandates.
//
// Example:
//   enum class Fact { None, Health, KillsOf, Gold };
//   struct Ctx { /* ... */ float value(Fact f, const char* arg = nullptr) const; };
//   // a boolean gate:
//   Arimu::Cond<Fact> conds[] = {{Fact::Gold, nullptr, 50}, {Fact::None}};
//   if (Arimu::gate_met_until(conds, ctx, Fact::None)) buy();
//   // a scored pick:
//   const Opt* best = Arimu::pick(opts, n, ctx,
//       [](const Opt& o, const Ctx& c){ return score(o, c); }, /*min*/0.0f);

#include <concepts>

namespace Arimu {

// A Context is any type that can resolve a Key (+ an optional argument string,
// for parameterized keys like "kill.<id>") to a float fact value.
template <class C, class Key>
concept SelectionContext = requires(const C& c, Key k, const char* arg) {
    { c.value(k, arg) } -> std::convertible_to<float>;
};

// One declarative condition row: fact `key` (+ optional `arg`) must satisfy
// min <= value <= max. `max` defaults to kCondNoMax (effectively unbounded), so
// existing 3-field aggregate initializers keep their >=min semantics unchanged.
// An upper bound expresses "at most N" gates declaratively (e.g. a dialogue
// trigger that only fires while its completion count is still 0: {min 0, max 0}).
inline constexpr float kCondNoMax = 3.4e38f;

template <class Key>
struct Cond {
    Key         key;
    const char* arg = nullptr;
    float       min = 0.0f;
    float       max = kCondNoMax;   // inclusive upper bound; kCondNoMax = none
};

// Test one condition against a context.
template <class Key, class C>
    requires SelectionContext<C, Key>
inline bool cond_met(const Cond<Key>& c, const C& ctx) {
    const float v = ctx.value(c.key, c.arg);
    return v >= c.min && v <= c.max;
}

// All-pass gate over a counted range of conditions (every cond must hold).
template <class Key, class C>
    requires SelectionContext<C, Key>
inline bool gate_met(const Cond<Key>* conds, int n, const C& ctx) {
    for (int i = 0; i < n; ++i)
        if (!cond_met(conds[i], ctx)) return false;
    return true;
}

// All-pass gate over a sentinel-terminated array. The sentinel Key is supplied
// by the caller so this never hardcodes a game's terminator idiom (e.g. the game
// passes Key::None). Requires Key to be equality-comparable.
template <class Key, class C>
    requires SelectionContext<C, Key> && std::equality_comparable<Key>
inline bool gate_met_until(const Cond<Key>* conds, const C& ctx, Key terminator) {
    for (int i = 0; conds[i].key != terminator; ++i)
        if (!cond_met(conds[i], ctx)) return false;
    return true;
}

// Scored picker: argmax of score(option, ctx) over `n` options, strictly above
// `min_score`. Returns a pointer to the winning option or nullptr if none beats
// the floor. The scorer is INJECTED as a callable so this never sees game-specific
// scoring (utility curves, per-actor weights, capability masks — a non-capable
// option simply scores below the floor). Uses strict `>` for a deterministic
// first-wins tie-break; callers relying on reproducible results (e.g. headless
// regression hashes) depend on this.
template <class Opt, class C, class ScoreFn>
inline const Opt* pick(const Opt* opts, int n, const C& ctx, ScoreFn&& score,
                       float min_score, float* out_score = nullptr) {
    const Opt* best = nullptr;
    float best_score = min_score;
    for (int i = 0; i < n; ++i) {
        const float s = score(opts[i], ctx);
        if (s > best_score) { best_score = s; best = &opts[i]; }
    }
    if (out_score) *out_score = best_score;
    return best;
}

} // namespace Arimu
