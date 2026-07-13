/**
 * rytwin — equivalence-preserving RefractIR program transformer (v0.2.3).
 *
 * Given a rysmith program `p1` (a concrete .sir) together with its state
 * profile — the per-program-point concrete state `p1` passes through under
 * its generated input, emitted by `rysmith --emit-state` — rytwin produces
 * an equivalent program `p2`. It does so by grafting, into selected basic
 * blocks, a synthesized twin block `B'` that reproduces `B`'s effect on the
 * exact state `B` sees, guarded by a checksum of that live-in state so `B'`
 * runs only on it and the original `B` runs otherwise. Thus `p1(i) == p2(i)`
 * for every input `i` (see docs/rytwin.md).
 *
 * This file is the scaffold: it wires the CLI, loads the program /
 * descriptor / state profile, and runs the Pass pipeline. With no passes
 * registered it round-trips p1 -> p2 unchanged, so the plumbing is
 * verifiable before TwinGraftPass lands.
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "ast/sir_printer.hpp"
#include "cxxopts.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "reify/common.hpp"
#include "reify/func_desc.hpp"
#include "reify/pass.hpp"
#include "reify/state_profile.hpp"
#include "solver/solver.hpp"
#if defined(USE_BITWUZLA)
#include "solver/bitwuzla_impl.hpp"
#elif defined(USE_ALIVESMT)
#include "solver/alive_impl.hpp"
#endif

namespace fs = std::filesystem;
using namespace refractir;
using namespace refractir::reify;

static std::string readFile(const fs::path &p) {
  std::ifstream ifs(p);
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

static SymbolicExecutor::SolverFactory makeSolverFactory() {
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

// The entry function of a rysmith leaf program: the sole `fun` that isn't
// the optional `@main` wrapper. Prefer the descriptor's name when we have
// one, since it is authoritative.
static std::string findEntry(const Program &prog, const std::optional<FuncDescriptor> &desc) {
  if (desc && !desc->name.empty())
    return desc->name;
  for (const auto &f: prog.funs)
    if (f.name.name != "@main")
      return f.name.name;
  return prog.funs.empty() ? std::string{} : prog.funs.front().name.name;
}

int main(int argc, char **argv) {
  cxxopts::Options opts("rytwin", "rytwin — equivalence-preserving RefractIR transformer");
  // clang-format off
  opts.add_options()
    ("input",   "Input concrete .sir (p1)", cxxopts::value<std::string>())
    ("desc",    "Descriptor JSON for p1's function (func_<id>_<i>.json)",
                cxxopts::value<std::string>())
    ("state",   "State profile sidecar (.state.json); inferred from the input stem if omitted",
                cxxopts::value<std::string>())
    ("p-twin",  "Probability of grafting a twin for each candidate block",
                cxxopts::value<double>()->default_value("0.5"))
    ("guard",   "state() checksum used by the twin guard: sum|crc32",
                cxxopts::value<std::string>()->default_value("sum"))
    ("seed",    "RNG seed (default: random)", cxxopts::value<uint32_t>())
    ("o,output","Output .sir (p2)", cxxopts::value<std::string>())
    ("h,help",  "Print usage");
  opts.parse_positional({"input"});
  opts.positional_help("<p1.sir>");
  // clang-format on

  cxxopts::ParseResult result;
  try {
    result = opts.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "rytwin: " << e.what() << "\n";
    return 2;
  }
  if (result.count("help") || !result.count("input") || !result.count("output")) {
    std::cout << opts.help() << "\n";
    return result.count("help") ? 0 : 2;
  }

  fs::path inputPath = result["input"].as<std::string>();
  fs::path outputPath = result["output"].as<std::string>();
  double pTwin = result["p-twin"].as<double>();
  std::string guard = result["guard"].as<std::string>();
  uint32_t seed =
      result.count("seed") ? result["seed"].as<uint32_t>() : (uint32_t) std::random_device{}();
  (void) pTwin; // consumed by TwinGraftPass (next chunk)

  if (guard != "sum" && guard != "crc32") {
    std::cerr << "rytwin: --guard must be 'sum' or 'crc32' (got '" << guard << "')\n";
    return 2;
  }

  // 1. Load p1. Keep the source alive: Lexer holds a std::string_view into
  // it, so a temporary would dangle.
  Program prog;
  std::string src = readFile(inputPath);
  try {
    Lexer lx(src);
    Parser ps(lx.lexAll());
    prog = ps.parseProgram();
  } catch (const std::exception &e) {
    std::cerr << "rytwin: failed to parse " << inputPath << ": " << e.what() << "\n";
    return 1;
  }
  if (!runAnalysisPasses(prog, /*verbose=*/true)) {
    std::cerr << "rytwin: analysis of " << inputPath << " failed\n";
    return 1;
  }

  // 2. Load the descriptor (optional) and resolve the entry function.
  std::optional<FuncDescriptor> desc;
  if (result.count("desc")) {
    desc = readFuncDescriptor(result["desc"].as<std::string>());
    if (!desc)
      std::cerr << "rytwin: warning: could not read descriptor " << result["desc"].as<std::string>()
                << "\n";
  }
  std::string entry = findEntry(prog, desc);
  if (entry.empty()) {
    std::cerr << "rytwin: no entry function found in " << inputPath << "\n";
    return 1;
  }

  // 3. Load the state profile (inferred as <input-stem>.state.json when
  // --state is omitted). rytwin needs it to synthesize twins; the scaffold
  // still round-trips without one.
  std::optional<StateProfile> profile;
  fs::path statePath = result.count("state")
                           ? fs::path(result["state"].as<std::string>())
                           : inputPath.parent_path() / (inputPath.stem().string() + ".state.json");
  if (fs::exists(statePath)) {
    profile = readStateProfileJson(readFile(statePath));
    if (!profile)
      std::cerr << "rytwin: warning: could not parse state profile " << statePath << "\n";
  } else if (result.count("state")) {
    std::cerr << "rytwin: warning: state profile " << statePath << " not found\n";
  }

  // 4. Assemble the pass context.
  PassCtx ctx;
  ctx.rng.seed(seed);
  ctx.solverFactory = makeSolverFactory();
  if (desc)
    ctx.descriptors[entry] = *desc;
  if (profile)
    ctx.profiles[entry] = *profile;

  // 5. Run the pipeline. (TwinGraftPass is added in the next chunk.)
  PassPipeline pipe;
  PassReport rep = pipe.run(prog, ctx);
  if (!rep.ok) {
    std::cerr << "rytwin: pass failed: " << rep.message << "\n";
    return 1;
  }

  // 6. Emit p2.
  std::ofstream ofs(outputPath);
  if (!ofs) {
    std::cerr << "rytwin: cannot open " << outputPath << " for writing\n";
    return 1;
  }
  ofs << "// rytwin: equivalent of " << inputPath.filename().string() << " (" << rep.sites
      << " twin(s) grafted)\n\n";
  SIRPrinter printer(ofs);
  printer.print(prog);

  std::cout << "rytwin: wrote " << outputPath << " (" << rep.sites << " twin(s), entry " << entry
            << ")\n";
  return 0;
}
