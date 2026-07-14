#include "backend/py_backend.hpp"
#include <cassert>
#include <stdexcept>
#include <unordered_set>
#include "analysis/dominators.hpp"
#include "analysis/loop_info.hpp"
#include "analysis/reducibility.hpp"
#include "analysis/structured_lowering.hpp"
#include "py_internal.hpp"

namespace refractir {

  namespace {

    // Semantics helpers emitted once per module. Python ints are
    // unbounded and its // and % floor, so every iN operation with UB
    // or wrapping rules goes through a checked helper; f32 arithmetic
    // round-trips through struct to round each operation to single
    // precision (RNE per IEEE 754).
    const char *kPreamble = R"PY(import math
import struct


class RefractIRTrap(Exception):
    pass


def _trap(msg):
    raise RefractIRTrap(msg)


def _ichk(v, n):
    if v < -(1 << (n - 1)) or v >= 1 << (n - 1):
        _trap("signed overflow (i%d)" % n)
    return v


def _iadd(a, b, n):
    return _ichk(a + b, n)


def _isub(a, b, n):
    return _ichk(a - b, n)


def _imul(a, b, n):
    return _ichk(a * b, n)


def _sdiv(a, b, n):
    if b == 0:
        _trap("division by zero")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return _ichk(q, n)


def _srem(a, b):
    if b == 0:
        _trap("remainder by zero")
    r = abs(a) % abs(b)
    return -r if a < 0 else r


def _shl(a, k, n):
    if k < 0 or k >= n:
        _trap("shift amount out of range")
    if a < 0:
        _trap("left shift of negative value")
    return _ichk(a << k, n)


def _ashr(a, k, n):
    if k < 0 or k >= n:
        _trap("shift amount out of range")
    return a >> k


def _lshr(a, k, n):
    if k < 0 or k >= n:
        _trap("shift amount out of range")
    r = (a & ((1 << n) - 1)) >> k
    return r - (1 << n) if r >= 1 << (n - 1) else r


def _cast_int(v, m):
    v &= (1 << m) - 1
    return v - (1 << m) if v >= 1 << (m - 1) else v


def _fin(x):
    if not math.isfinite(x):
        _trap("non-finite float result")
    return x


def _f32(x):
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        _trap("f32 overflow")


def _fadd(a, b, n):
    r = a + b
    if n == 32:
        r = _f32(r)
    return _fin(r)


def _fsub(a, b, n):
    r = a - b
    if n == 32:
        r = _f32(r)
    return _fin(r)


def _fmul(a, b, n):
    r = a * b
    if n == 32:
        r = _f32(r)
    return _fin(r)


def _fdiv(a, b, n):
    if b == 0.0:
        _trap("float division by zero")
    r = a / b
    if n == 32:
        r = _f32(r)
    return _fin(r)


def _ffmod(a, b, n):
    if b == 0.0:
        _trap("float remainder by zero")
    if not math.isfinite(a / b):
        _trap("float remainder intermediate overflow")
    r = math.fmod(a, b)
    if n == 32:
        r = _f32(r)
    return _fin(r)


def _f2i(x, n):
    if not math.isfinite(x):
        _trap("float-to-int of non-finite value")
    v = int(x)
    if v < -(1 << (n - 1)) or v >= 1 << (n - 1):
        _trap("float-to-int out of range")
    return v
)PY";

    const std::unordered_set<std::string> &pyKeywords() {
      static const std::unordered_set<std::string> kw = {
          "False",   "None",     "True",     "and",    "as",   "assert", "async",  "await",
          "break",   "class",    "continue", "def",    "del",  "elif",   "else",   "except",
          "finally", "for",      "from",     "global", "if",   "import", "in",     "is",
          "lambda",  "nonlocal", "not",      "or",     "pass", "raise",  "return", "try",
          "while",   "with",     "yield",    "match",  "case",
      };
      return kw;
    }

    std::string escapePyString(const std::string &s) {
      std::string out;
      for (char c: s) {
        if (c == '\\' || c == '"')
          out += '\\';
        out += c;
      }
      return out;
    }

  } // namespace

  std::string PyBackend::stripSigil(const std::string &name) {
    if (name.empty())
      return name;
    std::size_t start = 0;
    if (name[0] == '@' || name[0] == '%' || name[0] == '^') {
      start = 1;
      if (name.size() > 1 && name[1] == '?')
        start = 2;
    }
    return name.substr(start);
  }

  std::string PyBackend::mangleFun(const std::string &name) const {
    if (noMainMangle_ && name == "@main")
      return "main";
    return "refractir_" + stripSigil(name);
  }

  std::string PyBackend::symCall(const std::string &symName) const {
    // Provider convention shared with the C backend's extern symbols:
    // <fun>__<sym>, sigils stripped. The embedding defines these in
    // the module globals before calling the entry function.
    return stripSigil(curFuncName_) + "__" + stripSigil(symName) + "()";
  }

  std::string PyBackend::pyLocal(const std::string &sigiled) {
    auto it = pyNames_.find(sigiled);
    assert(it != pyNames_.end() && "local not in the per-function name map");
    return it->second;
  }

  void PyBackend::buildNameMap(const FunDecl &f, const ControlTree &tree) {
    pyNames_.clear();
    takenNames_.clear();
    // Guard flags own their names verbatim; user locals starting with
    // '_' are renamed below, so the namespaces cannot collide.
    for (const auto &flag: tree.flagNames)
      takenNames_.insert(flag);
    auto add = [&](const std::string &sigiled) {
      std::string base = stripSigil(sigiled);
      if (base.empty() || base[0] == '_' || pyKeywords().count(base))
        base = "v_" + base;
      while (takenNames_.count(base))
        base += "_";
      takenNames_.insert(base);
      pyNames_.emplace(sigiled, base);
    };
    for (const auto &p: f.params)
      add(p.name.name);
    for (const auto &l: f.lets)
      add(l.name.name);
  }

  void PyBackend::line(const std::string &s) {
    for (int i = 0; i < indent_; ++i)
      out_ << "    ";
    out_ << s << "\n";
    ++stmtCount_;
  }

  void PyBackend::comment(const std::string &s) {
    for (int i = 0; i < indent_; ++i)
      out_ << "    ";
    out_ << "# " << s << "\n";
    // Comments don't count as suite statements ("pass" still needed).
  }

  void PyBackend::emit(const Program &prog) {
    prog_ = &prog;
    if (!prog.extDecls.empty())
      throw std::runtime_error("python target: external decls not yet supported");
    out_ << "# Generated by symirc --target python\n" << kPreamble;
    for (const auto &f: prog.funs) {
      out_ << "\n\n";
      emitFunction(f);
    }
  }

  void PyBackend::emitFunction(const FunDecl &f) {
    curFuncName_ = f.name.name;
    varTypes_.clear();
    for (const auto &p: f.params) {
      requireSupportedType(p.type, "parameter");
      varTypes_[p.name.name] = p.type;
    }
    for (const auto &s: f.syms) {
      requireSupportedType(s.type, "symbol");
      varTypes_[s.name.name] = s.type;
    }
    for (const auto &l: f.lets) {
      requireSupportedType(l.type, "local");
      varTypes_[l.name.name] = l.type;
    }

    // The pipeline (ReducibilityCheck included for this target) has
    // validated the function, so the structuring below is total.
    DiagBag diags;
    CFG cfg = CFG::build(f, diags);
    DomTree dt = DomTree::build(cfg);
    LoopInfo li = LoopInfo::build(cfg, dt);
    ControlTree tree = StructuredLowering::run(Structurizer::build(f, cfg, dt, li), f, cfg);

    buildNameMap(f, tree);

    std::string sig = "def " + mangleFun(f.name.name) + "(";
    for (std::size_t i = 0; i < f.params.size(); ++i) {
      if (i)
        sig += ", ";
      sig += pyLocal(f.params[i].name.name);
    }
    sig += "):";
    line(sig);
    stmtCount_ = 0;
    suite([&] {
      for (const auto &flag: tree.flagNames)
        line(flag + " = False");
      for (const auto &l: f.lets)
        emitLet(l);
      if (tree.root)
        emitNode(tree, *tree.root, f, cfg);
    });
    stmtCount_ = 0;
    indent_ = 0;
  }

  void PyBackend::emitLet(const LetDecl &l) {
    const std::string name = pyLocal(l.name.name);
    if (!l.init) {
      // No initializer: definite-init guarantees no read before the
      // first assignment, so no declaration is needed.
      return;
    }
    const bool toF32 = floatWidth(l.type) == 32;
    auto wrap = [&](const std::string &s) { return toF32 ? "_f32(" + s + ")" : s; };
    switch (l.init->kind) {
      case InitVal::Kind::Int:
        line(name + " = " + std::to_string(std::get<IntLit>(l.init->value).value));
        break;
      case InitVal::Kind::Float:
        line(name + " = " + wrap(formatFloatLit(std::get<FloatLit>(l.init->value).value)));
        break;
      case InitVal::Kind::Sym:
        line(name + " = " + symCall(std::get<SymId>(l.init->value).name));
        break;
      case InitVal::Kind::Local:
        line(name + " = " + pyLocal(std::get<LocalId>(l.init->value).name));
        break;
      case InitVal::Kind::Undef:
        break; // reading undef is UB; definite-init rejects it statically
      case InitVal::Kind::Atom:
        line(name + " = " + wrap(atomStr(*std::get<AtomPtr>(l.init->value))));
        break;
      case InitVal::Kind::Null:
      case InitVal::Kind::Aggregate:
        throw std::runtime_error("python target: pointer/aggregate local not yet supported");
    }
  }

  std::string PyBackend::condText(const FunDecl &f, std::size_t block, bool negate) {
    const auto *br = std::get_if<BrTerm>(&f.blocks[block].term);
    assert(br && br->isConditional && br->cond && "structured condition on a non-branch block");
    std::string s = condStr(*br->cond);
    return negate ? "not (" + s + ")" : s;
  }

  void PyBackend::emitBlockInstrs(const Block &b) {
    if (!b.instrs.empty())
      comment(b.label.name);
    for (const auto &ins: b.instrs) {
      std::visit(
          [this](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, AssignInstr>) {
              line(lvalueStr(arg.lhs) + " = " + exprStr(arg.rhs));
            } else if constexpr (std::is_same_v<T, AssumeInstr>) {
              comment("assume " + condStr(arg.cond));
            } else if constexpr (std::is_same_v<T, RequireInstr>) {
              if (!noRequire_) {
                std::string msg = "require violation";
                if (arg.message)
                  msg += ": " + escapePyString(*arg.message);
                line("if not (" + condStr(arg.cond) + "): _trap(\"" + msg + "\")");
              }
            } else {
              static_assert(std::is_same_v<T, StoreInstr>);
              throw std::runtime_error("python target: pointers not yet supported");
            }
          },
          ins
      );
    }
  }

  void PyBackend::emitNode(
      const ControlTree &tree, const ControlTree::Node &n, const FunDecl &f, const CFG &cfg
  ) {
    std::visit(
        [&](const auto &node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T, ControlTree::Seq>) {
            for (const auto &item: node.items)
              if (item)
                emitNode(tree, *item, f, cfg);
          } else if constexpr (std::is_same_v<T, ControlTree::BlockStmts>) {
            emitBlockInstrs(f.blocks[node.block]);
          } else if constexpr (std::is_same_v<T, ControlTree::If>) {
            line("if " + condText(f, node.block, node.negate) + ":");
            suite([&] {
              if (node.thenBr)
                emitNode(tree, *node.thenBr, f, cfg);
            });
            if (node.elseBr) {
              line("else:");
              suite([&] { emitNode(tree, *node.elseBr, f, cfg); });
            }
          } else if constexpr (std::is_same_v<T, ControlTree::Loop>) {
            line("while True:");
            suite([&] { emitNode(tree, *node.body, f, cfg); });
          } else if constexpr (std::is_same_v<T, ControlTree::CondLoop>) {
            line("while " + condText(f, node.header, node.negate) + ":");
            suite([&] { emitNode(tree, *node.body, f, cfg); });
          } else if constexpr (std::is_same_v<T, ControlTree::Break>) {
            assert(node.levels == 1 && "multi-level break survived lowering");
            line("break");
          } else if constexpr (std::is_same_v<T, ControlTree::Continue>) {
            assert(node.levels == 0 && "multi-level continue survived lowering");
            line("continue");
          } else if constexpr (std::is_same_v<T, ControlTree::SetFlag>) {
            line(tree.flagNames[node.flag] + " = True");
          } else if constexpr (std::is_same_v<T, ControlTree::FlagBreak>) {
            const std::string &flag = tree.flagNames[node.flag];
            if (node.isFinal) {
              line("if " + flag + ":");
              suite([&] {
                line(flag + " = False");
                line("break");
              });
            } else {
              line("if " + flag + ": break");
            }
          } else if constexpr (std::is_same_v<T, ControlTree::FlagContinue>) {
            const std::string &flag = tree.flagNames[node.flag];
            line("if " + flag + ":");
            suite([&] {
              line(flag + " = False");
              line("continue");
            });
          } else if constexpr (std::is_same_v<T, ControlTree::Guarded>) {
            std::string cond;
            for (int fl: node.flags) {
              if (!cond.empty())
                cond += " and ";
              cond += "not " + tree.flagNames[fl];
            }
            line("if " + cond + ":");
            suite([&] { emitNode(tree, *node.body, f, cfg); });
          } else if constexpr (std::is_same_v<T, ControlTree::ResetFlag>) {
            line(tree.flagNames[node.flag] + " = False");
          } else if constexpr (std::is_same_v<T, ControlTree::Return>) {
            const auto *ret = std::get_if<RetTerm>(&f.blocks[node.block].term);
            assert(ret && "Return node on a block without ret");
            if (ret->value)
              line("return " + exprStr(*ret->value));
            else
              line("return");
          } else if constexpr (std::is_same_v<T, ControlTree::Trap>) {
            line("_trap(\"unreachable executed\")");
          } else {
            // FallThrough / JumpJoin are eliminated by StructuredLowering.
            assert(false && "unlowered transfer node reached the python emitter");
          }
        },
        n.v
    );
  }

} // namespace refractir
