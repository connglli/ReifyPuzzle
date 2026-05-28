#pragma once

// [v0.2.2] -I include-path resolution shared by symiri / symirc / symirsolve.
// Inline because the implementation is small and trivial to inline.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"

namespace symir {

  /**
   * Load every `.sir` file under each `-I` directory (recursively) and
   * return the parsed Programs. The caller must keep the returned vector
   * alive for as long as references into it (e.g. via resolveLinkDecls)
   * are used.
   */
  inline std::vector<Program> loadIncludeDirs(const std::vector<std::string> &dirs) {
    std::vector<Program> libs;
    for (const auto &dir: dirs) {
      if (!std::filesystem::exists(dir))
        throw std::runtime_error("-I path does not exist: " + dir);
      for (const auto &entry: std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file())
          continue;
        if (entry.path().extension() != ".sir")
          continue;
        std::ifstream ifs(entry.path());
        std::stringstream ss;
        ss << ifs.rdbuf();
        std::string libSrc = ss.str();
        try {
          Lexer lx(libSrc);
          auto toks = lx.lexAll();
          Parser ps(std::move(toks));
          libs.push_back(ps.parseProgram());
        } catch (const LexError &e) {
          std::cerr << "Error in -I file " << entry.path() << ": " << e.what() << "\n";
          throw;
        } catch (const ParseError &e) {
          std::cerr << "Error in -I file " << entry.path() << ": " << e.what() << "\n";
          throw;
        }
      }
    }
    return libs;
  }

  /**
   * Resolve every link-form `decl @name` in `main` against the loaded
   * library Programs. For each match, move the lib's `fun` into
   * `main.funs` and drop the matching link decl. Pull in any new struct
   * or intrinsic declarations the moved fun depends on. Contract-form
   * decls are left untouched.
   */
  inline void resolveLinkDecls(Program &main, std::vector<Program> &libs) {
    std::unordered_set<std::string> mainFunNames;
    for (const auto &f: main.funs)
      mainFunNames.insert(f.name.name);

    std::vector<ExtDecl> keep;
    for (auto &d: main.extDecls) {
      if (d.contract) {
        keep.push_back(std::move(d));
        continue;
      }
      if (mainFunNames.count(d.name.name)) {
        // Conflicts with a local fun — leave it for SemChecker.
        keep.push_back(std::move(d));
        continue;
      }
      bool found = false;
      for (auto &lib: libs) {
        for (auto it = lib.funs.begin(); it != lib.funs.end(); ++it) {
          if (it->name.name == d.name.name) {
            main.funs.push_back(std::move(*it));
            lib.funs.erase(it);
            mainFunNames.insert(d.name.name);
            found = true;
            break;
          }
        }
        if (found)
          break;
      }
      if (!found)
        keep.push_back(std::move(d));
    }
    main.extDecls = std::move(keep);

    // Pull in structs from libs that aren't already in main.
    std::unordered_set<std::string> mainStructNames;
    for (const auto &s: main.structs)
      mainStructNames.insert(s.name.name);
    for (auto &lib: libs) {
      for (auto it = lib.structs.begin(); it != lib.structs.end();) {
        if (!mainStructNames.count(it->name.name)) {
          main.structs.push_back(std::move(*it));
          mainStructNames.insert(main.structs.back().name.name);
          it = lib.structs.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Pull in intrinsic decls from libs (lib funs may call them).
    std::unordered_set<std::string> mainIntrNames;
    for (const auto &i: main.intrinsics)
      mainIntrNames.insert(i.name.name);
    for (auto &lib: libs) {
      for (auto it = lib.intrinsics.begin(); it != lib.intrinsics.end();) {
        if (!mainIntrNames.count(it->name.name)) {
          main.intrinsics.push_back(*it);
          mainIntrNames.insert(it->name.name);
          it = lib.intrinsics.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

} // namespace symir
