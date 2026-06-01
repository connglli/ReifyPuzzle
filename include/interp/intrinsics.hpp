#pragma once

// [v0.2.2] Interpreter-side intrinsic dispatch — see src/interp/intrinsics.cpp.
//
// This header exists only to make the cross-reference from interpreter.hpp
// visible. The actual implementation is in src/interp/intrinsics.cpp, which
// defines Interpreter::callIntrinsic as a member function.
//
// To track all supported intrinsics in one place, open:
//   src/interp/intrinsics.cpp      (interpreter concrete evaluation)
//   src/solver/intrinsics.cpp      (SMT lowering)
//   src/backend/intrinsics_c.cpp   (C code-gen helper)
//   src/backend/intrinsics_wasm.cpp (WASM code-gen helper)
