#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "analysis/definite_init.hpp"
#include "analysis/dominators.hpp"
#include "analysis/loop_info.hpp"
#include "analysis/pass_manager.hpp"
#include "analysis/reachability.hpp"
#include "analysis/reducibility.hpp"
#include "analysis/structured_lowering.hpp"
#include "analysis/structurizer.hpp"
#include "analysis/unused_name.hpp"
#include "ast/ast_dumper.hpp"
#include "backend/c_backend.hpp"
#include "backend/wasm_backend.hpp"
#include "cxxopts.hpp"
#include "error.hpp"
#include "frontend/lexer.hpp"
#include "frontend/link_resolver.hpp"
#include "frontend/parser.hpp"
#include "frontend/semchecker.hpp"
#include "frontend/typechecker.hpp"

int main(int argc, char **argv) {
  using namespace refractir;

  cxxopts::Options options("symirc", "RefractIR Compiler");

  // clang-format off
  options.add_options()
    ("input", "Input .sir file", cxxopts::value<std::string>())
    ("o,output", "Output file (default: stdout)", cxxopts::value<std::string>())
    ("target", "Backend target (c, wasm)", cxxopts::value<std::string>()->default_value("c"))
    ("require-reducible", "Reject functions with irreducible control flow", cxxopts::value<bool>()->default_value("false"))
    ("dump-ast", "Dump AST to stdout and exit", cxxopts::value<bool>()->default_value("false"))
    ("dump-domtree", "Dump per-function dominator trees to stdout and exit", cxxopts::value<bool>()->default_value("false"))
    ("dump-loops", "Dump per-function loop nesting forests to stdout and exit", cxxopts::value<bool>()->default_value("false"))
    ("dump-control-tree", "Dump per-function structured control trees to stdout and exit (implies --require-reducible)", cxxopts::value<bool>()->default_value("false"))
    ("dump-lowered-tree", "Dump per-function control trees after structured lowering and exit (implies --require-reducible)", cxxopts::value<bool>()->default_value("false"))
    ("w", "Inhibit all warning messages", cxxopts::value<bool>()->default_value("false"))
    ("Werror", "Make all warnings into errors", cxxopts::value<bool>()->default_value("false"))
    ("no-module-tags", "Omit (module ...) tags in WASM output", cxxopts::value<bool>()->default_value("false"))
    ("no-require", "Omit require checks from emitted code (useful for compiler testing)", cxxopts::value<bool>()->default_value("false"))
    ("vec-lowering", "C-backend vector lowering: vecext|scalars|array|structscalars|structarray", cxxopts::value<std::string>()->default_value("vecext"))
    ("I", "Include path for resolving link-form `decl`s (may repeat)", cxxopts::value<std::vector<std::string>>())
    ("split-by-source", "C target: emit one <stem>.c per source file plus common.h into the directory given by -o (instead of a single bundled .c)", cxxopts::value<bool>()->default_value("false"))
    ("emit-main", "Do not mangle @main function in emitted target code", cxxopts::value<bool>()->default_value("false"))
    ("h,help", "Print usage");
  options.parse_positional({"input"});
  // clang-format on

  auto result = options.parse(argc, argv);

  if (result.count("help")) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  if (!result.count("input")) {
    std::cerr << "Error: No input file specified." << std::endl;
    std::cerr << options.help() << std::endl;
    return 1;
  }

  std::string inputPath = result["input"].as<std::string>();
  std::ifstream ifs(inputPath);
  if (!ifs) {
    std::cerr << "Error: Could not open file " << inputPath << "\n";
    return 1;
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string src = ss.str();

  try {
    // 1. Frontend
    Lexer lx(src);
    auto toks = lx.lexAll();
    Parser ps(std::move(toks));
    Program prog = ps.parseProgram();

    // 1b. [v0.2.2] -I link-form resolution.
    std::vector<Program> libs;
    if (result.count("I")) {
      libs = loadIncludeDirs(result["I"].as<std::vector<std::string>>());
    }
    resolveLinkDecls(prog, libs);

    if (result["dump-ast"].as<bool>()) {
      ASTDumper dumper(std::cout);
      dumper.dump(prog);
      return 0;
    }

    std::string target = result["target"].as<std::string>();

    // 2. Analysis
    DiagBag diags;
    refractir::PassManager pm(diags);
    pm.addModulePass(std::make_unique<SemChecker>());
    pm.addModulePass(std::make_unique<TypeChecker>());
    pm.addFunctionPass(std::make_unique<ReachabilityAnalysis>());
    pm.addFunctionPass(std::make_unique<DefiniteInitAnalysis>());
    pm.addFunctionPass(std::make_unique<UnusedNameAnalysis>());
    // The structurizer is only total on reducible CFGs, so the
    // control-tree dump flags imply the check.
    if (result["require-reducible"].as<bool>() || result["dump-control-tree"].as<bool>() ||
        result["dump-lowered-tree"].as<bool>()) {
      pm.addFunctionPass(std::make_unique<ReducibilityCheck>());
    }

    bool werror = result["Werror"].as<bool>();
    bool nowarn = result["w"].as<bool>();

    if (pm.run(prog) == refractir::PassResult::Error || (werror && diags.hasWarnings())) {
      std::cerr << "Errors:\n";
      for (const auto &d: diags.diags) {
        // Notes only ever accompany errors (e.g. ReducibilityCheck points
        // at the multi-entry loop header), so print them alongside.
        if (d.level == DiagLevel::Error || d.level == DiagLevel::Note ||
            (werror && d.level == DiagLevel::Warning)) {
          printMessage(std::cerr, src, d.span, d.message, d.level);
        }
      }
      return ExitCode::StaticError;
    }

    // Print warnings
    if (!nowarn) {
      for (const auto &d: diags.diags) {
        if (d.level == DiagLevel::Warning) {
          printMessage(std::cerr, src, d.span, d.message, d.level);
        }
      }
    }

    // Analysis dump flags: print after the pipeline has validated the
    // program (so every CFG is well-formed) and exit without emitting.
    bool dumpDomtree = result["dump-domtree"].as<bool>();
    bool dumpLoops = result["dump-loops"].as<bool>();
    bool dumpControlTree = result["dump-control-tree"].as<bool>();
    bool dumpLoweredTree = result["dump-lowered-tree"].as<bool>();
    if (dumpDomtree || dumpLoops || dumpControlTree || dumpLoweredTree) {
      bool first = true;
      for (const auto &f: prog.funs) {
        if (!first)
          std::cout << "\n";
        first = false;
        CFG cfg = CFG::build(f, diags);
        DomTree dt = DomTree::build(cfg);
        if (dumpDomtree)
          dt.dump(std::cout, cfg, f.name.name);
        if (dumpLoops || dumpControlTree || dumpLoweredTree) {
          LoopInfo li = LoopInfo::build(cfg, dt);
          if (dumpLoops)
            li.dump(std::cout, cfg, f.name.name);
          if (dumpControlTree)
            Structurizer::build(f, cfg, dt, li).dump(std::cout, cfg, f.name.name);
          if (dumpLoweredTree) {
            ControlTree lowered =
                StructuredLowering::run(Structurizer::build(f, cfg, dt, li), f, cfg);
            lowered.dump(std::cout, cfg, f.name.name);
          }
        }
      }
      return 0;
    }

    // 3. Backend
    std::ostream *outStream = &std::cout;
    std::ofstream ofs;

    bool splitBySource = result.count("split-by-source") && result["split-by-source"].as<bool>();
    if (result.count("output") && !splitBySource) {
      // In split-by-source mode the -o argument is a directory; the
      // split path opens its own per-file streams. Skip opening a
      // single-file output here.
      std::string outPath = result["output"].as<std::string>();
      ofs.open(outPath);
      if (!ofs) {
        std::cerr << "Error: Could not open output file " << outPath << "\n";
        return 1;
      }
      outStream = &ofs;
    }

    bool noRequire = result["no-require"].as<bool>();
    bool emitMain = result["emit-main"].as<bool>();
    if (target == "c") {
      // [v0.2.2] --split-by-source: emit one <stem>.c per source file
      // + common.h into the directory specified by -o.
      if (result["split-by-source"].as<bool>()) {
        if (!result.count("output")) {
          std::cerr << "Error: --split-by-source requires -o <directory>\n";
          return 1;
        }
        std::string outDir = result["output"].as<std::string>();
        // We don't write to outStream in this branch; close the file if
        // -o opened one (it pointed at outDir as if it were a file).
        if (ofs.is_open())
          ofs.close();
        // Compute the primary file's stem from the input path.
        std::string primaryStem = std::filesystem::path(inputPath).stem().string();
        CBackend cb(std::cout); // sink; unused — emitSplit opens its own streams
        cb.setNoRequire(noRequire);
        cb.setNoMainMangle(emitMain);
        std::string vlName = result["vec-lowering"].as<std::string>();
        auto vl = makeVecLowering(vlName);
        if (!vl) {
          std::cerr << "Error: unknown --vec-lowering '" << vlName << "'\n";
          return 1;
        }
        cb.setVecLowering(std::move(vl));
        // [v0.2.2] No per-file `wrote …` chatter. rylink consumes this
        // path and wants a clean stderr so its own `completed: …`
        // per-program log line is the only thing the user sees; the
        // returned list is dropped and the caller inspects the output
        // directory if it cares about which files landed.
        (void) cb.emitSplit(prog, outDir, primaryStem);
      } else {
        CBackend cb(*outStream);
        cb.setNoRequire(noRequire);
        cb.setNoMainMangle(emitMain);
        // [v0.2.1] Set up the vector-lowering strategy.
        std::string vlName = result["vec-lowering"].as<std::string>();
        auto vl = makeVecLowering(vlName);
        if (!vl) {
          std::cerr << "Error: unknown --vec-lowering '" << vlName
                    << "' (try vecext|scalars|array|structscalars|structarray)\n";
          return 1;
        }
        cb.setVecLowering(std::move(vl));
        cb.emit(prog);
      }
    } else if (target == "wasm") {
      WasmBackend wb(*outStream);
      if (result.count("no-module-tags")) {
        wb.setNoModuleTags(result["no-module-tags"].as<bool>());
      }
      wb.setNoRequire(noRequire);
      wb.setNoMainMangle(emitMain);
      wb.emit(prog);
    } else {
      std::cerr << "Error: Unsupported target: " << target << "\n";
      return 1;
    }

  } catch (const LexError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return ExitCode::LexError;
  } catch (const ParseError &e) {
    printMessage(std::cerr, src, e.span, e.what(), DiagLevel::Error);
    return ExitCode::ParseError;
  } catch (const std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return ExitCode::Error;
  }

  return 0;
}
