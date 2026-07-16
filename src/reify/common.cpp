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
#include "frontend/diagnostics.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"
#include "interp/interpreter.hpp"

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

  // [v0.2.3] Structured emission (C --structured-lowering, python) is
  // only total on reducible CFGs. Callers filter or repair upstream;
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
      const std::string &vecLowering, bool structuredLowering, bool emitMain, bool splitBySource,
      bool verbose
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
      Program &prog, const fs::path &outFile, bool keepRequire, bool emitMain, bool verbose
  ) {
    if (!runAnalysisPasses(prog, verbose))
      return false;
    std::ofstream ofs(outFile);
    if (!ofs) {
      if (verbose)
        std::cerr << "reify: cannot open " << outFile << "\n";
      return false;
    }
    WasmBackend wb(ofs);
    wb.setNoRequire(!keepRequire);
    wb.setNoMainMangle(emitMain);
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
      Program &prog, const fs::path &outFile, bool keepRequire, const std::string &vecLowering,
      bool emitMain, bool verbose
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
      const std::string &vecLowering, bool structuredLowering, bool emitMain, bool verbose
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
            prog, outPath.parent_path(), outPath.stem().string(), keepRequire, vecLowering,
            structuredLowering, emitMain, /*splitBySource=*/false, verbose
        );
      } else if (target == "wasm") {
        return emitWasmInProcess(prog, outPath, keepRequire, emitMain, verbose);
      } else if (target == "python") {
        return emitPyInProcess(prog, outPath, keepRequire, vecLowering, emitMain, verbose);
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
