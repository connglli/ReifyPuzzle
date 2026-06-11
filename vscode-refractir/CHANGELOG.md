# Changelog
All notable changes to the "refractir-syntax" extension will be documented in this file.

## [0.2.2] - 2026-06-01

- Add `pre` and `post` contract-clause keywords (new in spec v0.2.2); scoped as
  `keyword.other.contract` so themes can colour them distinctly from declaration
  or control-flow keywords.
- Add `cmp` to expression keywords alongside `select` and `as` (was missing from
  all prior releases despite being in the grammar since v0.2.1).
- Promote `call` from "control-flow terminator" comment to a first-class entry:
  spec v0.2.2 makes it an expression `Atom` and a valid `let`-initializer, not
  only a statement-position keyword.
- Fix `addr`, `load`, `store`, `ptrindex`, `ptrfield` scope from
  `keyword.control` to `keyword.other.memory` — none of these alter control flow.
- Remove erroneous `@?name` and `%?name` identifier patterns. In the Oniguruma
  engine used by VS Code, `@\\?` matches the two-character literal sequence `@?`,
  which is not a valid refractir sigil; the patterns never matched real source text
  and shadowed the correct `@name` / `%name` rules.

## [0.2.0] - 2026-05-18

- Add v0.2.0 pointer keywords: `addr`, `load`, `store` highlighted as memory operators.
- Add `ptr` type keyword (same color family as `i32`, `f64`).
- Add `null` constant (same scope as `undef` — `constant.language`).
- Move `undef` from `keyword.other` to `constant.language` for correct theming.
- Fix `language-configuration.json`: bracket pairs were malformed (single strings
  instead of two-element arrays) and had missing commas — this prevented the
  extension from activating entirely.

## [0.1.0] - 2026-01-31

- Initial release of RefractIR syntax highlighting.
- Basic support for keywords, types, identifiers, and literals.
