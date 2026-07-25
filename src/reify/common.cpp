#include "reify/common.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <fstream>
#include <iostream>
#include <sstream>

#include "analysis/definite_init.hpp"
#include "analysis/dominators.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/reducibility.hpp"
#include "analysis/unused_name.hpp"
#include "ast/sir_printer.hpp"
#include "backend/c_backend.hpp"
#include "backend/c_vec_lowering.hpp"
#include "backend/py_backend.hpp"
#include "backend/wasm_backend.hpp"
#include "error.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "interp/interpreter.hpp"
#include "reify/state_profile.hpp"

namespace fs = std::filesystem;
using namespace refractir;

namespace refractir::reify {

  bool runAnalysisPasses(Program &prog, bool verbose) {
    DiagBag diags;
    PassManager pm(diags);
    pm.addModulePass(std::make_unique<SemChecker>());
    pm.addModulePass(std::make_unique<TypeChecker>());
    pm.addFunctionPass(std::make_unique<ReachabilityAnalysis>());
    pm.addFunctionPass(std::make_unique<DefiniteInitAnalysis>());
    pm.addFunctionPass(std::make_unique<UnusedNameAnalysis>());
    if (pm.run(prog) == PassResult::Error) {
      if (verbose) {
        std::cerr << "reify: analysis passes failed:\n";
        for (const auto &d: diags.diags)
          if (d.level == DiagLevel::Error)
            std::cerr << "  error: " << d.message << "\n";
      }
      return false;
    }
    return true;
  }

  std::optional<std::string> runSymiriCaptureResult(
      const fs::path &sirPath, const std::string &funcName,
      const std::vector<std::string> &paramArgs, StateProfile *outProfile, StateGranularity gran
  ) {
    std::ifstream ifs(sirPath);
    if (!ifs)
      return std::nullopt;
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string src = ss.str();

    try {
      Lexer lx(src);
      auto toks = lx.lexAll();
      Parser ps(std::move(toks));
      Program prog = ps.parseProgram();

      // Run semantics/type check passes first to ensure it's valid
      if (!runAnalysisPasses(prog, /*verbose=*/false))
        return std::nullopt;

      std::string canonical = funcName.empty() || funcName[0] == '@' ? funcName : "@" + funcName;

      // Capture "Result: <value>" via a local sink rather than redirecting
      // the process-global std::cout, which races with concurrent worker
      // threads (rysmith runs one generation thread per function).
      std::stringstream capturedStream;
      try {
        Interpreter interp(prog, capturedStream);
        // Capture the state profile from this same run when requested.
        if (outProfile) {
          outProfile->func = canonical;
          outProfile->granularity = gran;
          attachStateProfile(interp, *outProfile, gran);
        }
        interp.run(canonical, {}, paramArgs);
      } catch (...) {
        return std::nullopt;
      }

      std::string out = capturedStream.str();
      auto pos = out.rfind("Result:");
      if (pos == std::string::npos)
        return std::nullopt;
      pos += 7; // past "Result:"
      while (pos < out.size() && (out[pos] == ' ' || out[pos] == '\t'))
        ++pos;
      auto end = out.find_first_of("\r\n", pos);
      std::string val = out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      if (val.empty())
        return std::nullopt;
      return val;
    } catch (...) {
      return std::nullopt;
    }
  }

  bool validateNontermDiverges(
      const fs::path &sirPath, const std::string &funcName,
      const std::vector<std::string> &paramArgs, const std::string &headerLabel, int period,
      std::uint64_t maxBlocks
  ) {
    if (headerLabel.empty())
      return false;
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

      std::stringstream sink;
      Interpreter interp(prog, sink);
      StateProfile profile;
      profile.func = canonical;
      profile.granularity = StateGranularity::Pbb;
      attachStateProfile(interp, profile, StateGranularity::Pbb);
      interp.setMaxBlockSteps(maxBlocks);

      try {
        interp.run(canonical, {}, paramArgs);
        return false; // returned within budget => terminated, not diverging
      } catch (const StepLimitError &) {
        // Ran the whole budget without returning — the expected outcome for a
        // divergent loop. Fall through to the header-recurrence check.
      } catch (...) {
        return false; // UB / require / other => not a clean divergence
      }

      // Confirm the header-state fixed point at runtime: two arrivals at the
      // lasso header exactly `period` laps apart must carry bit-identical
      // state. Two such identical states in a deterministic program prove the
      // orbit repeats forever, which is the property we are certifying. For a
      // period-k orbit *consecutive* arrivals deliberately differ, so the gap
      // has to be k, not 1.
      //
      // The last arrivals rather than the first, because a local declared
      // `= undef` does not enter the store until it is first assigned: at the
      // header's first visit the state is both smaller and partly undefined
      // (the usual case, since the header is often the entry block that
      // initializes the pointers). Late visits are past all initialization, so
      // the comparison needs no exemption and stays strictly bit-exact.
      const int k = std::max(1, period);
      std::vector<const StatePoint *> arrivals;
      for (const auto &pt: profile.trace)
        if (pt.instr == -1 && pt.block == headerLabel)
          arrivals.push_back(&pt);
      if ((int) arrivals.size() < k + 1)
        return false; // fewer than one full orbit within the budget
      const StatePoint *last = arrivals.back();
      const StatePoint *prev = arrivals[arrivals.size() - 1 - k];
      if (prev->vars.size() != last->vars.size())
        return false;
      for (std::size_t i = 0; i < last->vars.size(); ++i) {
        if (last->vars[i].first != prev->vars[i].first)
          return false;
        if (!bitExactEq(last->vars[i].second, prev->vars[i].second))
          return false;
      }
      return true; // header state recurred after k laps => diverges
    } catch (...) {
      return false;
    }
  }

  bool programTraps(
      const fs::path &sirPath, const std::string &funcName,
      const std::vector<std::string> &paramArgs
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
      std::stringstream sink;
      Interpreter interp(prog, sink);
      try {
        interp.run(canonical, {}, paramArgs);
        return false; // returned cleanly — no UB
      } catch (const UndefinedBehaviorError &) {
        return true; // trapped, as a --require-ub program should
      } catch (...) {
        return false; // require-failure / other — not the UB we wanted
      }
    } catch (...) {
      return false;
    }
  }

  // [v0.2.3] Structured emission (C/WASM --structured-lowering, python)
  // is only total on reducible CFGs. Callers filter or repair upstream;
  // verify here so a violation is a clean failure instead of
  // malformed backend output.
  static bool allFunsReducible(const Program &prog, bool verbose) {
    for (const auto &f: prog.funs) {
      DiagBag diags;
      CFG cfg = CFG::build(f, diags);
      DomTree dt = DomTree::build(cfg);
      if (!ReducibilityResult::check(cfg, dt).reducible()) {
        if (verbose)
          std::cerr << "reify: structured lowering requires reducible control flow: " << f.name.name
                    << "\n";
        return false;
      }
    }
    return true;
  }

  bool emitCInProcess(
      Program &prog, const fs::path &outDir, const std::string &primaryStem, bool keepRequire,
      bool noUbGuards, const std::string &vecLowering, bool structuredLowering, bool emitMain,
      bool splitBySource, bool verbose
  ) {
    if (!runAnalysisPasses(prog, verbose))
      return false;
    if (structuredLowering && !allFunsReducible(prog, verbose))
      return false;
    auto vl = makeCVecLowering(vecLowering.empty() ? "vecext" : vecLowering);
    if (splitBySource) {
      std::ofstream sink;
      CBackend cb(sink);
      cb.setNoRequire(!keepRequire);
      cb.setNoUbGuards(noUbGuards);
      cb.setNoMainMangle(emitMain);
      cb.setStructuredLowering(structuredLowering);
      cb.setVecLowering(std::move(vl));
      try {
        cb.emitSplit(prog, outDir.string(), primaryStem);
      } catch (const std::exception &e) {
        if (verbose)
          std::cerr << "reify: CBackend failed: " << e.what() << "\n";
        return false;
      }
      return true;
    }
    fs::path outFile = outDir / (primaryStem + ".c");
    std::ofstream ofs(outFile);
    if (!ofs) {
      if (verbose)
        std::cerr << "reify: cannot open " << outFile << "\n";
      return false;
    }
    CBackend cb(ofs);
    cb.setNoRequire(!keepRequire);
    cb.setNoUbGuards(noUbGuards);
    cb.setNoMainMangle(emitMain);
    cb.setStructuredLowering(structuredLowering);
    cb.setVecLowering(std::move(vl));
    try {
      cb.emit(prog);
    } catch (const std::exception &e) {
      if (verbose)
        std::cerr << "reify: CBackend failed: " << e.what() << "\n";
      return false;
    }
    return true;
  }

  bool emitWasmInProcess(
      Program &prog, const fs::path &outFile, bool keepRequire, bool noUbGuards,
      const std::string &vecLowering, bool structuredLowering, bool emitMain, bool verbose
  ) {
    if (!runAnalysisPasses(prog, verbose))
      return false;
    if (structuredLowering && !allFunsReducible(prog, verbose))
      return false;
    std::ofstream ofs(outFile);
    if (!ofs) {
      if (verbose)
        std::cerr << "reify: cannot open " << outFile << "\n";
      return false;
    }
    auto vl = makeWasmVecLowering(vecLowering.empty() ? "vecext" : vecLowering);
    if (!vl) {
      if (verbose)
        std::cerr << "reify: WASM target does not support vec-lowering '" << vecLowering << "'\n";
      return false;
    }
    WasmBackend wb(ofs);
    wb.setNoRequire(!keepRequire);
    wb.setNoUbGuards(noUbGuards);
    wb.setNoMainMangle(emitMain);
    wb.setStructuredLowering(structuredLowering);
    wb.setVecLowering(std::move(vl));
    try {
      wb.emit(prog);
    } catch (const std::exception &e) {
      if (verbose)
        std::cerr << "reify: WasmBackend failed: " << e.what() << "\n";
      return false;
    }
    return true;
  }

  bool emitPyInProcess(
      Program &prog, const fs::path &outFile, bool keepRequire, bool noUbGuards,
      const std::string &vecLowering, bool emitMain, bool verbose
  ) {
    if (!runAnalysisPasses(prog, verbose))
      return false;
    if (!allFunsReducible(prog, verbose))
      return false;
    auto vl = makePyVecLowering(vecLowering.empty() ? "array" : vecLowering);
    if (!vl) {
      if (verbose)
        std::cerr << "reify: python target does not support vec-lowering '" << vecLowering << "'\n";
      return false;
    }
    std::ofstream ofs(outFile);
    if (!ofs) {
      if (verbose)
        std::cerr << "reify: cannot open " << outFile << "\n";
      return false;
    }
    PyBackend pb(ofs);
    pb.setNoRequire(!keepRequire);
    pb.setNoUbGuards(noUbGuards);
    pb.setNoMainMangle(emitMain);
    pb.setVecLowering(std::move(vl));
    try {
      pb.emit(prog);
    } catch (const std::exception &e) {
      if (verbose)
        std::cerr << "reify: PyBackend failed: " << e.what() << "\n";
      return false;
    }
    return true;
  }

  bool compileSirInProcess(
      const fs::path &sirPath, const std::string &target, const fs::path &outPath, bool keepRequire,
      bool noUbGuards, const std::string &vecLowering, bool structuredLowering, bool emitMain,
      bool verbose
  ) {
    std::ifstream ifs(sirPath);
    if (!ifs) {
      if (verbose)
        std::cerr << "compileSirInProcess: Could not open file " << sirPath << "\n";
      return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string src = ss.str();

    try {
      Lexer lx(src);
      auto toks = lx.lexAll();
      Parser ps(std::move(toks));
      Program prog = ps.parseProgram();

      if (target == "c") {
        return emitCInProcess(
            prog, outPath.parent_path(), outPath.stem().string(), keepRequire, noUbGuards,
            vecLowering, structuredLowering, emitMain, /*splitBySource=*/false, verbose
        );
      } else if (target == "wasm") {
        return emitWasmInProcess(
            prog, outPath, keepRequire, noUbGuards, vecLowering, structuredLowering, emitMain,
            verbose
        );
      } else if (target == "python") {
        return emitPyInProcess(
            prog, outPath, keepRequire, noUbGuards, vecLowering, emitMain, verbose
        );
      } else {
        if (verbose)
          std::cerr << "compileSirInProcess: Unknown target " << target << "\n";
        return false;
      }
    } catch (const std::exception &e) {
      if (verbose)
        std::cerr << "compileSirInProcess: Exception during compilation: " << e.what() << "\n";
      return false;
    }
  }

} // namespace refractir::reify
