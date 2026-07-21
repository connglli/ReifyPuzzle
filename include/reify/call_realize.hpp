#pragma once

// [v0.2.2] Call-realization transform for rylink.
//
// CallRealizeTransform is the whole-program Transform that turns rylink's
// chosen call-graph into real `call @callee(args)` sites. For each planned
// caller->callee edge it finds semantically-safe rewrite sites in the caller
// and substitutes them with a call whose solved arguments reproduce the
// original value. The site-finding is a small peephole engine over pluggable
// RewriteRules; v1 ships LiteralToCallRule, which rewrites scalar-literal
// `let` initializers. RewriteRule / RewriteSite are the internal peephole
// sub-tier of this one transform — future rules (unchanged-var-to-call,
// binop-fold-to-call, etc.) plug in without touching the transform.

#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "reify/func_desc.hpp"
#include "reify/transform.hpp"

namespace refractir::reify {

  // A candidate location inside a FunDecl where a rewrite could fire.
  // Opaque to the engine — only the rule that produced it knows how to
  // apply it. Kind discriminates so a future engine can sort/filter by
  // category, but at v1 there's only one kind.
  struct RewriteSite {
    enum class Kind { LetInitIntLit, LetInitFloatLit };
    Kind kind;
    // Index into FunDecl::lets. The rule that emitted this site is
    // responsible for re-validating the index before applying (cheap
    // because v1 only mutates the init value, not the lets vector).
    int letIdx = 0;
    // The literal's value and its SIR-surface type-string. Used by the
    // engine to filter callees whose retType / ret value match without
    // re-walking the AST.
    std::int64_t intVal = 0;
    double floatVal = 0.0;
    std::string sirType;
  };

  class RewriteRule {
  public:
    virtual ~RewriteRule() = default;
    virtual const char *name() const = 0;
    virtual std::vector<RewriteSite> findSites(const FunDecl &caller) = 0;
    // Decide whether the site can be rewritten by calling into
    // `callee` using `fixedRealizationIdx` as the bundled realization
    // (the engine has already locked which realization runs at the
    // call site; rules cannot pick a different one). Returns true when
    // the rule can produce an `apply()` for this combination.
    virtual bool matchCallee(
        const RewriteSite &site, const FuncDescriptor &callee, std::size_t fixedRealizationIdx
    ) = 0;
    // Splice the call in. Returns true on success; false if some
    // late-stage check fails (e.g. a param type the rule can't handle).
    // `rng` is passed through so the rule can make randomised
    // sub-decisions (e.g. per-arg choice of literal vs. var+bias) and
    // still play nicely with the engine's shuffled-candidates order.
    virtual bool apply(
        FunDecl &caller, const FuncDescriptor &callerDesc, const RewriteSite &site,
        const FuncDescriptor &callee, std::size_t realizationIdx, std::mt19937 &rng
    ) = 0;
  };

  // v1 rule: literal `let` initializers (scalar Int/Float).
  std::unique_ptr<RewriteRule> makeLiteralToCallRule();

  struct RewriteResult {
    int sitesFound = 0;
    int sitesRewritten = 0;
  };

  // One planned call-graph edge: realize a call from `caller` into `callee`,
  // pinned to `calleeRealizationIdx`. Functions are named (canonical "@...")
  // rather than pointed-to so the plan is pure data — the transform resolves
  // the FunDecls out of the Program and the FuncDescriptors out of the
  // TransformContext at apply() time.
  struct CallRealizeEdge {
    std::string caller;
    std::string callee;
    std::size_t calleeRealizationIdx = 0;
  };

  // rylink's composition decision: which caller->callee edges to realize,
  // in application order. This is the tool's plan (which functions, which
  // realization); the per-function metadata it needs (descriptors, incl. the
  // concretized path) rides in the shared TransformContext.
  struct CallRealizePlan {
    std::vector<CallRealizeEdge> edges;
  };

  // Whole-program call-realization transform. Owns the peephole RewriteRules
  // and walks `plan.edges` in order, realizing each. Build one with
  // makeCallRealizeTransform (pre-loaded with the v1 literal rule) and run it
  // through a TransformPipeline like any other Transform.
  class CallRealizeTransform : public Transform {
  public:
    explicit CallRealizeTransform(CallRealizePlan plan) : plan_(std::move(plan)) {}

    void addRule(std::unique_ptr<RewriteRule> r) { rules_.push_back(std::move(r)); }

    std::string_view name() const override { return "CallRealizeTransform"; }

    // Resolve each planned edge's caller/callee (FunDecl from `prog`,
    // FuncDescriptor from `ctx.descriptors`) and realize it, drawing all
    // randomness from `ctx.rng`. Edges whose endpoints are missing are
    // skipped defensively. `TransformReport::sites` accumulates the spliced
    // call sites.
    TransformReport apply(Program &prog, TransformContext &ctx) override;

  private:
    // Realize sites in `caller` that call into `callee`. Filters sites to
    // those whose callee matches, then rolls a count-keyed acceptance coin
    // (pRewriteForMatches) per match under `rng`, splicing every
    // accepted+appliable site — possibly several distinct sites per edge —
    // up to the per-edge attempts cap. Each site is consumed once (see the
    // composition-safety note below), so a given let-init is never spliced
    // twice. Returns counters for telemetry.
    //
    // `fixedRealizationIdx` pins which realization of the callee the
    // rule must use — it has to be the one whose .sir was actually
    // merged into the bundle, otherwise the call would return a
    // different solved value than the rewrite expression assumes and
    // --validate would fail.
    //
    // Composition safety: each rule is individually UB-free (the call
    // expression evaluates to the original literal under BV arithmetic),
    // but *stacking* rewrites on the same site is not — consecutive
    // sub-expressions evaluate left-to-right in RefractIR, so e.g. composing
    // `c → f1() + (c - r1)` with a later rewrite of the literal `(c - r1)`
    // into `f2() + ((c - r1) - r2)` produces `f1() + f2() + …` and that
    // left-prefix sum can wrap in unintended ways. The transform therefore
    // marks each (caller, site) it successfully rewrites as consumed and
    // skips it on subsequent edges; one splice per site for the lifetime
    // of the transform.
    // `callerDesc` supplies the metadata of the caller function (including
    // the concretized execution path to target unexecuted blocks safely).
    RewriteResult rewriteEdge(
        FunDecl &caller, const FuncDescriptor &callerDesc, const FunDecl &calleeFn,
        const FuncDescriptor &callee, std::size_t fixedRealizationIdx, std::mt19937 &rng
    );

    CallRealizePlan plan_;
    std::vector<std::unique_ptr<RewriteRule>> rules_;
    // Identity = (caller FunDecl pointer, site letIdx). Caller pointers
    // are stable across one rylink program (the bundle's funs vector is
    // reserved upfront — see rylink.cpp generateOne). Stored as a
    // std::set of the bare pair so two different (caller, letIdx)
    // combinations cannot collide on a hash — the previous version
    // packed both into a uint64 via shift+xor, which loses the top
    // bits of the pointer and could (very rarely) alias two distinct
    // sites in the same rylink run.
    std::set<std::pair<const FunDecl *, int>> consumed_;
  };

  // Build a CallRealizeTransform pre-loaded with the v1 rule set
  // (LiteralToCallRule). This is the entry point rylink adds to its pipeline.
  std::unique_ptr<Transform> makeCallRealizeTransform(CallRealizePlan plan);

} // namespace refractir::reify
