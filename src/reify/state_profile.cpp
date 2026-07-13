#include "reify/state_profile.hpp"

#include <algorithm>
#include <ostream>
#include <sstream>

#include "interp/interpreter.hpp"

namespace refractir::reify {

  StateValue toStateValue(const RuntimeValue &rv) {
    StateValue sv;
    switch (rv.kind) {
      case RuntimeValue::Kind::Int:
        sv.kind = StateValue::Kind::Int;
        sv.intVal = rv.intVal;
        sv.bits = rv.bits;
        break;
      case RuntimeValue::Kind::Float:
        sv.kind = StateValue::Kind::Float;
        sv.floatVal = rv.floatVal;
        sv.bits = rv.bits;
        break;
      case RuntimeValue::Kind::Array:
        sv.kind = StateValue::Kind::Array;
        sv.elems.reserve(rv.arrayVal.size());
        for (const auto &e: rv.arrayVal)
          sv.elems.push_back(toStateValue(e));
        break;
      case RuntimeValue::Kind::Vec:
        sv.kind = StateValue::Kind::Vec;
        sv.elems.reserve(rv.arrayVal.size());
        for (const auto &e: rv.arrayVal)
          sv.elems.push_back(toStateValue(e));
        break;
      case RuntimeValue::Kind::Struct:
        sv.kind = StateValue::Kind::Struct;
        // structVal is an unordered_map; sort fields by name for a stable
        // serialization. Consumers that need declaration order re-derive it
        // from the struct decl, so name order here is purely for determinism.
        {
          std::vector<std::string> names;
          names.reserve(rv.structVal.size());
          for (const auto &[k, _]: rv.structVal)
            names.push_back(k);
          std::sort(names.begin(), names.end());
          for (const auto &n: names)
            sv.fields.emplace_back(n, toStateValue(rv.structVal.at(n)));
        }
        break;
      case RuntimeValue::Kind::Ptr:
        sv.kind = StateValue::Kind::Ptr;
        break;
      case RuntimeValue::Kind::Undef:
      default:
        sv.kind = StateValue::Kind::Undef;
        break;
    }
    return sv;
  }

  void attachStateProfile(Interpreter &interp, StateProfile &out, StateGranularity gran) {
    interp.setStateHook(
        [&out](const std::string &blockLabel, int instrIdx, const Store &store) {
          StatePoint pt;
          pt.block = blockLabel;
          pt.instr = instrIdx;
          // Collect names first so the record is deterministically ordered.
          std::vector<std::string> names;
          names.reserve(store.size());
          for (const auto &[name, rv]: store) {
            // Skip uninitialized top-level locals — they carry no state.
            if (rv.kind == RuntimeValue::Kind::Undef)
              continue;
            names.push_back(name);
          }
          std::sort(names.begin(), names.end());
          for (const auto &n: names)
            pt.vars.emplace_back(n, toStateValue(store.at(n)));
          out.trace.push_back(std::move(pt));
        },
        /*perInstr=*/gran == StateGranularity::Ppp
    );
  }

  StateProfile profileProgram(
      const Program &prog, const std::string &func, const std::vector<std::string> &paramArgs,
      StateGranularity granularity
  ) {
    StateProfile profile;
    profile.func = func.empty() || func[0] == '@' ? func : "@" + func;
    profile.granularity = granularity;

    // Suppress the interpreter's "Result:" line — we only want the captured
    // states, and touching the process-global std::cout would be unsafe
    // under concurrency.
    std::ostringstream sink;
    Interpreter interp(prog, sink);
    attachStateProfile(interp, profile, granularity);
    interp.run(profile.func, {}, paramArgs, /*dumpExec=*/false);
    return profile;
  }

  namespace {

    void writeString(std::ostream &os, const std::string &s) {
      os << '"';
      for (char c: s) {
        if (c == '"' || c == '\\')
          os << '\\' << c;
        else
          os << c;
      }
      os << '"';
    }

    // Emit one StateValue as a tagged JSON object. Integers and floats are
    // carried as strings so large i64 values and bit-exact floats survive
    // the JSON boundary without precision loss.
    void writeValue(std::ostream &os, const StateValue &v) {
      switch (v.kind) {
        case StateValue::Kind::Int:
          os << "{\"k\":\"i\",\"w\":" << v.bits << ",\"v\":\"" << v.intVal << "\"}";
          break;
        case StateValue::Kind::Float:
          os << "{\"k\":\"f\",\"w\":" << v.bits << ",\"v\":\"" << formatDouble(v.floatVal) << "\"}";
          break;
        case StateValue::Kind::Array:
        case StateValue::Kind::Vec: {
          os << "{\"k\":\"" << (v.kind == StateValue::Kind::Array ? "arr" : "vec") << "\",\"e\":[";
          for (std::size_t i = 0; i < v.elems.size(); ++i) {
            if (i)
              os << ',';
            writeValue(os, v.elems[i]);
          }
          os << "]}";
          break;
        }
        case StateValue::Kind::Struct: {
          os << "{\"k\":\"st\",\"f\":{";
          for (std::size_t i = 0; i < v.fields.size(); ++i) {
            if (i)
              os << ',';
            writeString(os, v.fields[i].first);
            os << ':';
            writeValue(os, v.fields[i].second);
          }
          os << "}}";
          break;
        }
        case StateValue::Kind::Ptr:
          os << "{\"k\":\"ptr\"}";
          break;
        case StateValue::Kind::Undef:
        default:
          os << "{\"k\":\"undef\"}";
          break;
      }
    }

  } // namespace

  void writeStateProfileJson(std::ostream &os, const StateProfile &profile) {
    os << "{\n  \"func\": ";
    writeString(os, profile.func);
    os << ",\n  \"granularity\": \""
       << (profile.granularity == StateGranularity::Ppp ? "ppp" : "pbb") << "\",\n";
    os << "  \"trace\": [\n";
    for (std::size_t i = 0; i < profile.trace.size(); ++i) {
      const StatePoint &pt = profile.trace[i];
      os << "    {\"block\": ";
      writeString(os, pt.block);
      os << ", \"instr\": " << pt.instr << ", \"vars\": {";
      for (std::size_t j = 0; j < pt.vars.size(); ++j) {
        if (j)
          os << ", ";
        writeString(os, pt.vars[j].first);
        os << ": ";
        writeValue(os, pt.vars[j].second);
      }
      os << "}}";
      if (i + 1 < profile.trace.size())
        os << ',';
      os << '\n';
    }
    os << "  ]\n}\n";
  }

} // namespace refractir::reify
