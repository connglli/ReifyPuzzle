/**
 * rysmith — C++ random RefractIR leaf-function generator (v2).
 *
 * Builds RefractIR Programs directly in memory (no text generation/parsing),
 * then calls SymbolicExecutor in-process (no subprocess) to concretize them.
 *
 * Pipeline per leaf function:
 *   S1. Random CFG with n interior blocks
 *   S2. Sample a random execution path (EP)
 *   S3. Build symbolic Program AST directly (func_gen)
 *   S4. Validate (SemChecker + TypeChecker) and solve in-process
 *   S5. Emit concrete .sir via SIRPrinter
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "analysis/pass_manager.hpp"
#include "ast/sir_printer.hpp"
#include "cxxopts.hpp"
#include "error.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "interp/interpreter.hpp"
#include "reify/cfg_gen.hpp"
#include "reify/checksum.hpp"
#include "reify/common.hpp"
#include "reify/func_desc.hpp"
#include "reify/func_gen.hpp"
#include "reify/hyperparameters.hpp"
#include "reify/id_gen.hpp"
#include "reify/path_sampler.hpp"
#include "reify/state_profile.hpp"
#include "reify/var_catalogue.hpp"
#include "solver/solver.hpp"
#if defined(USE_BITWUZLA)
#include "solver/bitwuzla_impl.hpp"
#elif defined(USE_ALIVESMT)
#include "solver/alive_impl.hpp"
#endif

namespace fs = std::filesystem;
using namespace refractir;
using namespace refractir::reify;

// Parse "[lo, hi]" domain string
static std::pair<int64_t, int64_t> parseDomain(const std::string &s) {
  if (s.size() < 5 || s.front() != '[' || s.back() != ']')
    throw std::invalid_argument("invalid domain (expected [lo, hi]): " + s);
  auto inner = s.substr(1, s.size() - 2);
  auto comma = inner.find(',');
  if (comma == std::string::npos)
    throw std::invalid_argument("invalid domain (missing comma): " + s);
  auto loStr = inner.substr(0, comma);
  auto hiStr = inner.substr(comma + 1);
  while (!loStr.empty() && std::isspace((unsigned char) loStr.back()))
    loStr.pop_back();
  while (!hiStr.empty() && std::isspace((unsigned char) hiStr.front()))
    hiStr = hiStr.substr(1);
  return {std::stoll(loStr), std::stoll(hiStr)};
}

// Format a solved model value as the canonical descriptor / SOLVED-
// header string. Floats go through refractir::formatDouble for bit-exact
// round-trip — see its comment in ast.hpp for the rationale (signed
// zero preservation, no int/float dispatch ambiguity, subnormal
// safety).
static std::string fmtModelVal(const SymbolicExecutor::Result::ModelVal &v) {
  if (std::holds_alternative<int64_t>(v))
    return std::to_string(std::get<int64_t>(v));
  return formatDouble(std::get<double>(v));
}

static auto makeSolverFactory() {
  return [](const SymbolicExecutor::Config &cfg) -> std::unique_ptr<smt::ISolver> {
#if defined(USE_BITWUZLA)
    return std::make_unique<solver::BitwuzlaSolver>(cfg.timeout_ms, cfg.seed, cfg.num_smt_threads);
#elif defined(USE_ALIVESMT)
    return std::make_unique<solver::AliveSolver>(cfg.timeout_ms, cfg.seed, cfg.num_smt_threads);
#else
    (void) cfg;
    throw std::runtime_error("No solver backend compiled in");
#endif
  };
}

static bool validateWithSymiri(
    const fs::path &sirPath, const std::string &funcName, const std::vector<std::string> &paramArgs,
    bool verbose, SolvingMode solMode = SolvingMode::UBFree
) {
  std::ifstream ifs(sirPath);
  if (!ifs)
    return false;
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string src = ss.str();
  try {
    Lexer lx(src);
    auto toks = lx.lexAll();
    Parser ps(std::move(toks));
    Program prog = ps.parseProgram();

    if (!runAnalysisPasses(prog, /*verbose=*/false))
      return false;

    std::string canonical = funcName.empty() || funcName[0] == '@' ? funcName : "@" + funcName;

    // Send the interpreter's `Result:` line to a local sink unless verbose,
    // instead of redirecting the process-global std::cout (unsafe with
    // concurrent worker threads).
    std::stringstream sink;
    std::ostream &out = verbose ? std::cout : sink;

    try {
      Interpreter interp(prog, out);
      interp.run(canonical, {}, paramArgs);
      // Success (no exception) is correct only if we didn't require UB
      return (solMode != SolvingMode::RequireUB);
    } catch (const UndefinedBehaviorError &e) {
      if (verbose) {
        std::cout << "  (intercepted expected UB: " << e.what() << ")\n";
      }
      // UB exception is correct only if we required UB
      return (solMode == SolvingMode::RequireUB);
    } catch (...) {
      return false;
    }
  } catch (...) {
    return false;
  }
}

// [v0.2.2] One per concretized .sir file. Bundles the on-disk path
// with the per-init solved values (parameter args, sym values,
// return value) so consumers (--validate, --emit-desc) don't need
// to re-parse the SOLVED header. `rz.paramValues` is in declaration
// order; rysmith --validate flattens it for `symiri ... -- arg0
// arg1`.
struct ConcreteFile {
  fs::path path;
  FuncDescriptor::Realization rz;
};

struct GenerateResult {
  std::vector<ConcreteFile> produced;
};

static GenerateResult generateLeaf(
    // CFG params
    int nBbls, double pBranch, double pBackedge, bool requireReducible,
    // Path params
    int maxLoopIter, int minLoopIter,
    // Var params (varCfg.typeConfig contains the type generation configuration)
    const VarGenConfig &varCfg,
    // Func params
    const std::string &funcName, int nStmts, double offPathMultiplier, bool enableInterestCoefs,
    double pLargeCoef, int64_t largeCoefThreshold, int64_t coefLo, int64_t coefHi, int64_t valueLo,
    int64_t valueHi, int64_t indexLo, int64_t indexHi, const ExprGenConfig &exprCfg,
    bool enableIntrinsics,
    // Solver params
    uint32_t timeoutMs, SolvingMode solMode,
    // Retry params
    int maxRetries, int nInits,
    // IO
    const fs::path &outDir, bool keepSymbolic, bool verbose,
    // RNG (by value — safe to run in a detached thread)
    std::mt19937 rng, uint32_t baseSeed,
    // [v0.2.2] 6-char generation ID — used for the per-fn descriptor.
    const std::string &genId,
    // [v0.2.2] When true, write the rylink-consumable func_<id>_<i>.json
    // sidecar next to each successful concrete .sir.
    bool emitDesc, bool emitMain, bool noCrc32
) {
  // S1: CFG
  GenCFGParams cfgParams;
  cfgParams.nBbls = nBbls;
  cfgParams.seed = rng();
  cfgParams.pBranch = pBranch;
  cfgParams.pBackedge = pBackedge;
  cfgParams.requireReducible = requireReducible;
  auto cfg = genCFG(cfgParams);

  if (verbose)
    std::cout << "[cfg] " << cfg.blocks.size() << " blocks\n";

  // Generate VarCatalogue (shared across all inits for the same CFG)
  VarCatalogue vars = genVarCatalogue(rng, varCfg);

  if (verbose)
    std::cout << "[vars] " << vars.vars.size() << " vars, " << vars.structDecls.size()
              << " structs\n";

  for (int attempt = 0; attempt <= maxRetries; attempt++) {
    // Path sampling (reduce loop iterations on retry)
    SamplePathParams pathParams;
    pathParams.seed = rng();
    // Keep max ≥ min so retry decay can't violate the requested minimum.
    pathParams.maxLoopIter = std::max(minLoopIter, maxLoopIter - attempt);
    pathParams.minLoopIter = minLoopIter;

    auto maybePath = samplePath(cfg, pathParams);
    if (!maybePath) {
      if (verbose)
        std::cerr << "[sampler] attempt=" << attempt
                  << " sample failed (minLoopIter=" << minLoopIter
                  << ", maxLoopIter=" << pathParams.maxLoopIter << ")\n";
      continue;
    }
    const auto &path = *maybePath;

    if (verbose)
      std::cout << "[sampler] attempt=" << attempt << " EP len=" << path.size() << "\n";

    // CFG/PATH comment header shared by every init of this attempt
    // the path itself is fixed for the whole attempt, only the per-init
    // sym/param solutions differ.
    auto writePathHeader = [&](std::ostream &os) {
      os << "// CFG:\n";
      for (const auto &b: cfg.blocks) {
        os << "//   " << b.label;
        if (!b.succs.empty()) {
          os << " ->";
          for (const auto &succ: b.succs)
            os << " " << succ;
        }
        os << "\n";
      }
      os << "// PATH:";
      for (std::size_t k = 0; k < path.size(); k++)
        os << (k == 0 ? " " : " -> ") << path[k];
      os << "\n\n";
    };

    // Generate nInits independently-seeded programs
    std::vector<ConcreteFile> produced;
    for (int initIdx = 0; initIdx < nInits; initIdx++) {
      FuncGenConfig fcfg;
      fcfg.funcName = funcName;
      fcfg.seed = rng();
      fcfg.nStmts = nStmts;
      fcfg.offPathMultiplier = offPathMultiplier;
      fcfg.enableInterestCoefs = enableInterestCoefs;
      fcfg.enableInterestInits = true;
      fcfg.enableIntrinsics = enableIntrinsics;
      fcfg.pLargeCoef = pLargeCoef;
      fcfg.largeCoefThreshold = largeCoefThreshold;
      fcfg.exprCfg = exprCfg;
      fcfg.coefLo = coefLo;
      fcfg.coefHi = coefHi;
      fcfg.valueLo = valueLo;
      fcfg.valueHi = valueHi;
      fcfg.indexLo = indexLo;
      fcfg.indexHi = indexHi;

      auto [prog, pathLabels] = genFunction(cfg, path, vars, fcfg);

      // Optionally dump symbolic program
      if (keepSymbolic) {
        auto symPath =
            outDir / (funcName + reify::rysmith::hp::kSymInfix + std::to_string(initIdx) + ".sir");
        std::ofstream ofs(symPath);
        writePathHeader(ofs);
        SIRPrinter printer(ofs);
        printer.print(prog);
        if (verbose)
          std::cout << "  symbolic: " << symPath << "\n";
      }

      // Validate AST
      DiagBag diags;
      PassManager pm(diags);
      pm.addModulePass(std::make_unique<SemChecker>());
      pm.addModulePass(std::make_unique<TypeChecker>());
      if (pm.run(prog) == PassResult::Error) {
        if (verbose) {
          std::cerr << "[validate] init " << initIdx << ": generated program failed validation\n";
          for (const auto &d: diags.diags)
            if (d.level == DiagLevel::Error)
              std::cerr << "  error: " << d.message << "\n";
        }
        continue;
      }

      // Solve
      SymbolicExecutor::Config solverCfg;
      solverCfg.timeout_ms = timeoutMs;
      solverCfg.seed = baseSeed + (uint32_t) (attempt * 100 + initIdx);
      solverCfg.num_threads = 1;
      solverCfg.num_smt_threads = 1;
      solverCfg.mode = solMode;

      SymbolicExecutor executor(prog, solverCfg, makeSolverFactory());
      SymbolicExecutor::Result res;
      try {
        res = executor.solve("@" + funcName, pathLabels);
      } catch (const std::exception &e) {
        if (verbose)
          std::cerr << "[solver] init " << initIdx << ": exception: " << e.what() << "\n";
        res.unknown = true;
        continue;
      } catch (...) {
        if (verbose)
          std::cerr << "[solver] init " << initIdx << ": unknown exception\n";
        res.unknown = true;
        continue;
      }

      if (res.sat) {
        // Apply the checksum rewrite while we still hold the in-memory
        // prog. The solver only saw the sum-based `%_chk = %_chk + ...`
        // contract; rewriting now means every downstream consumer
        // (SIRPrinter, symiri, symirc, the C / WASM backends) sees the
        // opaque CRC32 form instead. The model from the solver
        // (sym → value bindings) carries over unchanged because no
        // symbols are introduced or removed by the rewrite. The
        // pre-rewrite `res.retModel` (the solver's sum) is now stale
        // and is dropped from the SOLVED header so the post-rewrite
        // symiri value can take its place below.
        size_t crcUpdates = 0;
        if (!noCrc32) {
          crcUpdates = rewriteExitToCrc32Checksum(prog, funcName, res.letExitValues);
        }
        bool rewriteApplied = crcUpdates > 0;
        if (rewriteApplied)
          res.retModel.reset();

        // [v0.2.2] Init suffix is a lowercase letter a..z so descriptor
        // consumers (rylink) can address a specific concretization by
        // `<funcName><letter>`. nInits is clamped to [1, 26] at CLI
        // parse time, so initIdx is always in range.
        char letter = static_cast<char>('a' + initIdx);
        std::string outName = nInits > 1 ? funcName + letter + ".sir" : funcName + ".sir";
        auto concretePath = outDir / outName;

        // Locate the entry function in the rewritten prog. Snapshot
        // every piece of metadata we'll need (params, syms) into
        // owning vectors here, BEFORE any subsequent `prog.funs`
        // mutation — e.g. the optional `@main` push_back below
        // invalidates pointers/references into the funs vector when
        // it reallocates, and reading `entry->params` afterward
        // returned junk for `cf.rz.paramValues`, which fed the
        // wrong CLI args to the validate-time symiri.
        const FunDecl *entry = nullptr;
        for (const auto &f: prog.funs) {
          if (f.name.name == "@" + funcName) {
            entry = &f;
            break;
          }
        }
        std::vector<std::string> paramVals;
        std::vector<std::pair<std::string, std::string>> paramValuesCaptured;
        std::vector<std::pair<std::string, std::string>> symValuesCaptured;
        if (entry) {
          paramVals.reserve(entry->params.size());
          for (const auto &p: entry->params) {
            auto it = res.paramModel.find(p.name.name);
            std::string val = it != res.paramModel.end() ? fmtModelVal(it->second) : "0";
            paramVals.push_back(val);
            if (it != res.paramModel.end())
              paramValuesCaptured.emplace_back(p.name.name, std::move(val));
          }
          for (const auto &s: entry->syms) {
            auto it = res.model.find(s.name.name);
            if (it != res.model.end())
              symValuesCaptured.emplace_back(s.name.name, fmtModelVal(it->second));
          }
        }

        // Capture the post-rewrite CRC32 return value via a MINIMAL
        // oracle program. The oracle:
        //   - shares struct decls and the @crc32_update intrinsic with
        //     the full program;
        //   - declares the same lets + params as the entry function,
        //     but each scalar / aggregate let-init is rewritten to its
        //     solver-known exit-time value (LetExitValue);
        //   - replays each pointer let's EXIT-time target via
        //     `%p = addr <targetLocal>;` (the solver's prov_base
        //     reverse-FNV'd back to a local name) so body-side
        //     pointer retargets — e.g. `%p0 = load %pp1;` — are
        //     captured;
        //   - then runs the verbatim exit block (load preamble +
        //     CRC32 chain + ret).
        // Driving the oracle from a SEPARATE program is intentional:
        // it makes `--validate` a real cross-check that the solver's
        // exit-time model + the minimal program == the interpreter's
        // execution of the full program. If we instead captured from
        // the full program itself, validate would compare X to X.
        //
        // Capture failure (symiri exits non-zero or returns no
        // parseable Result line) typically means the generated
        // function tripped UB that the solver missed. Treat the init
        // as failed: skip the .sir write and try the next init.
        std::string crcRetValue;
        if (rewriteApplied && entry) {
          Program miniProg = buildMiniCrc32Prog(prog, funcName, res.letExitValues);
          auto tempPath = outDir / (outName + ".oracle.tmp");
          {
            std::ofstream tofs(tempPath);
            if (tofs) {
              SIRPrinter printer(tofs);
              printer.print(miniProg);
            }
          }
          auto captured = runSymiriCaptureResult(tempPath, "minimal_" + funcName, paramVals);
          std::error_code ec;
          fs::remove(tempPath, ec); // best-effort; safe to leave on disk
          (void) ec;
          if (!captured) {
            if (verbose) {
              std::cerr << "[oracle] init " << initIdx
                        << ": symiri capture failed for the minimal "
                           "checksum oracle of "
                        << concretePath << "; skipping init\n";
            }
            continue;
          }
          crcRetValue = std::move(*captured);
        }

        std::string expectedRet =
            !crcRetValue.empty() ? crcRetValue
                                 : (res.retModel.has_value() ? fmtModelVal(*res.retModel) : "");

        // Now that we have the expected return value we can build a faithful
        // `@main` wrapper that asserts it via `@check_chksum`. This
        // push_back invalidates `entry`, but every read we needed
        // from it has already been snapshotted into paramVals /
        // paramValuesCaptured / symValuesCaptured above.
        if (emitMain && entry) {
          FunDecl mainFn = buildMainFunction(prog, *entry, paramVals, expectedRet);
          prog.funs.push_back(std::move(mainFn));
          entry = nullptr; // do not use after realloc
        }

        {
          std::ofstream ofs(concretePath);
          if (!ofs) {
            std::cerr << "error: cannot open " << concretePath << "\n";
            continue;
          }
          // [v0.2.2] SOLVED header (same format as symirsolve --output).
          // Records the synthesised param + ret values so symiri can
          // re-run via `--main @f <file> -- <p0> <p1>` deterministically.
          if (!res.paramModel.empty() || !expectedRet.empty()) {
            ofs << "// SOLVED:";
            bool first = true;
            for (const auto &[name, val]: res.paramModel) {
              ofs << (first ? " " : ", ") << name << "=" << fmtModelVal(val);
              first = false;
            }
            if (!expectedRet.empty()) {
              ofs << (first ? " " : ", ") << "ret=" << expectedRet;
            }
            ofs << "\n";
          }
          writePathHeader(ofs);
          SIRPrinter printer(ofs, res.model);
          printer.print(prog);
        }

        // [v0.2.2] Use the metadata we snapshotted above (entry may now
        // be dangling thanks to the @main push_back). Param values
        // are in declaration order — symiri positional args need that
        // ordering at validate time. Syms are in declaration order so
        // the descriptor's top-level `syms` list and the per-realization
        // `symValues` line up positionally.
        ConcreteFile cf;
        cf.path = concretePath;
        cf.rz.file = concretePath.filename().string();
        cf.rz.paramValues = std::move(paramValuesCaptured);
        cf.rz.symValues = std::move(symValuesCaptured);
        cf.rz.retValue = expectedRet;
        produced.push_back(std::move(cf));
        if (emitDesc) {
          std::vector<FuncDescriptor::Realization> realizations;
          realizations.reserve(produced.size());
          for (const auto &x: produced)
            realizations.push_back(x.rz);
          auto descPath = outDir / (funcName + ".json");
          writeFuncDescriptorFromProgram(descPath, funcName, prog, pathLabels, realizations, genId);
        }
        if (verbose)
          std::cout << "[emit] init " << initIdx << ": " << concretePath << "\n";
      } else if (verbose) {
        std::cerr << "[solver] init " << initIdx << ": " << (res.unsat ? "UNSAT" : "UNKNOWN")
                  << "\n";
      }
    }

    if (!produced.empty())
      return GenerateResult{std::move(produced)};

    if (verbose)
      std::cerr << "[solver] attempt=" << attempt << ": all inits failed, retrying\n";
  }

  return GenerateResult{};
}

int main(int argc, char **argv) {
  cxxopts::Options opts("rysmith", "rysmith — C++ random RefractIR leaf-function generator");

  // clang-format off
  opts.add_options()
    ("n,n-funcs",         "Number of leaf functions to generate",
                          cxxopts::value<int>()->default_value("1"))
    // Type control
    ("no-fp",             "Disable f32/f64 types entirely")
    ("no-vec",            "Disable <N> T vector type generation")
    ("no-agg-ptr",        "Disable ptr [N] T / ptr @S aggregate pointer generation")
    ("max-ptr-depth",     "Maximum pointer nesting depth (0 disables pointers)",
                          cxxopts::value<int>()->default_value("2"))
    ("max-agg-nest",      "Maximum aggregate nesting depth",
                          cxxopts::value<int>()->default_value("2"))
    ("max-agg-elems",     "Maximum array size and struct field count",
                          cxxopts::value<int>()->default_value("3"))
    // Generation
    ("n-params",          "Number of scalar parameters per generated function (default: 3)",
                          cxxopts::value<int>()->default_value("3"))
    ("n-vars",            "Variables per function",
                          cxxopts::value<int>()->default_value("10"))
    ("n-stmts",           "Statements per block on path",
                          cxxopts::value<int>()->default_value("3"))
    ("min-atoms",         "Minimum atoms per generated expression",
                          cxxopts::value<int>()->default_value("1"))
    ("max-atoms",         "Maximum atoms per generated expression",
                          cxxopts::value<int>()->default_value("3"))
    ("off-path-multiplier", "Scale --n-stmts / --min-atoms / --max-atoms by this factor in off-path blocks (never executed; solver-free)",
                          cxxopts::value<double>()->default_value("2.0"))
    // Operators
    ("no-divmod",         "Disable integer division and modulo")
    ("no-select",         "Disable select ternary expressions")
    ("no-intrinsics",     "Disable intrinsic call generation")
    ("no-ptrarith",       "Disable pointer arithmetics")
    ("no-crc32",          "Disable CRC32 replacement of the original addition-based checksum")
    // CFG
    ("n-bbls",            "Basic blocks between entry and exit per CFG",
                          cxxopts::value<int>()->default_value("15"))
    ("p-branch",          "Probability of two-successor block",
                          cxxopts::value<double>()->default_value("0.5"))
    ("p-backedge",        "Probability of back-edge",
                          cxxopts::value<double>()->default_value("0.3"))
    ("require-reducible", "Only generate reducible CFGs (irreducible back edges are repaired away)")
    // Solver
    ("timeout",           "SMT solver timeout per attempt in ms",
                          cxxopts::value<uint32_t>()->default_value("2000"))
    ("require-ub",        "Force at least one UB to be triggered on the chosen path")
    ("coef-domain",       "Domain for coef symbols",
                          cxxopts::value<std::string>()->default_value("[-2147483647, 2147483647]"))
    ("value-domain",      "Domain for value/constant symbols",
                          cxxopts::value<std::string>()->default_value("[-2147483647, 2147483647]"))
    ("index-domain",      "Domain for index symbols",
                          cxxopts::value<std::string>()->default_value("[1, 30]"))
    // Retry/inits
    ("n-inits",           "Concretizations per template (different seeds)",
                          cxxopts::value<int>()->default_value("3"))
    ("max-retries",       "Retry attempts on solver failure",
                          cxxopts::value<int>()->default_value("2"))
    ("max-loop-iter",     "Max loop iterations in the execution path (EP) sample",
                          cxxopts::value<int>()->default_value("3"))
    ("min-loop-iter",     "Require at least one loop in the EP to iterate this many times",
                          cxxopts::value<int>()->default_value("0"))
    ("p-large-coef",      "Fraction of new on-path coefs forced to |c| > --large-coef",
                          cxxopts::value<double>()->default_value("0.3"))
    ("large-coef",        "Magnitude threshold T for the |c| > T interest require (clamped per-coef to --coef-domain)",
                          cxxopts::value<int64_t>()->default_value("1048576"))
    // Output
    ("o,output-dir",      "Output directory",
                          cxxopts::value<std::string>()->default_value("rysmith_out"))
    ("target",            "Compile concrete .sir to target (sir, c, wasm); sir = no compilation",
                          cxxopts::value<std::string>()->default_value("sir"))
    ("vec-lowering",      "Vec-lowering strategy for C backend (random|vecext|scalars|array|structscalars|structarray)",
                          cxxopts::value<std::string>()->default_value("random"))
    ("keep-require",      "Include require checks in compiled output (default: omitted)")
    ("keep-symbolic",     "Write intermediate symbolic .sir files to disk")
    ("emit-desc",         "Emit per-function descriptor JSON (func_<id>_<i>.json) — needed by rylink")
    ("emit-state",        "Emit a func_<id>_<i>.state.json profile of the concrete state at each program point — consumed by rytwin. Value selects granularity: pbb (per basic block) or ppp (per program point)",
                          cxxopts::value<std::string>())
    ("emit-main",         "Generate a main wrapper in the output program")
    // Validation
    ("validate",          "Run symiri on each concrete .sir to validate")
    // Misc
    ("seed",              "Master RNG seed (default: random)",
                          cxxopts::value<uint32_t>())
    ("v,verbose",         "Verbose output")
    ("h,help",            "Print usage");
  // clang-format on

  cxxopts::ParseResult result;
  try {
    result = opts.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n" << opts.help() << "\n";
    return 1;
  }

  if (result.count("help")) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  // ---- Parse domains -------------------------------------------------------
  int64_t coefLo, coefHi, valueLo, valueHi, indexLo, indexHi;
  try {
    auto [clo, chi] = parseDomain(result["coef-domain"].as<std::string>());
    auto [vlo, vhi] = parseDomain(result["value-domain"].as<std::string>());
    auto [ilo, ihi] = parseDomain(result["index-domain"].as<std::string>());
    coefLo = clo;
    coefHi = chi;
    valueLo = vlo;
    valueHi = vhi;
    indexLo = ilo;
    indexHi = ihi;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  // ---- Setup ---------------------------------------------------------------
  uint32_t masterSeed =
      result.count("seed") ? result["seed"].as<uint32_t>() : (uint32_t) std::random_device{}();
  std::cout << "rysmith: master seed = " << masterSeed << "\n";
  std::mt19937 rng(masterSeed);

  // [v0.2.2] 6-char hex generation ID — namespaces function and struct
  // names so multiple rysmith outputs link without rename. The ID is
  // always derived from the master seed (via genHexId) so two runs with
  // the same --seed reproduce the same ID; there is no CLI override.
  std::string genId = genHexId(rng);
  std::cout << "rysmith: generation id = " << genId << "\n";

  fs::path outDir = result["output-dir"].as<std::string>();
  fs::create_directories(outDir);

  // Type config
  TypeGenConfig typeCfg;
  typeCfg.enableFp = !result.count("no-fp");
  typeCfg.enableVec = !result.count("no-vec");
  typeCfg.enableAggPtr = !result.count("no-agg-ptr");
  typeCfg.maxPtrDepth = result["max-ptr-depth"].as<int>();
  typeCfg.maxAggNesting = result["max-agg-nest"].as<int>();
  typeCfg.maxAggElems = result["max-agg-elems"].as<int>();

  int minAtoms = result["min-atoms"].as<int>();
  int maxAtoms = result["max-atoms"].as<int>();
  if (minAtoms < 1) {
    std::cerr << "error: --min-atoms must be >= 1 (got " << minAtoms << ")\n";
    return 2;
  }
  if (maxAtoms < minAtoms) {
    std::cerr << "error: --max-atoms must be >= --min-atoms (got " << maxAtoms << " vs " << minAtoms
              << ")\n";
    return 2;
  }
  double offPathMultiplier = result["off-path-multiplier"].as<double>();
  if (offPathMultiplier < 0.0) {
    std::cerr << "error: --off-path-multiplier must be >= 0 (got " << offPathMultiplier << ")\n";
    return 2;
  }

  // Var config
  VarGenConfig varCfg;
  varCfg.nVars = result["n-vars"].as<int>();
  varCfg.nParams = result["n-params"].as<int>();
  varCfg.genId = genId;
  varCfg.typeConfig = typeCfg;

  // Expr config
  ExprGenConfig exprCfg;
  exprCfg.enableAllOps = true; // always enable all ops by default
  exprCfg.enableDiv = !result.count("no-divmod");
  exprCfg.enableSelect = !result.count("no-select");
  exprCfg.enablePtrArith = !result.count("no-ptrarith");
  exprCfg.enableFp = typeCfg.enableFp;
  exprCfg.minAtoms = minAtoms;
  exprCfg.maxAtoms = maxAtoms;
  bool enableIntrinsics = !result.count("no-intrinsics");

  int nFuncs = result["n-funcs"].as<int>();
  int nBbls = result["n-bbls"].as<int>();
  int nStmts = result["n-stmts"].as<int>();
  int maxLoopIter = result["max-loop-iter"].as<int>();
  int minLoopIter = result["min-loop-iter"].as<int>();
  // [v0.2.2] Clamp to [1, 26] — each init's concrete file is named
  // with a lowercase-letter suffix `func_<id>_<i><a..z>.sir`, so
  // 26 is the natural cap. 0 is meaningless (no concretization).
  int nInits = result["n-inits"].as<int>();
  if (nInits < 1) {
    std::cerr << "warning: --n-inits clamped to 1 (was " << nInits << ")\n";
    nInits = 1;
  } else if (nInits > 26) {
    std::cerr << "warning: --n-inits clamped to 26 (was " << nInits << ")\n";
    nInits = 26;
  }
  int maxRetries = result["max-retries"].as<int>();
  double pBranch = result["p-branch"].as<double>();
  double pBackedge = result["p-backedge"].as<double>();
  bool requireReducible = result.count("require-reducible") > 0;
  bool enableInterestCoefs = true; // kept in code; not user-exposed
  double pLargeCoef = result["p-large-coef"].as<double>();
  if (pLargeCoef < 0.0 || pLargeCoef > 1.0) {
    std::cerr << "error: --p-large-coef must be in [0, 1] (got " << pLargeCoef << ")\n";
    return 2;
  }
  int64_t largeCoefThreshold = result["large-coef"].as<int64_t>();
  if (largeCoefThreshold < 0) {
    std::cerr << "error: --large-coef must be >= 0 (got " << largeCoefThreshold << ")\n";
    return 2;
  }
  uint32_t timeoutMs = result["timeout"].as<uint32_t>();
  SolvingMode solMode = result.count("require-ub") ? SolvingMode::RequireUB : SolvingMode::UBFree;
  // Wall-clock budget per function: covers all retries × inits plus 50 ms for non-solver overhead
  // (CFG gen, path sampling, formula construction, SIRPrinter). Compilation runs outside the
  // thread.
  uint32_t funcTimeoutMs = (uint32_t) ((uint64_t) (maxRetries + 1) * nInits * timeoutMs + 50);
  bool keepSymbolic = result.count("keep-symbolic") > 0;
  bool emitDesc = result.count("emit-desc") > 0;
  bool doValidate = result.count("validate") > 0;
  bool emitMain = result.count("emit-main") > 0;
  bool verbose = result.count("verbose") > 0;
  std::string emitStateMode =
      result.count("emit-state") ? result["emit-state"].as<std::string>() : "";
  if (!emitStateMode.empty() && emitStateMode != "pbb" && emitStateMode != "ppp") {
    std::cerr << "error: --emit-state must be 'pbb' or 'ppp' (got '" << emitStateMode << "')\n";
    return 2;
  }
  // --require-ub implies --no-crc32. The solver sees the sum-form
  // checksum (`%_chk = %_chk + <leaf>` per leaf, emitted by
  // buildSumChecksum). In RequireUB mode it is free to satisfy the
  // "at least one UB on the path" obligation by overflowing that
  // accumulator — a perfectly real signed-overflow UB in the program
  // it solved. But rewriteExitToCrc32Checksum then replaces every
  // `%_chk = %_chk + <leaf>` with a total `@crc32_update(...)` call,
  // deleting exactly that overflow. The emitted program would then be
  // UB-free even though the solver proved a UB, so symiri reports no
  // UB. Keeping the sum form (no crc32 rewrite) makes the program we
  // emit byte-identical to the one we solved, so a solver-found UB is
  // guaranteed to trap. This costs nothing: a UB-triggering program
  // aborts before it reaches a clean `ret`, so the crc32 return-value
  // oracle is vestigial for it anyway.
  bool noCrc32 = result.count("no-crc32") > 0 || solMode == SolvingMode::RequireUB;
  std::string target = result["target"].as<std::string>();
  bool noRequire = !result.count("keep-require");
  std::string vecLoweringOpt = result["vec-lowering"].as<std::string>();

  if (target != "sir" && target != "c" && target != "wasm") {
    std::cerr << "error: unknown target '" << target << "' (expected sir, c, wasm)\n";
    return 1;
  }

  // Sibling symiri is mandatory: the post-solve checksum rewrite uses
  // it to compute each concrete .sir's CRC32 return value, which lands
  // in the descriptor's `retValue` and gets asserted by rylink's
  // `@check_chksum(EXPECTED, …)` main wrapper. Falling back to the
  // solver's pre-rewrite model would put the SUM into the descriptor
  // while the on-disk program returns the CRC32, breaking every
  // compiled program downstream — so abort rather than silently emit
  // bad oracles. --validate's symiri usage rides on the same binary.
  // ---- Main loop -----------------------------------------------------------
  auto wallStart = std::chrono::steady_clock::now();
  int nOk = 0, nFail = 0;

  for (int i = 0; i < nFuncs; i++) {
    std::string funcName =
        std::string(reify::rysmith::hp::kFuncPrefix) + "_" + genId + "_" + std::to_string(i);
    uint32_t funcSeed = rng();
    std::cout << "[" << (i + 1) << "/" << nFuncs << "] generating " << funcName
              << " (seed=" << funcSeed << ")\n";

    // Heap-allocated state lets us safely detach the thread on timeout without
    // dangling references. Leaked on timeout — bounded by nFuncs, cleaned at exit.
    struct FuncState {
      std::mt19937 rng;
      GenerateResult result;
      std::atomic<bool> done{false};
    };

    auto *state = new FuncState{std::mt19937(funcSeed), {}, false};

    // [v0.2.2] Per-function copy so funcIdx makes it into struct names
    // (`@struct_<id>_<funcIdx>_<j>`). Without this every sibling fun in
    // the same rysmith run would emit `@struct_<id>_0` etc, breaking
    // rylink's bundle merge on a name vs. content mismatch.
    VarGenConfig fnVarCfg = varCfg;
    fnVarCfg.funcIdx = i;

    std::thread t([&, state]() {
      state->result = generateLeaf(
          nBbls, pBranch, pBackedge, requireReducible, maxLoopIter, minLoopIter, fnVarCfg, funcName,
          nStmts, offPathMultiplier, enableInterestCoefs, pLargeCoef, largeCoefThreshold, coefLo,
          coefHi, valueLo, valueHi, indexLo, indexHi, exprCfg, enableIntrinsics, timeoutMs, solMode,
          maxRetries, nInits, outDir, keepSymbolic, verbose, state->rng, funcSeed, genId, emitDesc,
          emitMain, noCrc32
      );
      state->done.store(true, std::memory_order_release);
    });

    bool timedOut = false;
    if (funcTimeoutMs > 0) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(funcTimeoutMs);
      while (!state->done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          timedOut = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    if (timedOut) {
      t.detach(); // state leaked; thread completes or dies with the process
      std::cerr << "[TIMEOUT] " << funcName << " exceeded " << funcTimeoutMs << "ms wall clock\n";
      nFail++;
      continue;
    }
    t.join();
    GenerateResult genRes = std::move(state->result);
    delete state;

    if (genRes.produced.empty()) {
      std::cerr << "[FAIL] all attempts failed for " << funcName << " (seed=" << funcSeed << ")\n";
      nFail++;
      continue;
    }

    for (const auto &cf: genRes.produced) {
      const fs::path &p = cf.path;
      std::cout << "  concrete: " << p << "\n";

      if (target != "sir") {
        std::string ext = (target == "c") ? ".c" : ".wat";
        fs::path outPath = p.parent_path() / (p.stem().string() + ext);
        std::string vecLowering = reify::pickVecLowering(rng, vecLoweringOpt);
        if (verbose && !vecLowering.empty())
          std::cout << "  vec-lowering: " << vecLowering << "\n";
        bool ok =
            compileSirInProcess(p, target, outPath, !noRequire, vecLowering, emitMain, verbose);
        if (ok)
          std::cout << "  compiled: " << outPath << "\n";
        else
          std::cerr << "  compile FAIL: " << p << "\n";
      }
    }

    if (doValidate || !emitStateMode.empty()) {
      const bool wantProfile = !emitStateMode.empty();
      const StateGranularity stGran =
          emitStateMode == "ppp" ? StateGranularity::Ppp : StateGranularity::Pbb;
      bool allOk = true;
      for (const auto &cf: genRes.produced) {
        const fs::path &p = cf.path;
        // [v0.2.2] Strip the trailing init-letter suffix (`a`..`z`)
        // from the stem to recover the base function name. Stems are
        // either `func_<id>_<i>` (single init) or `func_<id>_<i><a..z>`
        // (multi-init); drop one trailing lowercase letter when the
        // last char is a letter and the char before it is a digit
        // (so the trailing `<id>_<i>` digit run is preserved).
        std::string stem = p.stem().string();
        std::string baseFuncName = stem;
        if (stem.size() >= 2) {
          char last = stem.back();
          char prev = stem[stem.size() - 2];
          if (last >= 'a' && last <= 'z' && std::isdigit((unsigned char) prev))
            baseFuncName = stem.substr(0, stem.size() - 1);
        }
        // Param values came straight from the solver's model — no
        // need to re-parse the SOLVED header. Flatten the
        // declaration-order (name, value) pairs into the bare
        // value list `symiri --` expects.
        std::vector<std::string> paramArgs;
        paramArgs.reserve(cf.rz.paramValues.size());
        for (const auto &pv: cf.rz.paramValues)
          paramArgs.push_back(pv.second);
        // Run the on-disk full program through symiri once. When
        // validating, its Result is asserted equal to the descriptor's
        // retValue (captured from the independent minimal oracle at emit
        // time) — that cross-check exercises the rewriter, the CFG body
        // execution, and intrinsic dispatch in one shot; exit code 0
        // alone would only prove symiri didn't crash. When --emit-state
        // is on, the SAME run also fills the rytwin state profile (see
        // runSymiriCaptureResult), so profiling costs no extra interpret.
        // A UB-triggering program (require-ub) traps before producing a
        // clean trace, so it is validated via the trap and yields no
        // sidecar.
        bool ok = !doValidate; // nothing to fail when only profiling
        std::string mismatchReason;
        if (solMode == SolvingMode::RequireUB) {
          if (doValidate) {
            ok = validateWithSymiri(p, baseFuncName, paramArgs, verbose, solMode);
            if (!ok)
              mismatchReason = "Expected UB but program executed successfully or failed statically";
          }
        } else {
          StateProfile profile;
          auto observed = runSymiriCaptureResult(
              p, baseFuncName, paramArgs, wantProfile ? &profile : nullptr, stGran
          );
          if (wantProfile && observed) {
            std::ofstream sofs(outDir / (p.stem().string() + ".state.json"));
            if (sofs)
              writeStateProfileJson(sofs, profile);
          }
          if (doValidate) {
            if (observed) {
              if (cf.rz.retValue.empty()) {
                // No oracle to compare against (e.g. the rewrite was
                // skipped). Fall back to the exit-code check.
                ok = validateWithSymiri(p, baseFuncName, paramArgs, verbose, solMode);
              } else if (*observed == cf.rz.retValue) {
                ok = true;
              } else {
                mismatchReason = "expected=" + cf.rz.retValue + " observed=" + *observed;
              }
            } else {
              mismatchReason = "symiri produced no Result line";
            }
          }
        }
        if (doValidate) {
          std::cout << "  validated: " << (ok ? "OK" : "FAIL") << " (" << p.filename() << ")";
          if (!ok && !mismatchReason.empty())
            std::cout << " [" << mismatchReason << "]";
          std::cout << "\n";
          if (!ok) {
            allOk = false;
            nFail++;
            break;
          }
        }
      }
      if (!doValidate || allOk)
        nOk++;
    } else {
      nOk++;
    }
  }

  // [v0.2.2] Single end-of-run orphan sweep.  When a per-function wall-clock
  // timeout fires, the detached worker may have already written some
  // concrete .sir files into outDir before we abandoned it.  The compile-
  // to-target step then skips that function, leaving the .sir on disk
  // with no matching .c/.wat.  Downstream consumers (reify-diff) see a
  // .sir without its companion and mis-classify it as a `cfail` symirc
  // bug instead of the rysmith timeout it really is.  A single pass after
  // the main loop is O(N) and handles every timed-out function uniformly
  // — much cheaper than scanning the whole directory inside each
  // [TIMEOUT] branch.  A residual race where the detached thread writes
  // a new file *after* this sweep is bounded: when rysmith returns, the
  // subprocess exits and the OS reaps any surviving worker.
  //
  // The `--keep-symbolic` switch emits `<stem><kSymInfix><N>.sir` files
  // (e.g. `func_ab12cd_42_sym0.sir`) that are pre-solve sources and
  // intentionally have no .c twin.  Detect them by the kSymInfix +
  // trailing-digits pattern and exclude from the orphan check.
  if (target != "sir") {
    std::string ext = (target == "c") ? ".c" : ".wat";
    std::string symInfix = reify::rysmith::hp::kSymInfix;
    std::vector<fs::path> orphanSirs;
    std::error_code ec;
    for (auto &entry: fs::directory_iterator(outDir, ec)) {
      const auto &p = entry.path();
      if (p.extension() != ".sir")
        continue;
      auto stem = p.stem().string();
      // Skip --keep-symbolic preserve files: stem ends with kSymInfix
      // followed by one or more digits.
      auto symPos = stem.rfind(symInfix);
      if (symPos != std::string::npos && symPos + symInfix.size() < stem.size()) {
        std::string tail = stem.substr(symPos + symInfix.size());
        if (!tail.empty() &&
            std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); }))
          continue;
      }
      auto twin = p.parent_path() / (stem + ext);
      if (!fs::exists(twin, ec))
        orphanSirs.push_back(p);
    }
    for (const auto &p: orphanSirs)
      fs::remove(p, ec);
    if (!orphanSirs.empty())
      std::cerr << "[CLEANUP] removed " << orphanSirs.size()
                << " orphan .sir file(s) from timed-out functions\n";
  }

  auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
  double throughput = elapsed > 0 ? nOk / elapsed : 0.0;
  std::cout << "\nDone: " << nOk << " succeeded, " << nFail << " failed (total " << nFuncs << ")"
            << "  [" << elapsed << "s, " << throughput << " funcs/s]\n";

  return 0;
}
