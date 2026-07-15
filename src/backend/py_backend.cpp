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


def _srem(a, b, n):
    if b == 0:
        _trap("remainder by zero")
    if a == -(1 << (n - 1)) and b == -1:
        _trap("signed remainder overflow (INT_MIN % -1)")
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
    q = a / b
    if n == 32:
        q = _f32(q)
    if not math.isfinite(q):
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


class _Ptr:
    # A provenance-tracked pointer into a flat leaf-slot list: `off` is
    # the current leaf offset, `stride` the pointee's leaf count, and
    # [lo, hi) the extent of the innermost enclosing object (hi itself
    # is the legal one-past-end position).
    __slots__ = ("buf", "off", "stride", "lo", "hi")

    def __init__(self, buf, off, stride, lo, hi):
        self.buf = buf
        self.off = off
        self.stride = stride
        self.lo = lo
        self.hi = hi


_NULL = _Ptr(None, 0, 0, 0, 0)
_UNDEF = ["undef"]  # unique identity sentinel


def _rd(buf, off):
    v = buf[off]
    if v is _UNDEF:
        _trap("read of undef value")
    return v


def _idx(i, n):
    if i < 0 or i >= n:
        _trap("array index out of bounds")
    return i


def _vrd(buf, off, n):
    return [_rd(buf, off + k) for k in range(n)]


def _padd(p, n):
    if p.buf is None:
        _trap("pointer arithmetic on null")
    off = p.off + n * p.stride
    if off < p.lo or off > p.hi:
        _trap("pointer arithmetic out of object bounds")
    return _Ptr(p.buf, off, p.stride, p.lo, p.hi)


def _pdiff(p, q):
    if p.buf is None or q.buf is None or p.buf is not q.buf:
        _trap("cross-object pointer subtraction")
    return (p.off - q.off) // p.stride


def _peq(p, q):
    return p.buf is q.buf and p.off == q.off


def _prel(p, q):
    if p.buf is None or q.buf is None or p.buf is not q.buf:
        _trap("relational compare of cross-object pointers")
    return p.off - q.off


def _load(p):
    if p.buf is None:
        _trap("null pointer dereference")
    if p.off < p.lo or p.off + p.stride > p.hi:
        _trap("pointer dereference out of bounds")
    return _rd(p.buf, p.off)


def _store(p, v):
    if p.buf is None:
        _trap("null pointer store")
    if p.off < p.lo or p.off + p.stride > p.hi:
        _trap("pointer store out of bounds")
    p.buf[p.off] = v


def _pidx(p, i, n, estride):
    if p.buf is None:
        _trap("ptrindex on null pointer")
    if p.off >= p.hi:
        _trap("ptrindex on one-past-end pointer")
    if i < 0 or i > n:
        _trap("ptrindex index out of range")
    return _Ptr(p.buf, p.off + i * estride, estride, p.off, p.off + n * estride)


def _pfield(p, foff, flen, slen):
    # Provenance of a field pointer is the whole containing struct
    # (SPEC 7.5 rule 15): arithmetic may roam across sibling fields.
    if p.buf is None:
        _trap("ptrfield on null pointer")
    if p.off >= p.hi:
        _trap("ptrfield on one-past-end pointer")
    return _Ptr(p.buf, p.off + foff, flen, p.off, p.off + slen)
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
    structFields_.clear();
    for (const auto &s: prog.structs) {
      auto &fields = structFields_[s.name.name];
      for (const auto &f: s.fields)
        fields.emplace_back(f.name, f.type);
    }
    out_ << "# Generated by symirc --target python\n" << kPreamble;
    emitIntrinsicHelpers(prog);
    // External declarations: a link-form decl resolved via -I carries
    // its body, which emits like a normal function (the python module
    // is self-contained — no separate link step). A contract-form decl
    // has no body by design; it emits a stub that traps if reached at
    // runtime (the C analogue is an unresolved extern at link time).
    for (const auto &d: prog.extDecls) {
      if (d.resolvedBody) {
        out_ << "\n\n";
        emitFunction(*d.resolvedBody);
      } else {
        out_ << "\n\ndef " << mangleFun(d.name.name) << "(";
        for (std::size_t i = 0; i < d.params.size(); ++i)
          out_ << (i ? ", " : "") << "_p" << i;
        out_ << "):\n    _trap(\"call to external declaration " << d.name.name << "\")\n";
      }
    }
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

    collectBoxedRoots(f);

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
    const bool boxed = boxedRoots_.count(l.name.name) > 0;
    const bool aggregate = l.type && (std::holds_alternative<ArrayType>(l.type->v) ||
                                      std::holds_alternative<StructType>(l.type->v) ||
                                      std::holds_alternative<VecType>(l.type->v));
    if (aggregate) {
      if (!l.init || l.init->kind == InitVal::Kind::Undef) {
        line(name + " = [_UNDEF] * " + std::to_string(leafCount(l.type)));
      } else if (std::holds_alternative<VecType>(l.type->v) &&
                 (l.init->kind == InitVal::Kind::Local || l.init->kind == InitVal::Kind::Sym)) {
        // Whole-vector copy init (vectors are value types).
        std::string src = l.init->kind == InitVal::Kind::Local
                              ? pyLocal(std::get<LocalId>(l.init->value).name)
                              : symCall(std::get<SymId>(l.init->value).name);
        line(name + " = list(" + src + ")");
      } else {
        line(name + " = " + flattenInit(*l.init, l.type));
      }
      return;
    }
    if (boxed) {
      // Address-taken scalar: a one-slot box.
      if (!l.init || l.init->kind == InitVal::Kind::Undef)
        line(name + " = [_UNDEF]");
      else
        line(name + " = [" + scalarInit(*l.init, l.type) + "]");
      return;
    }
    if (!l.init || l.init->kind == InitVal::Kind::Undef) {
      // Definite-init guarantees no read before the first assignment.
      return;
    }
    line(name + " = " + scalarInit(*l.init, l.type));
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
              emitAssign(arg);
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
              emitStore(arg);
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
          } else if constexpr (std::is_same_v<T, ControlTree::DoWhile>) {
            // Python has no do-while; re-expand to the exact
            // pre-peephole form (`while True:` + tail `if c: break`).
            line("while True:");
            suite([&] {
              if (node.body)
                emitNode(tree, *node.body, f, cfg);
              line("if " + condText(f, node.latch, !node.negate) + ":");
              suite([&] { line("break"); });
            });
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
            if (ret->value) {
              F32Guard ctx(f32Ctx_, !containsF64(f.retType));
              line("return " + exprStr(*ret->value));
            } else {
              line("return");
            }
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
