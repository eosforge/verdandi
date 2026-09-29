[English](#english) | [简体中文](#chinese)

<a id="english"></a>

# Multilingual Coding and File Organization Standard

> Standard version: 1.0.0. Git revisions and file digests identify the content. This document can be reused across projects with the [AI Development and Long-Term Maintenance Standard](development.md). Having the documents does not mean that a project has adopted or passed their requirements.

This standard defines how code is expressed, modules are divided, and files are owned. Development admission, authorization, verification strength, native evidence, gates, and release follow `development.md`; this document does not establish a second workflow or acceptance threshold. Its purpose is consistent, direct, verifiable implementation during long-term AI maintenance, while respecting each language's semantics and ecosystem.

## 1. Scope and Rule Ownership

The meanings of “must,” “should,” and “may,” and the authority boundaries, follow the development standard. This document covers handwritten production code, tests, build tools, migration scripts, generation templates, and executable examples. Manage generated and third-party files by provenance; do not rewrite them directly for stylistic consistency.

| Source | Sole responsibility |
| --- | --- |
| Development and maintenance standard | Workflow, design admission, contract verification, evidence, and gates |
| General sections and Section 8 of this document | Cross-language principles, language-specific conventions, modules, file organization, and enforcement |
| Committed tool configuration | Encoding these formatting and diagnostic requirements as tool parameters, without a duplicate prose standard |
| Project design and configuration | Actual components, dependency boundaries, directory mappings, product interfaces, tool entry points and parameters, and explicitly registered coding differences |
| Project authorization rules | Which actions are authorized and which require a user decision |

Language rules are centralized in Section 8. Register confirmed project-specific conventions under Section 1.1. Project configuration records supported versions, tool parameters, and compatibility differences; do not create parallel language standards. Resolve missing conventions that affect public interfaces or compatibility before proceeding. For private local implementation, follow effective rules and nearby code; neighboring code does not itself authorize an override.

Each fact has one authoritative source. AI entry points link to rules and project configuration; they do not duplicate the rules or accumulate session-specific copies. When reusing this standard, update companion links and leave project paths and commands in project documentation.

<a id="overrides"></a>

### 1.1 Project Overrides

This document is a reusable coding baseline. A project may register differences in a fixed section of its existing maintenance guide, such as `CONTRIBUTING.md`. Project `AGENTS.md` must link to that section and require reading it at task start. Do not copy or fork the entire standard. Add a project rules file only when no suitable existing document exists. Subdirectory rules must be discoverable from the project entry point, not silently introduced through hidden files or transient prompts.

For a particular coding matter, precedence is: current explicit user instruction > confirmed project difference applicable to the path > language-specific rule here > general rule here. A subdirectory difference must identify the parent provision and scope it replaces. Proximity, recency, or stronger wording alone grants no precedence. Explain and resolve overlapping differences with no explicit relationship rather than choosing the less restrictive one.

Record at least the following; unaffected provisions continue to inherit the baseline:

| Field | Requirement |
| --- | --- |
| Provision | Standard version and section/anchor; identify the requirement being replaced |
| Scope | Repository-relative paths, language, and production/test/tool file categories |
| Replacement | A complete executable rule, not merely “follow project conventions” |
| Reason and authority | Compatibility, ecosystem, or product reason, plus user confirmation or an effective existing project agreement |
| Enforcement | Formatter/check configuration; explicitly state when automatic enforcement is unavailable |

For example, a maintenance guide may register a confirmed formatting difference:

```markdown
## Coding Differences

- Provision: coding.md 1.0.0, Section 8, default line width for languages other than C++.
- Scope: handwritten Python files under tools/**/*.py, including tests.
- Replacement: Black line-length is 88 instead of the general 160-column target.
- Reason and authority: the project has confirmed its existing Python tool configuration; link the actual confirmation when registering this difference.
- Enforcement: [tool.black] in pyproject.toml.
```

This example does not establish an 88-column rule for the current project. Tool configuration implements confirmed differences; its values do not create authorization or an override. Resolve and synchronize discrepancies between prose and tools. Project rules may adjust naming, comment language/density, formatting, and directory mappings. They may not weaken correctness, compatibility contracts, evidence, or acceptance requirements, or expand testing, download, commit, or release permission. Such changes still require the applicable decision and exception process.

When using structured configuration, reuse legal existing fields and entry points. Do not add unsupported fields to `development.json`. On standard upgrades, verify that differences still correspond to the intended provision; do not silently apply an old override to a changed rule.

## 2. Naming and Scope

- Minimal naming applies to every language: types, functions, variables, fields, modules, and owned files. Prefer one precise, complete word, and use the same name for the same concept. Do not shorten words or invent initialisms merely for brevity, or replace precision with vague short names.
- Abbreviations are limited to widely established terms such as ID, UUID, TCP, TLS, RPC, and TTL. Local preferences are not industry consensus. Adapt capitalization to the language. Comments supplement contracts and constraints; they do not repair incorrect or ambiguous names.
- Let modules, types, and lexical scope supply context instead of repeating their names. Do not add empty classes or nesting merely to shorten identifiers.
- First choose a word that accurately expresses the responsibility, then adapt capitalization to the language and symbol role, including uppercase constants where specified. Use multiple words only when a single word and existing scope cannot remove real ambiguity or distinguish necessary units or semantics. Apply the language's camelCase or underscore conventions then; support for compound names is not a reason to use them.
- Preserve prefixes, suffixes, and fixed names required by a language, framework, or external interface. Do not change external interfaces or serialized fields for internal style, overturn established names by personal preference, or perform unrelated mass renaming.
- Actions, queries, and conversions must reflect observable behavior. If a read interface performs I/O, mutates state, consumes an iterator, or transfers ownership, state this in its contract rather than hiding it behind a seemingly pure name.
- Distinguish easily confused units, identities, counts, offsets, and clock domains in types or names. Do not use comments to excuse avoidable confusion between same-typed arguments.
- Place definitions in the narrowest valid scope that owns their responsibility, private by default. Expose types and interfaces only for real usage contracts, never merely to simplify tests.
- Broad names such as `utils`, `common`, and `manager` do not replace responsibility boundaries. Existing names with a clear responsibility may remain. Find an owner for unassigned code instead of dumping it into a shared directory.

## 3. Implementation and Complexity

### 3.1 Functions, State, and Data

Organize functions around one explainable operation or invariant. Keep validation, preparation, commit, notification, and cleanup stages clear; place variables near their use and lifetime. Direct early returns and local helpers are acceptable. Do not scatter one commit boundary across layers of jumps to meet an arbitrary line limit.

Prefer types, encapsulation, explicit results, and native resource mechanisms to express constraints. Distinguish absence, empty collections, zero, errors, and unknown outcomes; do not hide them behind a common sentinel or successful default. When mutually exclusive flags admit invalid combinations, prefer a representation that constrains the state space.

Shared mutation, borrowing, implicit conversions, and lazy-evaluation effects must be identifiable. Do not depend on unspecified traversal order, evaluation order, or environmental defaults. Establish concurrency, atomicity, time, numeric, and error contracts under development Sections 3–4; do not introduce weaker equivalents here.

### 3.2 Abstraction and Efficiency

- Prefer language and standard-library facilities supported by the actual toolchain. Choose syntax for expression, correctness, and cost, not its age.
- When established semantics, correctness, resources, and performance are preserved, prefer the language's cross-platform standard library. Use platform-specific APIs or alternatives only where the standard library cannot meet the need, and isolate differences at explicit adapters. Standard-library membership alone proves neither semantic nor performance equivalence.
- New abstractions must serve an existing invariant, explicit boundary, or actual consumer. Interfaces for testability, platform adaptation, or security isolation are valid; frameworks, plugins, or inheritance layers built only for possible future reuse are not.
- Similar code is suitable for merging only when semantics, failure guarantees, lifecycle, and evolution agree. Do not unify different domain state machines merely because they look alike, or duplicate the same business rule without a reason.
- Simplification reduces repeated knowledge and understanding cost. It must not remove necessary checks, comments, error context, or tests. Do not save lines through dense expressions, nested ternaries, hidden effects, or excessive metaprogramming.
- Ordinary implementation must consider complexity, allocations, copying, locality, and lock scope. Added optimization complexity requires evidence. Distinguish static analysis from measured gains; fewer lines, fewer objects, or theoretical complexity are not performance measurements.

## 4. Module and Dependency Boundaries

Divide modules by business responsibility, data ownership, and reasons to change. Keep external interfaces stable and small; restrict internal access through the language or build system. Tests may use controlled internal test mechanisms without turning internal symbols into maintained public APIs.

Separate core rules and I/O adapters under development Section 3.2. The core uses explicit data, events, and boundary interfaces; startup composes implementations. Do not require an interface for every type or obscure dependencies with a dependency-injection container or global service locator.

Register allowed component dependency directions and public entry points. Do not bypass them through private paths, runtime reflection, or test shortcuts. Remove accidental cycles. If code truly depends on itself mutually and cannot be separated, define a jointly maintained unit; renaming directories does not establish layering.

Public protocols and cross-language structures have one semantic source. Bindings retain native language expression. Share field meanings, units, errors, and compatibility commitments rather than forcing identical internal type shapes. Give generators, shared vectors, and handwritten adapters explicit owners.

Contain third-party APIs, exceptions, lifetimes, and version differences at appropriate boundaries. Adapters must provide real isolation rather than mechanically wrapping every external function. If the product deliberately exposes an external protocol, document that dependency and compatibility responsibility.

## 5. Files and Paths

Organize each file around a cohesive responsibility, keeping private helpers near callers. Split for visibility, ownership, dependencies, or independent changes, not arbitrary line limits. Do not collect unrelated features in one large file. Respect language-required type/file correspondence, module entry points, and export structures.

Use stable responsibility names, not `new`, `final`, `v2-final`, dates, or authors to distinguish current implementations. Preserve names with runtime meaning, such as protocol versions, migration numbers, or historical compatibility fixtures; they are not temporary clutter.

Paths and configuration must distinguish public entry points, internals, test support, and generated output. Do not impose one directory tree on all languages. Unit tests may live beside implementations when conventional; system and cross-component contract tests may be centralized. Both must participate in actual discovery.

New owned paths must be valid on target platforms. Avoid case-only distinctions, reserved names, and unnecessary special characters. Verify Git tracking during case-sensitive/insensitive renames. Do not unnecessarily change public paths, or alter meaningful names of external resources.

Versioned configuration fixes encoding, line endings, and executable permissions; new text defaults to UTF-8. Tools resolve paths from an explicit working directory or their own location, not personal directories, implicit search paths, or accidental cwd. Do not make local absolute paths prerequisites for portable source.

## 6. Repository Contents and Directory Responsibilities

The following are responsibility boundaries; names are illustrative mappings. Create only needed directories and follow the language/build ecosystem. Do not create empty trees for completeness or automatically migrate an existing repository on adoption.

| Content | Ownership and boundary |
| --- | --- |
| Production code | Its component and native source layout, such as `src`, `internal`, or `lib`; idiomatic root packages are allowed, with clear entry-point responsibilities |
| Public API | Explicit exports, headers, or packages; distinguish internals from public commitments |
| Tests and support | Beside components or under `tests`; test dependencies must not enter production delivery |
| Fixtures, vectors, counterexamples | Owned by tests or shared contracts; declare provenance and discovery, version protected counterexamples |
| Performance and long-running scenarios | Separate entry points, budgets, and authorization within the test system, optionally under `bench`; ordinary tests must not trigger them accidentally |
| Build, generation, maintenance tools | `tools` or ecosystem-defined locations, with stable inputs, outputs, entry points, and failure semantics; temporary scripts must not become hidden dependencies |
| Protocols, schemas, migrations | One source location per interface/component; preserve migration sequences and compatibility samples by semantics |
| Generated code | Mark its input/generator relationship; tracking depends on consumption and build needs, not a blanket exclusion |
| Examples | Consume real public APIs; label complete examples, snippets, pseudocode, and expected failures; discover runnable examples in verification |
| Configuration and deployment | By environment and purpose; commit shareable configuration and safe templates, reference secrets externally |
| Documentation | One entry point each for navigation, standards, design, usage, and latest validation; version control holds history |
| Third-party source and licenses | Separate or register provenance; retain required licenses and NOTICE files regardless of the root license |
| Build output, caches, temporary data | Declared reproducible output or authorized caches; excluded from source commits by default; release only explicitly listed, evidence-bound artifacts, never an entire output directory |
| Raw execution evidence | Seal in declared local/controlled storage under the development standard; retain failures, and do not treat evidence as disposable merely because it is under build output |

Organize multi-component repositories by independent build, delivery, and ownership boundaries. Root tools coordinate; components declare actual inputs and dependencies. Independently distributable components must list all build/runtime inputs and cannot depend on undeclared parent-repository contents.

Production builds must not rely on undeclared files incidentally present in personal caches. Required tools, generated code, and fixtures belong in provenance/dependency management; being “just a script” does not exempt an input from maintenance or verification.

## 7. Comments, Formatting, and Readability

Comments explain current responsibility, contracts, reasons, and non-obvious constraints. Public interfaces describe arguments, results, errors, ownership, and concurrency; implementation comments explain important state, commit, and cleanup stages. Document relevant units, ranges, defaults/initial state, absence, and special values without inventing guarantees.

Handwritten code comments use clear, standard English grammar and punctuation, preserving identifiers, protocol fields, and citations. This does not require English product interfaces, test inputs, or Markdown. Files, classes, functions, and variables have responsibility/purpose descriptions by default, including internals, tests, and templates. Adapt syntax, placement, and detail to the language; do not reduce coverage to public APIs alone. Do not impose comment ratios or narrate every statement. Register project differences in language or coverage granularity under Section 1.1.

| Object | Baseline |
| --- | --- |
| File | Brief header for handwritten source, tests, and scripts describing its role; tests identify what they verify. Use native file comments or module docstrings, not filename repetition, authors, dates, or change logs |
| Class and other types | Describe responsibility, concept, and important invariants for classes, structs, interfaces, and major types; add ownership/thread constraints where relevant, not merely a rephrased name |
| Enums and variants | Describe the enum's state/category and each item's meaning and conditions; named union variants, fields, and state constants also require individual semantics rather than only a type description |
| Functions and methods | Describe purpose at declaration/definition, with relevant arguments, results, failures, and effects, including private functions. Do not duplicate full contracts; accessors/implementations may explicitly inherit property/interface documentation but must document new semantics |
| Variables and constants | Explain members, globals, constants, and configuration near their definitions, including relevant units, ranges, and special values. Function docs may cover parameters. Locals need purpose descriptions; related simple temporaries, loop variables, structured bindings, or captures may share adjacent block commentary rather than one comment line per object |

Place file descriptions where the language allows, preserving required ordering of shebangs, encoding declarations, build directives, licenses, and module syntax. Merge with module documentation rather than duplicating it. For formats without comments, use schemas or owning documentation. Generated/third-party files follow provenance rules. Adjacent explanations must clearly identify their objects and purposes; vague phrases such as “process data” do not count. Shared commentary must not omit lifecycle, locks, units, or special states.

| Language/format | Comment form and detail |
| --- | --- |
| C / C++ | Consistent declaration documentation, using Doxygen when adopted; adjacent `//` may describe private declarations/variables, while implementation explains ownership, locking, and commit boundaries |
| Go | Adjacent exported-declaration docs begin with the symbol name; maintain package docs centrally and describe file roles/internals in adjacent comments |
| Rust | `///` for public items, `//!` for modules; ordinary or doc comments for private items; explain `unsafe` preconditions |
| Python | Module/class/function docstrings with consistent parameter style; `#` for variables and implementation, without repeating type hints |
| JavaScript / TypeScript | Project JSDoc/TSDoc for public APIs; avoid duplicating readable TypeScript types; explain effects, async work, and lifecycle internally |
| HTML / CSS / templates | Native syntax describing accessibility, compatibility, and complex styling constraints; script blocks follow their language |
| Java / Kotlin / C# | Javadoc, KDoc, or XML documentation for public APIs; ordinary comments for internals/variables, simple accessors may inherit property docs |
| Lua | `--` or existing project documentation format; public modules describe arguments/results/host resource duties, internals according to complexity |
| SQL / Shell / PowerShell | Migration effects, transaction/retry boundaries, preconditions, and external effects; suitable help comments for public commands, not line-by-line command narration |
| Protocol / configuration / template | Supported comments or schema descriptions for units, defaults, absence, and compatibility; never insert comments into formats that forbid them |

Follow existing tools for documentation tags and layout; do not introduce a generator for visual uniformity. Maintain the complete contract only at its authoritative declaration, with reasons and invariants in implementation.

Place enum-item comments next to each declaration using native documentation, preceding, or trailing comments. Even obvious names need brief semantic explanations rather than repetition. Explain protocol values, indices, flags, defaults, unknowns, and reserved values where applicable. Comment/format changes must not change enum values or ordering. Comments must agree with implementation and formal contracts; planned behavior is not delivered capability. Durable TODOs identify a concrete gap and owner. Do not preserve history as commented-out code, session narratives, or author signatures. Language/density changes must retain valid contract information and do not trigger unrelated bulk translation.

Formatting follows committed language configuration and existing formatters. Do not duplicate indentation, braces, wrapping, import sorting, or width settings across documents. Format only authorized handwritten files, not unrelated source, generated output, or dependencies. Explain configuration changes separately; do not disable a rule for one inconvenient line.

If a required tool is unavailable, report the unchecked scope. Do not install it automatically or equate visual inspection with execution. Formatting does not prove behavior. Static checks that compile, execute, or download still require authorization for their actual effects.

## 8. Language-Specific Conventions

All general language conventions live here, not in parallel coding-standard files. Projects register languages, versions, tooling, directories, and explicit Section 1.1 differences. Apply general and language-specific sections together; mixed stacks read every relevant section. Defaults here do not authorize changing public APIs, upgrading toolchains, or rewriting dependencies.

Every language first follows Section 2's preference for one complete word. `camelCase`, `PascalCase`, `snake_case`, and `UPPER_SNAKE_CASE` specify capitalization and how unavoidable multiword names join; they do not require multiple words. A single word may be `entry`, `Entry`, or `ENTRY`. Use `readTimeout`, `ReadTimeout`, or `read_timeout` only when read/write deadlines genuinely need distinction.

| Language/format | Section |
| --- | --- |
| C++ / C | [C++](#cpp) / [C](#c) |
| Go / Rust / Python | [Go](#go) / [Rust](#rust) / [Python](#python) |
| JavaScript / TypeScript / frontend | [JS/TS](#typescript) / [HTML, CSS, components](#web) |
| Java / Kotlin / C# / Lua | [Java/Kotlin](#jvm) / [C#](#csharp) / [Lua](#lua) |
| SQL / Shell / PowerShell | [SQL](#sql) / [Scripts](#shell) |
| Protocol / configuration / templates | [Protocols and configuration](#protocol) |

Unlisted languages follow general rules first. On first adoption, add naming, visibility, modules, errors, resources, and formatting here, then register tools/support boundaries at project level. A finite list does not cover every language. Outside C++, 160 columns is a configurable target, with actual output following existing tool configuration; width must not corrupt literals, generation semantics, or fixed external formats. C++ follows its explicit long-line rules.

<a id="cpp"></a>

### 8.1 C++

These provisions define C++ scope, implementation, comments, long lines, and logical-block spacing. They include handwritten tests and templates. Projects still declare the actual language standard and compatibility floor; listing a feature here does not authorize an upgrade.

#### 8.1.1 Scope

- All handwritten C++ must comply, including production, tests, templates, headers, and implementation files.
- Generated and third-party files are outside stylistic cleanup. Do not rename, comment, or reformat them to apply these rules.
- Do not mechanically apply C++ conventions to other languages or resume frozen component development on this basis.

#### 8.1.2 Naming

- Minimal names, established abbreviations, and conditions for compound names follow Section 2; they are not C++-only rules.
- Use meaningful class scope, such as `Store::Entry`, without repeating ownership or creating empty wrapper classes. Types use `PascalCase`; functions and variables begin lowercase. When multiple words are unavoidable, use `EntryState` and `readTimeout`, not underscore-joined identifiers.
- Private data members have a trailing underscore, such as `timeout_`; parameters and locals do not, such as `timeout`. Do not add meaningless prefixes merely to distinguish a member from a parameter.
- Property accessors use same-name overloads, such as `timeout()` and `timeout(value)`. Provide only required read/write operations, not automatic setters for every field. Name non-property operations by behavior.
- Enum items use `PascalCase`, such as `State::Ready`, without repeating the enum name. Unavoidable compounds use upper-initial camel case. Preserve fixed external protocol/generated symbols.
- Owned constants follow variable naming, such as `limit` or necessary `readLimit`, without `k` prefixes or uppercase solely because they are constant. Do not rewrite macros or fixed external symbols on this basis.
- Prefer complete lowercase namespace words and meaningful nesting, avoiding compounds. If responsibility cannot be split and multiple words are necessary, use lower-initial camel case. Do not manufacture empty nesting for short names.
- Owned headers and implementations use `.hpp` and `.cpp`. Filenames use complete lowercase words; unavoidable compounds use underscores, such as `read_timeout.hpp`. Paths and symbols have separate conventions: an uppercase type name does not require an uppercase filename.

#### 8.1.3 Narrow Scope

- Place definitions in the narrowest scope matching their responsibility. Prefer class-local types, enums, constants, and helpers when only that class uses them.
- Class definitions are private by default and become public only for an actual external contract. Nesting does not imply public visibility; do not expand production APIs for tests.
- Order class access sections `public`, `protected`, `private`, omitting empty sections. Within each, group related types, operations, and data by responsibility instead of mechanically separating symbol categories. Declaration order must not alter appropriate visibility.
- Use `struct` for public data records and `class` for encapsulated invariants, resources, and behavior. Do not decide solely by member count or presence of methods; records may have semantically related helpers.
- For a clearly class-owned operation that needs no instance, prefer a static member over a public namespace function.
- Do not create empty classes merely to collect miscellaneous helpers. Shared concepts across independent components, and interfaces that must be namespace-level, retain their appropriate ownership.
- Prefer anonymous namespaces for translation-unit-private implementation details in `.cpp`; this restricts linkage and visibility, not necessarily symbol-table contents.
- Do not mechanically use anonymous namespaces in headers and create distinct types/state in every translation unit. Prefer private class definitions and arrangements appropriate to header semantics.
- Where supported, use compact nested namespaces such as `namespace astra::detail {}`. Compact syntax does not change ownership or scope.

#### 8.1.4 Performance, Caches, and Layout

- Correctness, lifecycle, and concurrency safety come first. Do not sacrifice invariants, object validity, or synchronization for speed or brevity.
- Aim for very high efficiency: consider hot-path allocation, copying, traversal, indirection, locks, and working sets. Simplification must not introduce hidden copies, repeated scans, or extra allocations.
- Arrange data, hot/cold fields, and ownership for actual access patterns and locality. Consider batches and concurrent access, not just one object's size.
- Do not optimize layout solely for minimum `sizeof`. Alignment, padding, cache-line sharing, and false sharing matter; saving bytes must not introduce unsuitable unaligned access or contention.
- Added performance complexity requires an identified hotspot and benefit rationale. Distinguish static inference from measured results; theoretical gains are not measurements.
- Performance verification still requires current-turn authorization. Benchmarks and optimization work do not bypass project permission rules.

#### 8.1.5 Modern Syntax and Simplicity

- Prefer C++26, C++23, and C++20 features and standard-library capabilities actually supported by the target toolchain.
- Prefer modern syntax when it makes implementation more efficient or simpler without compromising correctness or performance.
- Express ownership, constraints, resource management, and compile-time work directly through the language and library. Avoid duplicate mechanisms and layers with no benefit.
- New syntax is not an achievement by itself. Do not conceal simple logic behind complex templates, macros, or abstractions.
- With equal performance and code size, prefer direct, natural, consistent expression in which important invariants and lifetimes are visible.
- Simplification removes real redundancy and complexity, not necessary validation, comments, or tests.
- Use `auto` when initialization clearly expresses the type, or with iterators/complex templates. Write the type when it carries important meaning not evident from initialization. Apply references and `const` semantically; deduction must not hide copies or borrowing.
- Locals that are not reassigned default to `const`, except when mutation or moving is needed. `const` does not prove deep immutability or thread safety, and must not introduce copies for uniformity.
- Put `const` before the qualified type, such as `const Entry&` and `const auto`. A non-reassignable pointer remains `Entry* const`; preserve which object is qualified.
- Prefer `=` for ordinary value initialization and `{}` for aggregates/zero initialization. Choose `()` or `{}` for construction by semantics. Respect overload selection, narrowing, and `auto` deduction rather than replacing syntax mechanically.
- Ordinary functions use leading return types, such as `Result read()`. Use trailing return types, such as `auto read(...) -> Result`, when dependencies on parameters or similar reasons make them appropriate; do not convert every function for style.
- Prefer explicit lambda captures so borrowing, copying, and ownership transfer remain visible. Captured lifetimes must cover execution, especially escape/async cases; explicit capture alone proves no safety.
- Constructors that could convert implicitly default to `explicit`. Permit implicit conversion only with a clear, justified semantic contract, not merely to save a construction expression.

#### 8.1.6 Comments and Information

- Follow Section 7's English comments, object coverage, and C++ documentation form. Headers describe file responsibility; classes, functions, and variables have corresponding explanations. Keep public contracts at declarations and reasons/invariants at implementations without duplicating full documentation.
- Use preceding `///` uniformly for type, function, and member declarations, including enum items and private declarations. Use `//` for local variables and implementation logic; do not alternate declaration block-comment styles by visibility.
- Document units, legal ranges, results, failures, ownership, lifetime, and threading constraints not apparent from signatures. Explain non-obvious semantics of special members, template parameters, and constraints.
- Distinguish defaults from initial values, and do not invent inapplicable ranges or guarantees. Simple locals, loop variables, structured bindings, and captures may share adjacent block descriptions, provided purpose and required borrowing/lifetime/special-value constraints are clear.
- Internal comments focus on resource handoff, lock boundaries, commit order, rollback, and cleanup that statements do not make obvious. Do not mechanically comment every ordinary control-flow stage. Describe enums and each item separately under Section 7.
- Comments may wrap; the single-line expression rule does not apply to them. Preserve valid contract information; reducing duplication is not permission to remove guarantees.

#### 8.1.7 Long Lines and Wrapping

- Indent with four spaces. Attach pointer/reference symbols to the type, as in `Entry* entry` and `Entry& entry`. Formatting must not obscure pointer versus pointee `const`.
- Keep braces around `if`, `else`, `for`, `while`, and `do` bodies, including single statements.
- Long lines are allowed; do not force wrapping to a fixed width.
- Keep function declaration/definition headers on one line, including return type, name, arguments, and qualifiers. Put the opening definition brace on that line.
- Do not wrap ordinary statements, calls, argument lists, or expressions solely for width. Function/lambda/class bodies retain normal structural lines; do not compress entire functions into one line.
- The first `enum`/`enum class` item starts on the line after `{`, and `}` has its own line, even for a one-item enum.
- Keep chained calls on one line unless exceptionally long chains obscure their stages; then wrap at chain boundaries. Exceeding an old width limit alone is not a reason.
- Complex or deeply nested conditions may wrap by logical groups while preserving parentheses, precedence, and evaluation. Simple conditions stay on one line.
- Do not measure complexity solely by line count or character count, or compress excessively to avoid readability requirements.

#### 8.1.8 Logical Blocks and Blank Lines

- Logical blocks are stages such as input validation, preparation, execution, branching, commit, notification, and cleanup. They may span several lines and are not counted merely by braces.
- Keep declarations, comments, and statements belonging to one stage together. A variable or statement on its own line is not automatically a separate block.
- Separate logical blocks with a blank line, normally one.
- If a function contains more than one logical block, its first line after the opening brace must be blank, before the first stage's comments or statements.
- A very simple single-block function needs no initial blank line. Short length does not exempt a multi-stage function.
- Divide branches and internal stages by actual logic, not mechanically at every brace pair. Keep comments beside their block; place the blank separator before its comment.

#### 8.1.9 Headers and Accessor Results

- Headers default to `#pragma once`; use macro guards when unsupported and register the compatibility difference. Do not use both for the same purpose.
- A `.cpp` includes its own header first when present, then project, third-party, and standard headers in separate groups, sorting within groups by path. Headers directly include their declaration dependencies, rather than relying on caller order or incidental transitive includes. Respect external headers with real ordering requirements.
- Forward declarations reduce unnecessary dependencies only when robust and no complete type is required. Include definitions when needed; never invent declarations for standard-library or third-party types. Prefer implementation-only includes in `.cpp`.
- Organize templates and caller-visible `constexpr` definitions as the language requires. Otherwise simple getters, setters, and single-step forwarding may be inline in `.hpp`; complex validation, synchronization, and state transitions belong in `.cpp` even if short.
- Non-template definitions in headers must satisfy the one-definition rule, using appropriate inline semantics. A header definition does not guarantee compiler inlining or a speed improvement.
- Return scalars and small values by value by default. Larger members may use const references/views to avoid unnecessary copies. State borrow validity and invalidation conditions; read-only access does not imply immutable storage or thread safety.
- References/views must satisfy caller lifetime and synchronization contracts. Never return references to temporaries or expose data valid only under an internal lock as safe after unlocking. Use independent values or controlled snapshots when a valid borrow cannot be guaranteed.

#### 8.1.10 Errors and Return-Value Checks

- Where supported, prefer `std::expected` for recoverable failures, with stable error categories and context. Do not implicitly upgrade tools or add a dependency for it; compatibility targets retain their established result representation.
- Exceptions cover paths that cannot return normal results and third-party boundaries. Specify what may propagate, where conversion/termination occurs, and failure-state guarantees. `std::expected` does not imply non-throwing behavior or justify `noexcept`. Do not disguise broken internal invariants by catching everything and returning a business error.
- Mark error results, resource acquisition, and other results whose omission risks misuse `[[nodiscard]]`, at an appropriate type or function. Decide ordinary queries by actual misuse risk rather than annotating every non-`void` operation mechanically.
- When ignoring a result is justified, express it explicitly and retain the reason. Blanket discards, meaningless casts, and diagnostic suppression must not bypass error handling or resource duties.

<a id="c"></a>

### 8.2 C

- Functions, variables, and owned files default to `snake_case`; macros use `UPPER_SNAKE_CASE`. Public symbols have necessary module prefixes. Do not invent reserved identifiers or copy C++ class-local short names into global C scope.
- Owned type names use lowercase words, joining unavoidable compounds with underscores. Retain explicit structure tags and use forms such as `struct entry*`; do not default to `typedef` that hides tags or pointer ownership.
- Public headers contain only caller-required declarations; opaque internal structures are allowed. Restrict private functions/state to file-local linkage. Fix guards, includes, and C/C++ interoperability according to supported use.
- Headers default to `#pragma once`; use macro guards on unsupported tools and register the compatibility difference.
- Document pointers, lengths, capacities, encoding, and ownership together: allocation, release, and output-parameter state on failure. Avoid unbounded string operations. Casts do not prove alignment, aliasing, or representation validity.
- Separate errors from valid results. Pair acquisition with release on every exit, including partial initialization. A clear shared cleanup exit is acceptable.
- Verify signedness and bounds in bit operations, integer conversions, and buffer arithmetic. `volatile` is not synchronization. Macros must avoid repeated argument evaluation and hidden control flow; prefer functions/constants when suitable.
- Use the project's existing formatter. C++ single-line function-header and initial-blank-line conventions do not automatically apply to C.

<a id="go"></a>

### 8.3 Go

- Package names are short, lowercase responsibility names; exported identifiers use `PascalCase`, others `camelCase`. Preserve conventional initialisms such as `ID`, `URL`, and `HTTP`: `userID`, not `userId`. Do not repeat context with `Get`, package prefixes, or empty receiver types.
- Receivers use accurate complete words, such as `(store *Store)`, consistently for the type; do not replace responsibility names with single-letter abbreviations.
- Filenames prefer lowercase words; concatenate unavoidable compounds, such as `readtimeout.go`. Preserve Go-required platform, build, and test suffixes, including their underscores.
- Use `New` when the package's primary type needs a constructor, and names such as `NewEntry` for other types where needed. Do not force constructors onto usable zero values merely for naming consistency.
- Format with `gofmt`, without manual column alignment overriding it. Organize files by package responsibility and tests by language discovery conventions; make build constraints and platform ownership explicit.
- Group imports as standard library, third party, and project packages, separated by blank lines and sorted within groups by existing tools. Do not install tools just to reorder imports.
- Prefer external test packages for public behavior and same-package tests for internal invariants. Do not expose production internals to accommodate external tests or use package selection to narrow verification.
- Keep interfaces minimal and consumer-oriented, rather than mirroring every concrete type. Choose value/pointer receivers by copying cost, mutability, and semantics; do not copy lock-bearing or otherwise noncopyable state casually.
- Express recoverable failures with `error`; preserve identifiable causes when adding context. Do not classify by error-string comparisons or hide external-input/ordinary failures with panic/recover.
- Explicitly accept and propagate Context for cancellable operations; do not replace request context with an unrelated background context. Assign goroutine, channel, timer, and connection shutdown/release duties. Extra goroutines must not conceal blocking or errors.
- Document mutability and sharing of exposed slices, maps, pointers, and interface values. Returned collections are not automatically independent copies. An interface containing a typed nil pointer is not necessarily a nil interface.
- Data packages must not initiate networking, spawn tasks, or alter process environment during initialization. Document exported behavior and failure constraints, and complex internal stages as required by Section 7.

<a id="rust"></a>

### 8.4 Rust

- Modules, functions, variables, and files default to `snake_case`; types/traits use `PascalCase`; constants/statics use `UPPER_SNAKE_CASE`. Use `rustfmt` and the project's configured diagnostic entry points.
- Prefer `entry.rs` without submodules and `entry/mod.rs` as the parent entry with submodules. Do not declare conflicting forms together.
- Ordinary constructors may consistently use `new`; `Self` versus `Result<Self, Error>` expresses fallibility. A `try_` prefix is not mandatory.
- Simple getters use property names such as `timeout()`. Name mutations for their behavior, without a universal setter pattern or automatic mutators for every field.
- Group imports as standard library, third party, and current crate, separated by blank lines and sorted with existing tools.
- Default to private, widening to local/crate visibility only for actual use. Traits, generics, and macros serve real constraints/reuse; do not hide ordinary control flow or build frameworks with one forwarding implementation.
- Use `Result` for recoverable errors and `Option` for absence. Do not use `unwrap/expect` on external inputs or ordinary I/O failure. Internal invariant failures follow the development standard's isolation rules.
- Libraries/domains use explicit error types; application edges may aggregate errors while preserving causes. This choice does not authorize an error-handling dependency.
- Prefer borrowing and explicit ownership. Unjustified cloning, leaks, and indefinite sharing must not silence compiler errors. `Arc` expresses shared ownership, not synchronization.
- Confine `unsafe` to a minimal boundary and explain caller/implementation safety obligations. Raw pointers, FFI, aliasing, and release duties require concrete constraints, not merely “for performance.”
- Explain async waits while locked, future cancellation, and destruction duties. Dropping a task handle does not prove draining; destruction must not omit required asynchronous external cleanup.

<a id="python"></a>

### 8.5 Python

- Modules, functions, and variables use `snake_case`; classes use `PascalCase`; constants use `UPPER_SNAKE_CASE`. Organize imports by package boundaries; do not hide dependencies with wildcard imports or global search-path changes.
- Enum members and module constants use uppercase words, such as `READY` and `LIMIT`, joining unavoidable compounds with underscores (`READ_TIMEOUT`). Enum types remain `PascalCase`.
- Use Google-style docstrings with `Args`, `Returns`, and `Raises` only as needed. Do not pad simple descriptions with empty sections, repeat type annotations, or invent error guarantees.
- Default to absolute imports within packages; use explicit relative imports only for a small number of justified cases. Paths must match actual package boundaries.
- New file-path handling defaults to `pathlib.Path`, converting at external boundaries as needed. Do not rewrite unrelated existing code merely to adopt it.
- Prefer `dataclass` for simple data records; choose immutability by semantics. Types encapsulating complex behavior/invariants follow their responsibilities; do not convert all classes mechanically.
- Prefer keyword-only configuration, boolean switches, and easily confused same-type arguments. Preserve published positional-call compatibility.
- Use the existing Black configuration; format immediately after edits and check syntax statically. Annotate functions/public data boundaries, but validate dynamic external input at runtime: annotations are not execution guards.
- Do not use mutable defaults for cross-call state. Distinguish `None`, empty containers, and false values. Specify serialization, path, and subprocess input types/boundaries rather than removing ambiguity through string conversion.
- Release resources with context managers or explicit `try/finally`. Catch only exceptions that can be handled or translated, preserving causes. Bare `except`, default success, or swallowed cancellation must not hide failure.
- Assign ownership to async tasks, threads, and subprocesses; await completion/cleanup after cancellation. Background tasks and synchronous blocking must not bypass lifecycle constraints.
- Imports must not run maintenance, connect to networks, or execute tests. Separate executable entry points from reusable computation; preserve shell-argument/protocol-byte semantics and propagate failures, skips, and cleanup errors.

<a id="typescript"></a>

### 8.6 JavaScript / TypeScript

- Functions, variables, and fields use `camelCase`; types/classes use `PascalCase`. Do not add meaningless interface prefixes. Constants also use `camelCase`, including fixed module values.
- Ordinary module filenames use lowercase words, with `camelCase` for unavoidable compounds, such as `readTimeout.ts`. Component files follow component naming.
- Prefer `interface` for TypeScript object contracts and `type` for unions, mapped/composed types. Do not rewrite external types for uniformity or hide interface changes through declaration merging.
- Prefer `function` for named module operations and arrow functions for callbacks. All named TypeScript functions, including methods and named arrow bindings, explicitly declare return types; anonymous callbacks may infer them. Plain JavaScript uses its documentation/checking mechanisms, not TypeScript syntax.
- Default to `const`, using `let` for reassignment. Avoid implicit conversion/global state; distinguish `null`, `undefined`, empty strings, and zero rather than treating all absence through truthiness.
- Owned optional values default to `undefined`; reserve `null` for explicit business states or external protocols. Internal preferences must not alter protocol null semantics.
- Prefer `enum` for finite TypeScript states, documenting every member under Section 7. Projects requiring erasable-only syntax or other runtime restrictions register a Section 1.1 compatibility difference; do not silently disable compiler checks or change execution modes.
- Class-private fields default to TypeScript `private field`, not `#field`. Type-level access control is not runtime isolation. Plain JavaScript uses its actual encapsulation mechanisms, not TypeScript-only modifiers.
- Prefer named exports for owned modules, preserving framework-required defaults. Distinguish type/runtime imports; fix resolution, extensions, and browser/server targets in project configuration.
- Enable applicable strict checks, including null, index, and optional-field boundaries. Do not hide uncertainty through `any`, non-null assertions, casts, or ignore comments; validate unknown external data before converting it to domain types.
- Callers or explicit task owners observe Promise failures. Avoid unawaited async callbacks and hidden fire-and-forget. Handle cancellation, stale responses, and component destruction separately: signalling cancellation does not prove completion.
- Exposed data defaults to readonly intent; document shared references and runtime mutability. Type-level `readonly` does not deeply freeze objects; reference equality is not business identity.
- Retain semicolons, default to single-quoted ordinary strings, and use template strings for interpolation. Keep trailing commas in multiline structures where the target syntax permits, including applicable parameter lists. External fixed formats follow their syntax.
- Place unit/cross-module tests in separate `tests/` directories, organized by source responsibility and included in actual discovery. Do not expose production internals for this layout or migrate existing tests automatically during standards cleanup.
- Use the configured Prettier or existing equivalent. Do not weaken build/static diagnostics to obtain a pass. Importing business libraries must not start servers or alter the host environment.

<a id="web"></a>

### 8.7 HTML, CSS, and frontend components

- Use semantic HTML, with explicit control names, keyboard behavior, and focus handling. Visual appearance does not replace accessible names. Do not insert untrusted HTML through concatenation.
- CSS names express components/states; maintain shared design values through common variables/tokens. Control global scope, selector coupling, and cascade; escalating specificity or `!important` must not patch structural defects.
- Follow the owning public component/library's established CSS naming/isolation, including namespaces, states, and styling interfaces; BEM is not universally required. Establish ownership and match peers for new components. Do not replace conventions by preference; projects define missing conventions without implicitly introducing a library.
- Define component, event, input, and output contracts; component names default to `PascalCase`. Partition state by ownership; do not duplicate derivable state without justification or use array positions as mutable entities' stable identities.
- Component filenames match names (`Entry.vue`, `Entry.tsx`); ordinary modules follow JS/TS rules. Event handlers use `on`, such as `onClick` and `onChange`; business operations still use responsibility names.
- In Vue, prefer Composition API, explicit props/emits, and TypeScript single-file components; composables use `use`. Other frameworks retain their lifecycle idioms; migration to Vue is not required.
- Vue single-file sections appear as `script`, `template`, then `style`, omitting unnecessary sections.
- Reclaim listeners, observers, timers, animation frames, network tasks, and GPU resources with their owners. Keep high-frequency computation/external mutable engines separate from reactive UI state; avoid deep proxies that obscure updates/ownership.
- Separate business state, rendering, and data adaptation. Projects register frameworks, directories, style isolation, and build targets; one application's directory names are not a universal frontend layout.

<a id="jvm"></a>

### 8.8 Java / Kotlin

- Packages are lowercase; types use `PascalCase`; methods/fields use `camelCase`; constants use `UPPER_SNAKE_CASE`. Narrow visibility to actual contracts; public setters/mutable collections are not default APIs.
- Define Java null boundaries explicitly; prefer Kotlin immutable `val` and nullable types. Do not bypass validation with unjustified non-null assertions. Validate Java/Kotlin interop and reflection inputs too.
- Express exceptions/results consistently across calls, preserving causes and interruption/cancellation semantics. Do not catch everything and return successful defaults or return damaged state to callers.
- Close files, connections, and other closeable resources through structured scopes; GC does not replace closing. Threads, executors, coroutine scopes, and tasks need owners; global orphan tasks must not hide lifecycle duties.
- Distinguish readonly collection interfaces from underlying mutability, and synchronize actual concurrent access. Avoid deep inheritance for similar methods and annotations/reflection that conceal commits or resource ownership.
- Existing project tools control formatting/static diagnostics; register platform versions and interoperability separately.

<a id="csharp"></a>

### 8.9 C#

- Types, methods, and properties use `PascalCase`; parameters/locals use `camelCase`. Follow the `I` interface prefix and usual `Async` suffix for awaitable methods. Private instance fields have a trailing underscore, such as `entry_`.
- Use brace-block namespaces rather than defaulting to file-scoped namespaces. Use local `var` when the initializer makes the type obvious; otherwise spell out the type.
- Data models default to ordinary classes. Choose `record` individually for actual value equality, copying, and immutability needs; do not convert all data classes mechanically.
- Use nullable-reference analysis where supported and still validate external input. Null-forgiving operators must not remove unproven risks. Define copying semantics for values, references, and mutable collections.
- Use `using` or appropriate async disposal scopes. Specify ownership of native handles/FFI buffers; finalizers cannot guarantee timely release or business commits.
- Callers observe Task errors/completion and explicitly propagate CancellationToken. Avoid `async void` except framework-required event boundaries; synchronous waiting does not replace a correct async chain.
- Preserve causes during exception translation and distinguish cancellation from failure. Do not call unknown callbacks while locked; background tasks must not implicitly violate UI-thread/context constraints.
- Organize namespaces, source, and test projects by responsibility. Existing configuration controls formatting/analyzers. ABI, marshalling, and native dependencies are explicit interface contracts.

<a id="lua"></a>

### 8.10 Lua

- Owned functions, variables, and files default to `snake_case`. Use `local` state and explicitly return module APIs; do not share business state through implicit globals.
- Indent with four spaces. Recoverable failures default to `nil, error`, clearly distinguished from success. Handle programming errors/host-required exception boundaries by contract rather than downgrading every error.
- Distinguish `nil`, `false`, empty tables, and zero. Do not mix implicit length assumptions for lists, maps, and sparse tables or depend on unspecified iteration order.
- Tables are shared references; specify readonly-view/copy semantics. Metatables/dynamic dispatch serve actual requirements without hiding ownership or error boundaries.
- Fix external error conventions; use `pcall/xpcall` only at boundaries able to handle errors. Do not swallow failure or equate coroutine suspension with completion.
- Clean files, connections, native userdata, and coroutines according to runtime capabilities. Register version, sandbox, and numeric representation; Lua environments do not all share identical libraries/integer behavior.

<a id="sql"></a>

### 8.11 SQL and database migrations

- Owned tables, columns, and constraints default to `snake_case`; uppercase SQL keywords such as `SELECT` and `FROM`. New table names are plural (`entries`); do not rename published schemas for singular/plural uniformity. Follow the target dialect's case/quoting rules.
- Explicitly use `AS` for aliases where supported, including applicable table/column aliases; uniformity does not justify unsupported syntax.
- Bind data parameters. Dynamic identifiers come from explicit allowed sets and use correct dialect quoting; do not concatenate external text into executable SQL.
- Select required columns explicitly; specify ordering/tie handling for stable or paged results. Stable interfaces must not depend on `SELECT *` or incidental database order.
- Transactions, constraints, and application validation jointly enforce commitments. Define rollback and unknown outcomes; verify bulk-operation scope and do not hide partial success by catching errors and continuing.
- Version migrations by fixed order/content identity; do not rewrite deployed migrations. Define compatibility windows, locks/costs, rollback points, and irreversible boundaries. Presence in the repository does not authorize execution.

<a id="shell"></a>

### 8.12 Shell / PowerShell

- Declare interpreter/platform; do not confuse POSIX shell, Bash, and PowerShell syntax/exit rules. Follow environment naming conventions and do not repurpose reserved system variables for task state.
- Internal PowerShell functions use short complete responsibility names; external commands use Verb-Noun, preferably approved verbs. Owned Shell functions use lowercase words and `camelCase` for unavoidable compounds.
- Owned CLIs prefer readable long options, with documented short aliases for common ones. Use actual interpreter/parser syntax rather than forcing one prefix across platforms. Aliases preserve semantics; do not rewrite external tools' fixed arguments.
- Prefer argument arrays/literal paths, specifying wildcard, space, encoding, and stdin semantics. Do not eval/concatenate external inputs into commands; JSON escaping is not shell escaping.
- Check actual exit codes and every pipeline stage, not just global strict mode; propagate output-parsing errors too. Logs must not print credentials or complete secret-bearing commands.
- Assign ownership to temporary paths and descendant processes. Use verifiable exit/cancellation cleanup; resolve and confirm target boundaries before deletion/moving. Do not compose destructive operations across shells.
- Separate reusable logic from command entry points. Do not modify user/system environment for a local path issue, download dependencies implicitly, or turn one-off operations into import/startup side effects.

<a id="protocol"></a>

### 8.13 Protocols, configuration, and generation templates

- Define names, types, units, defaults, missing values, and unknown-value policy. Schemas own structure; product contracts add cross-field semantics. Do not maintain contradictory handwritten structures.
- Follow compatibility rules for field numbers, enum values, and reservations; never reuse reserved identities. JSON/YAML/TOML configuration must reject ambiguity, with explicit parser capabilities and duplicate-field policy.
- Maintain templates to production safety/coding standards. Encode inputs/outputs for their contexts; do not generate source, SQL, HTML, or shell by concatenating unescaped text.
- Trace inputs, generators, and outputs; do not hand-edit generated results. Projects decide whether to track artifacts; generation does not exempt consumer verification.
- Configuration must not contain real secrets. Mark example values clearly; missing required settings fail explicitly rather than silently selecting permissive access, external services, or production environments.

## 9. Documentation and design ownership

READMEs own purpose, navigation, and necessary startup instructions; the development standard owns process/acceptance; this document owns coding/organization, with language details in Section 8. Feature documents own complete feature design; project configuration owns executable parameters; validation records own actual evidence/boundaries. Reference or generate commands/thresholds from one configuration rather than duplicating them manually.

New features/public behavior changes follow development Section 2.4: name the feature document, resolve critical details/boundaries, and obtain approval before implementation. Directory examples here are not approved designs; documented commands are not execution authorization.

Retain current documents and necessary compatibility material; use version control for obsolete process history. Published API descriptions, migration guides, legal texts, and historical protocol fixtures are not disposable Markdown. Update navigation/references when moving files; externally promised links require a migration arrangement.

## 10. Reorganization, migration, and cleanup

Bound reorganization by identified users and objectives. Before moving, renaming, or deleting, inspect build entry points, imports, package/test discovery, generators, resource loading, installation manifests, documentation, and external consumers. Distinguish path-only changes from behavior/publication changes; do not conceal the latter as cleanup.

Remove dead code, obsolete switches, and temporary implementations after checking references and behavior. Text-search absence alone does not establish that dynamic loading, platform files, or compatibility paths are unused. Do not retain `old`/`backup` source copies instead of version control.

Regenerable caches are not automatically safe to delete now. Verify paths/ownership, retaining active outputs, raw evidence, user files, compatibility fixtures, and licenses. Ignore rules control tracking, not deletion safety, publication manifests, or secret checks.

Do not rename, convert, or upgrade dependencies repository-wide merely for style. Preserve user changes; development/project rules still govern verification and commits.

## 11. Integrating rules into automated checks

Register this document's content identity/applicable language sections and map requirements to existing configuration/check entry points. Adding this document does not authorize changing schemas, inventing fields, or claiming unsupported gate capabilities; implement required integration explicitly.

| Requirement | Appropriate enforcement |
| --- | --- |
| Formatting, naming form, imports, language diagnostics | Configured formatters, compilers, or static checks with explicit versions/file scope |
| Module public surface/dependency direction | Language visibility, build-target dependencies, and applicable boundary checks |
| Ownership of tests, examples, generated and published files | Actual discovery manifests, build/package inputs, and generation consistency checks |
| Semantic naming, responsibility, abstraction, complexity tradeoffs | Design/contracts, relevant counterexamples, and necessary focused review; do not pretend static tools understand every design |
| Suppressions, exclusions, migration | Versioned configuration/exact scope; verify protection changes/exceptions under the development standard |

Projects may associate obligations with existing DEV-CONFIG, DEV-CORE, DEV-TRACE, DEV-ORACLE, and DEV-IMPACT IDs. Listing an ID is not execution and does not redefine it; register necessary extensions in the project namespace.

Checks cover maintained handwritten production code, tests, and tools. Generated/third-party exclusions have explicit provenance; moving directories must not evade checks. Do not impose universal line counts, file counts, or directory-depth limits divorced from responsibility, or create empty wrappers to satisfy metrics.

Documentation is ready when the two common standards have clear responsibilities, language/project registrations are explicit, references agree, and unsupported tools are not presented as implemented. Project adoption/release qualification follow the development standard separately. Change the common layer only for actual gaps it cannot express; address already-covered issues through project configuration, implementation, or counterexamples.

---

<a id="chinese"></a>

# 多语言编码与文件组织规范

> 规范版本: 1.0.0. 内容身份由 Git 提交及文件摘要固定. 本文可与 [AI 开发与长期维护规范](development.md) 一起跨项目复用; 文档齐备不表示项目已经接入或验证通过.

本文规定代码如何表达、模块如何划分、文件如何归属. 开发准入、授权、验证强度、原生证据、门禁及发布遵循 `development.md`, 本文不重复制定另一套流程或通过标准. 目标是让 AI 在长期维护中保持一致、直接、可验证的实现, 同时尊重不同语言的语义与生态.

## 1. 适用范围与规则归属

“必须”“应”“可”的含义及权限边界沿用开发规范. 本文覆盖手写生产代码、测试、构建工具、迁移脚本、生成模板和可执行示例. 生成结果与第三方文件按其来源管理, 不为统一风格直接改写.

| 规则来源 | 唯一负责的内容 |
| --- | --- |
| 开发与长期维护规范 | 工作流程、设计准入、契约验证、证据和门禁 |
| 本文通用章节及第 8 节 | 跨语言原则、各语言具体写法、模块与文件组织、规范落地要求 |
| 已提交工具配置 | 将本文的格式与诊断要求落实为工具参数, 不维护另一份编码正文 |
| 项目设计与配置 | 真实组件、依赖边界、目录映射、产品接口、工具入口与参数, 以及明确登记的编码差异 |
| 项目授权约定 | 哪些操作已获准, 哪些需要用户决定 |

通用语言规则集中到第 8 节, 项目已确认的特殊习惯按第 1.1 节登记差异. 项目配置记录实际支持版本、工具参数和兼容差异, 不另建语言规范副本. 缺少会影响公共接口或兼容性的约定时先完成必要决策; 私有局部写法沿用有效规则及相邻代码, 相邻代码不自动构成覆盖依据.

每项事实只维护一份权威来源. AI 入口只链接规则与项目配置; 不在多个入口复制规范, 不另建随会话增长的规则副本. 复制到其他项目时同步调整配套文档链接, 项目路径与工具命令留在项目侧.

<a id="overrides-cn"></a>

### 1.1 项目级覆盖

本文是可复用的编码基线. 项目可在既有维护指南 (如 `CONTRIBUTING.md`) 的固定章节登记差异, 由项目 `AGENTS.md` 明确链接并要求开始工作时读取; 不复制或分叉整份本文. 仅在项目没有合适载体时新增项目规则文件. 子目录规则必须能从项目入口发现, 不能靠隐藏文件或临时提示静默覆盖.

同一编码事项按以下顺序确定有效规则: 用户当前明确要求 > 已确认且适用于该路径的项目差异 > 本文对应语言规则 > 本文通用规则. 子目录差异必须明确覆盖的父级条款及范围; 仅因文件更近、更新或措辞更强不自动优先. 重叠差异没有明确承接关系时说明冲突并确认, 不自行选择较宽松的一项.

每项差异至少记录以下内容, 未覆盖的事项继续继承基线:

| 字段 | 要求 |
| --- | --- |
| 对应条款 | 通用规范版本及章节/锚点, 明确替换哪项要求 |
| 适用范围 | 仓库相对路径、语言及生产/测试/工具等文件类别 |
| 替代规则 | 可直接执行的完整要求, 不只写“遵循项目习惯” |
| 原因与依据 | 兼容、生态或产品原因, 以及用户确认或已有有效项目约定的依据 |
| 落实位置 | 对应格式化器/检查配置; 无自动检查能力时如实说明 |

例如项目维护指南可以登记一项已确认的排版差异:

```markdown
## 编码差异

- 对应条款: coding.md 1.0.0 第 8 节的非 C++ 默认列宽.
- 适用范围: tools/**/*.py 中的手写 Python 文件, 包括测试.
- 替代规则: Black 的 line-length 使用 88, 替代通用的 160 列目标.
- 原因与依据: 项目已确认沿用现有 Python 工具配置; 正式登记时链接实际确认记录.
- 落实位置: pyproject.toml 的 [tool.black].
```

该示例不代表当前项目已采用 88 列. 工具配置落实已确认的差异, 不凭配置值自行生成授权或覆盖结论; 文本与工具冲突时确认真实约定并同步. 项目规则可调整命名形式、注释语言/密度、排版与目录映射, 不能借编码覆盖弱化正确性、兼容契约、开发规范的证据与验收要求, 也不能扩大测试、下载、提交或发布权限. 此类变化仍按相应决策与例外流程处理.

采用结构化配置时复用项目已有的合法字段与入口, 不擅自给既有 `development.json` 增加 Schema 未支持的字段. 通用规范升级时核对差异仍对应原意, 不让旧覆盖静默套用到已改变的条款.

## 2. 命名与作用域

- 极简命名是所有语言的共同原则, 适用于类型、函数、变量、字段、模块和自有文件. 优先选用一个准确、完整的单词, 同一概念使用同一名称. 不使用截断拼写和自造首字母缩写追求短小, 不用含混的短名代替准确表达.
- 缩写只允许使用共识级别的术语, 例如 ID、UUID、TCP、TLS、RPC、TTL. 不把个人习惯或局部约定当作行业共识; 大小写按所属语言适配. 注释补充契约和约束, 不能弥补错误或含混的命名.
- 利用模块、类型和词法作用域提供上下文, 避免重复所属对象的名称. 不为缩短名字额外制造空壳类或嵌套层.
- 先确定能准确表达职责的单词, 再按语言及符号角色适配首字母大小写, 常量等按对应规则使用全大写. 只有单词与现有作用域仍不能消除实际歧义、区分必要单位或语义时, 才使用多词名称, 并按语言选择驼峰、下划线等连接形式. 语言支持复合写法不构成使用多词的理由.
- 语言、框架或外部接口要求的前后缀及固定名称保留, 不改写外部固定接口或序列化字段来满足内部风格. 不因个人偏好推翻已确定的名称, 也不为落实本规则进行无关的批量重命名.
- 动作、查询和转换的名称体现可观察行为. 读取接口若会触发 I/O、修改状态、消耗迭代器或移交所有权, 必须在接口契约中明确; 不由看似无副作用的名称掩盖.
- 在类型或名称中区分容易混用的单位、身份、计数、偏移和时间域. 不以注释弥补本可避免的同类型参数混淆.
- 定义放在拥有该职责的最窄有效作用域, 默认私有. 只因真实使用契约公开类型和接口, 不为测试便利扩大生产 API.
- `utils`、`common`、`manager` 等宽泛名称不能代替职责划分. 已有名称有明确职责时可以保留; 无归属的代码先寻找所有者, 不统一堆入公共目录.

## 3. 实现表达与复杂度

### 3.1 函数、状态与数据

函数围绕一个可解释的操作或不变量组织. 校验、准备、提交、通知和清理等阶段保持清晰, 变量靠近其使用及有效期. 允许直接的早返回和局部辅助函数, 不为达到固定行数将一个提交边界拆散到多层跳转中.

优先用类型、封装、明确结果与语言原生资源机制表达约束. 必须区分缺失、空集合、零值、错误和结果未知; 不以同一哨兵值或成功默认值掩盖不同状态. 多个互斥布尔标志出现非法组合时, 优先采用能限制状态空间的表达.

共享可变状态、借用关系、隐式转换和惰性求值的副作用必须可识别. 实现不得依赖未约定的遍历次序、求值次序或环境默认值. 并发、原子性、时间、数值和错误保证按开发规范第 3-4 节建立契约, 不在本文另定较弱版本.

### 3.2 抽象与效率

- 优先使用目标工具链已支持的语言和标准库能力. 是否采用新语法取决于表达、正确性与成本, 不以语法新旧代替设计判断.
- 在满足既定语义、正确性和资源约束且不降低性能的前提下, 应尽量使用所属语言的跨平台标准库实现. 标准库无法满足实际需求时才采用必要的平台专用接口或其他实现, 并将差异限制在清晰的适配边界; 不因接口属于标准库就推断其语义或性能与现有实现等价.
- 新抽象须服务于现有不变量、明确边界或真实使用者. 允许为可测边界、平台适配或安全隔离建立必要接口; 不仅为未来可能复用制造框架、插件或继承层.
- 相似代码只有在语义、失败保证、生命周期和演进方向一致时才适合合并. 跨领域状态机不因结构相似就强行统一, 同一业务规则也不应无理由重复实现.
- 精简减少重复知识和理解成本, 不能删除必要检查、注释、错误上下文或测试. 不用密集表达式、多层三元、隐藏副作用或过度元编程换取少几行代码.
- 常规实现就应注意复杂度、分配、复制、访问局部性和锁范围. 额外优化复杂度需要明确依据, 区分静态分析与实测收益; 不把少行、少对象或理论复杂度直接当作性能证据.

## 4. 模块与依赖边界

模块按业务职责、数据所有权和变更原因划分. 对外接口稳定而精简, 内部实现通过语言或构建系统限制访问. 测试可使用受控的内部测试机制, 不能因此把内部符号变成承诺维护的公共接口.

核心规则与 I/O 适配按开发规范第 3.2 节分离. 核心使用明确的数据、事件和边界接口; 启动入口负责组装具体实现. 不为每个类型强制建立接口, 不以依赖注入容器或全局服务定位器隐藏真实依赖.

项目登记允许的组件依赖方向和公开入口. 跨组件不得通过私有路径、运行时反射或测试捷径绕过边界. 意外依赖环应消除; 确实互相依赖且不可拆分的代码明确为共同维护单元, 不能只靠改目录名字宣称完成分层.

公共协议和跨语言数据结构维护单一语义来源, 各语言绑定保留原生表达. 共享的是字段含义、单位、错误语义与兼容承诺, 不是要求内部类型形状一致. 生成器、共享向量和手写适配分别有明确所有者.

第三方 API、异常、对象寿命和版本差异在适当边界收容. 边界适配应有实际隔离目的, 不逐层机械包装全部外部函数. 产品确实以外部协议为公共接口时, 明确这项依赖及兼容责任.

## 5. 文件划分与路径

文件围绕一个紧密相关的职责组织, 私有辅助逻辑靠近调用方. 拆分依据是可见性、所有权、依赖或独立变更需要, 不按任意行数上限机械拆文件, 也不把无关功能放入一个大文件. 语言要求的类型与文件对应、模块入口或导出结构优先遵守.

文件名使用稳定的职责名称, 不采用 `new`、`final`、`v2-final`、日期或作者名区分现行实现. 协议版本、数据库迁移序号、历史兼容夹具等具有运行语义的命名保留, 不当作临时文件删除.

公共入口、内部实现、测试支持和生成结果必须能从路径与配置识别. 不强制所有语言采用同一目录树: 单元测试可以按语言惯例与实现同目录, 独立系统测试和跨组件契约测试可以集中组织, 二者均须纳入真实发现入口.

自有新增路径采用目标平台可用的命名, 避免仅大小写不同的路径、保留名称以及不必要的特殊字符. 大小写敏感平台与不敏感平台之间的重命名要核验 Git 记录. 非必要不改既有公开路径, 原样保留外部资源有语义的名称.

文本编码、换行和可执行权限由版本控制配置固定; 新文本默认使用 UTF-8. 工具从明确的工作目录或自身位置解析路径, 不依赖开发者个人目录、隐式搜索路径或偶然的当前目录. 不把本机绝对路径写成可移植源码的运行前提.

## 6. 仓库内容与目录职责

下表规定职责归属, 名称仅为常见映射示例. 项目只建立实际需要的目录, 沿用语言和构建生态要求; 不为形式齐全创建空树, 不因采用本文自动迁移已有仓库.

| 内容 | 归属与边界 |
| --- | --- |
| 生产源码 | 所属组件及语言原生源码目录, 如 `src`、`internal`、`lib`; 允许语言惯用的根包布局, 工程入口有明确职责 |
| 公共 API | 明确导出入口、头文件或包; 私有实现与公共承诺可区分 |
| 测试及测试支持 | 随组件放置或进入 `tests`; 测试依赖不反向进入生产交付 |
| 夹具、协议向量和反例 | 归属具体测试或共享契约; 声明来源与发现方式, 受保护反例纳入版本控制 |
| 性能与长期场景 | 在测试体系中单独标识入口、预算和授权, 可映射至 `bench` 等目录; 不由普通测试意外触发 |
| 构建、生成和维护工具 | `tools` 或生态指定目录; 有稳定入口、输入输出及失败语义, 临时脚本不成为隐藏依赖 |
| 协议、Schema、迁移 | 所属接口或组件的唯一源目录; 迁移序列与兼容样本按语义保留 |
| 生成代码 | 明确标记与输入、生成器的关系; 是否跟踪由消费和构建需求决定, 不能笼统忽略 |
| 示例 | 消费实际公共接口, 标明完整示例、片段、伪代码或预期失败; 可运行项进入相应验证发现范围 |
| 配置与部署描述 | 按环境和用途归属, 只提交可共享配置及安全模板; 秘密使用外部引用 |
| 文档 | 导航、规范、设计、使用说明与最新验证各有唯一入口; 历史修订由版本控制保存 |
| 第三方源码与许可 | 按来源隔离或登记; 保留实际所需许可与 NOTICE, 不因根目录已有许可证而删除第三方义务 |
| 构建输出、缓存、临时数据 | 进入明确的可再生输出目录或获准缓存位置, 默认排除源码提交; 发布只选明确清单中已绑定证据的实际产物, 不打包整个输出目录 |
| 原始运行证据 | 按开发规范封存于声明的本地或受控存储, 保留失败记录; 不因位于构建目录就当作可随意清理的缓存 |

单仓库多组件按独立构建、交付和所有权边界组织. 根工具负责协调, 组件声明实际输入及对其他组件的依赖. 声称可独立分发的组件必须列明构建与运行所需文件, 不能依赖父仓库的未声明内容.

生产构建不得依赖个人缓存中恰好存在的未声明文件. 工具、生成代码和测试夹具若是必需输入, 必须纳入来源与依赖管理; 不以“只是脚本”逃避维护与验证.

## 7. 注释、排版与可读性

注释说明当前职责、契约、原因及非显然约束. 公共接口说明参数、返回值、错误、所有权和并发边界; 实现说明状态变化、提交及清理等关键阶段. 单位、范围、默认或初始状态、空值和特殊值按实际需要说明, 不编造不适用的保证.

手写代码注释使用清晰、标准的英文, 遵循英文语法和标点, 标识符、协议字段及引用保持准确. 这不要求将产品界面、测试输入或 Markdown 文档改成英文. 文件、类、函数和变量默认都有职责或用途说明, 包括内部实现、测试及模板. 按语言适配的是注释语法、放置方式和说明详略, 不是只给公共 API 写注释. 不设注释行数比例, 不逐语句复述代码; 项目需要不同语言或覆盖粒度时按第 1.1 节明确覆盖.

| 对象 | 基线要求 |
| --- | --- |
| 文件 | 手写源码、测试和脚本在文件头简要说明作用及所属职责; 测试文件说明验证对象. 使用语言适用的文件注释或模块 docstring, 不只是重复文件名, 不写作者、日期或修改流水账 |
| 类与其他类型 | 类、结构、接口及主要类型说明职责、表示的概念和关键不变量; 按需说明所有权与线程约束, 不只把类型名改写成一句话 |
| 枚举与变体 | 枚举类型说明表示的状态或分类, 每个枚举项分别注释其含义和适用条件; 联合类型的命名变体、结构字段及具名状态常量同样说明各自语义, 不以整体类型说明代替各项说明 |
| 函数与方法 | 声明或定义处说明操作目的, 按需补充参数、返回值、失败及副作用; 私有函数同样适用. 声明与实现不重复完整契约, 纯访问器或接口实现可明确承接已有属性/接口文档, 新增语义仍须补充 |
| 变量与常量 | 成员、全局、常量和配置项在定义附近说明用途及必要单位、范围和特殊值. 参数可在函数文档中集中说明; 局部变量默认有用途说明, 紧密相关的简单临时量、循环变量、结构化绑定或捕获可由相邻代码块说明共同覆盖, 不强制每个对象单独占一行注释 |

文件头说明放在语言允许的位置, 保留 shebang、编码声明、构建指令、许可证及模块语法的必要顺序; 文件说明与已有模块文档合并, 不重复添加. 不支持注释的载体通过 Schema 描述或所属文档说明; 生成和第三方文件仍遵循来源管理. 相邻说明必须能明确对应对象及用途, 不能用“处理数据”一类泛泛文字替代覆盖. 生命周期、锁、单位和特殊状态等信息不能因采用合并说明而遗漏.

| 语言或载体 | 注释格式与密度 |
| --- | --- |
| C / C++ | 公共声明使用一致的文档注释, 项目采用 Doxygen 时遵循其格式; 私有声明与变量可用相邻 `//`, 实现补充所有权、锁及提交边界 |
| Go | 导出声明使用与声明相邻、以符号名开头的文档注释; 包说明集中维护, 各文件职责及内部声明用相邻注释说明 |
| Rust | 公共项使用 `///`, 模块说明使用 `//!`; 私有项可用文档或普通注释, `unsafe` 边界说明安全前提 |
| Python | 模块、类及函数使用 docstring, 参数格式在项目内一致; 变量和实现说明使用 `#`, 不重复类型标注 |
| JavaScript / TypeScript | 公共接口按项目采用 JSDoc/TSDoc, TypeScript 不重复可直接读取的类型信息; 内部注释关注副作用、异步与生命周期 |
| HTML / CSS / 前端模板 | 使用对应载体的注释语法, 说明可访问性取舍、兼容处理和复杂样式约束; 脚本部分遵循所属语言 |
| Java / Kotlin / C# | 公共接口分别使用 Javadoc、KDoc 或 XML 文档注释; 私有声明与变量可用普通注释, 简单访问器可承接已有属性文档 |
| Lua | 使用 `--` 或项目已有文档格式; 公共模块说明参数、结果与宿主资源责任, 内部按复杂度补充 |
| SQL / Shell / PowerShell | 说明迁移影响、事务/重试边界、前置条件及外部副作用; 公共命令入口使用适用的帮助注释, 不逐行翻译命令 |
| 协议 / 配置 / 模板 | 采用载体支持的注释或 Schema 描述; 重点说明单位、默认值、缺失语义及兼容要求, 不向不支持注释的格式强行插入注释 |

文档注释的具体标签与排版遵循已有工具链, 不为统一外观新引入生成器. 同一契约只在权威声明处完整维护, 实现处补充原因与不变量.

枚举项的注释紧邻对应声明, 使用所属语言适用的文档注释、前置注释或行尾注释. 即使名称直观也保留简短的语义说明, 不只重复名称; 用于协议数值、数组下标、位标志、默认状态、未知值或保留值时说明相应约束. 不因格式或注释整理改变枚举数值或顺序. 注释不能与实现或正式契约相矛盾, 未来计划不写成已具备能力. 需长期保留的待办绑定具体缺口与责任入口; 不以注释掉的旧代码、会话过程或作者签名维护历史. 语言或密度调整不能丢失原有的有效契约信息, 也不触发无关文件的批量翻译.

排版由所属语言的已提交配置及现有格式化器控制. 缩进、括号、换行、导入排序和列宽不在多个文件重复定义. 格式化限制在授权范围内的手写文件, 不顺带重排无关代码、生成结果或第三方文件. 配置变更单独说明影响, 不为了当前一行代码关闭规则.

无法使用规定工具时如实保留未检查项, 不自动安装, 不声称手工目测等价于工具执行. 格式化不证明行为正确; 静态检查若会编译、运行代码或下载依赖, 仍按实际副作用核验授权.

## 8. 各语言编码规范

所有语言的通用编码正文集中在本节, 不另建平行的语言编码规范文件. 项目登记实际使用的语言、版本、工具配置、目录及第 1.1 节的明确差异. 通用规则与对应语言小节一起适用; 组合技术栈同时读取相关小节. 这里的默认写法不授权修改既有公共 API、升级工具链或改写第三方代码.

各语言均先遵循第 2 节的单个完整词优先原则. 下文的 `camelCase`、`PascalCase`、`snake_case` 和 `UPPER_SNAKE_CASE` 只规定大小写及不得不用多词时的连接形式, 不要求名称由多个词组成. 例如单词按角色写作 `entry`、`Entry` 或 `ENTRY`; 只有确需同时区分读写期限时, 才按语言选用 `readTimeout`、`ReadTimeout` 或 `read_timeout`.

| 语言或载体 | 章节 |
| --- | --- |
| C++ / C | [C++](#cpp-cn) / [C](#c-cn) |
| Go / Rust / Python | [Go](#go-cn) / [Rust](#rust-cn) / [Python](#python-cn) |
| JavaScript / TypeScript / 前端 | [JS/TS](#typescript-cn) / [HTML、CSS 与组件](#web-cn) |
| Java / Kotlin / C# / Lua | [Java/Kotlin](#jvm-cn) / [C#](#csharp-cn) / [Lua](#lua-cn) |
| SQL / Shell / PowerShell | [SQL](#sql-cn) / [脚本](#shell-cn) |
| 协议 / 配置 / 模板 | [协议与配置](#protocol-cn) |

未列出的语言先适用通用规则; 首次引入时在本节补齐命名、可见性、模块组织、错误、资源及格式约定, 然后由项目登记工具与支持边界. 不声称有限列举已经穷尽所有语言. 非 C++ 默认以 160 列作为可配置排版目标, 具体输出遵循已有工具配置; 不用行宽破坏字面量、生成语义或外部固定格式. C++ 长行遵循自己的明确要求.

<a id="cpp-cn"></a>

### 8.1 C++

以下规定 C++ 的作用域、实现、注释、长行和逻辑块空行. 采用范围包含手写测试和模板; 实际语言标准及兼容下限仍由项目声明, 不因本节列出新特性而自动升级.

#### 8.1.1 适用范围

- 所有手写 C++ 代码均须遵循, 包括生产代码、测试代码、模板、头文件及实现文件.
- 生成代码和第三方代码不属于本规范的整理范围, 不允许为落实本规范修改它们, 包括重命名、补注释和重新排版.
- 不将本规范机械套用到其他语言, 也不据此恢复已冻结组件的开发.

#### 8.1.2 命名适配

- 极简命名、共识缩写和复合名称的使用条件统一遵循第 2 节, 不作为 C++ 专属规则另行定义.
- 利用真实类作用域表达归属, 例如 `Store::Entry`; 不重复类名, 不为缩短名称增加空壳层级. 类型使用 `PascalCase`, 函数和变量使用小写开头; 不得不用多词时分别使用 `EntryState`、`readTimeout` 这样的形式, 不采用下划线连接多个词.
- 私有成员变量使用尾下划线, 例如 `timeout_`; 参数和局部变量不加该标记, 例如 `timeout`. 不为区分同名成员与参数另加无语义前缀.
- 属性访问器采用同名重载, 例如 `timeout()` 读取、`timeout(value)` 设置. 只按真实接口需要提供读写操作, 不为每个成员自动生成 setter; 非属性操作仍按实际行为命名.
- 枚举项使用 `PascalCase`, 例如 `State::Ready`; 不重复枚举类型名称. 必要的多词枚举项同样使用大写开头的驼峰形式, 已固定的外部协议或生成符号保持原有身份.
- 自有常量与变量采用相同命名形式, 例如 `limit`、必要时的 `readLimit`, 不加 `k` 前缀, 不仅因是常量就改成全大写. 宏及外部固定符号不据此改写.
- namespace 优先使用小写完整单词, 通过有实际职责的嵌套层次表达归属, 尽量避免复合名称. 确实不能按职责拆分而又必须多词时使用小写开头的驼峰, 不为缩短名称制造无意义层级.
- 自有 C++ 头文件和实现文件分别使用 `.hpp`、`.cpp`, 文件名使用小写完整单词; 不得不用多词时使用下划线连接, 例如 `read_timeout.hpp`. 文件路径与符号命名分别适配, 不因类型首字母大写就将文件名改成大写.

#### 8.1.3 作用域收敛

- 定义放在最窄且符合真实职责的作用域. 仅一个类使用的类型、枚举、常量和辅助函数, 优先定义在该类内部.
- 类内定义默认优先 `private`, 只有真实的外部调用契约需要时才公开. 嵌套不等于公开, 不为测试方便扩大生产 API.
- 类声明依次组织 `public`、`protected`、`private` 区域, 没有内容的区域省略. 每个区域内按职责将相关类型、操作和数据相邻组织, 不机械按符号种类拆散关联; 声明顺序不改变成员应有的可见性.
- 公开的数据记录使用 `struct`; 封装不变量、管理资源与行为的类型使用 `class`. 不仅按成员数量或是否存在方法决定, 数据记录也可包含与其语义一致的辅助操作.
- 有明确类归属、又不依赖实例状态的操作, 优先使用该类的静态成员函数, 而不是 namespace 中的公共函数.
- 不为收纳任意辅助函数建立无实际职责的空壳类. 跨独立组件共用的概念, 或必须位于 namespace 的接口, 按实际归属保留.
- `.cpp` 内仅供该翻译单元使用的实现细节, 优先使用匿名 namespace 限制外部链接和可见范围. 它不是清空符号表的机制.
- 不机械在头文件中使用匿名 namespace, 避免每个翻译单元形成不同的类型或状态副本. 优先用类内私有定义及符合头文件语义的实现组织.
- 目标标准支持时, 嵌套 namespace 使用 `namespace astra::detail {}` 这样的紧凑声明; 语法紧凑不改变实际作用域层次.

#### 8.1.4 性能、缓存与内存布局

- 正确性、生命周期和并发安全是前提. 不以性能或代码精简为由破坏既有不变量、对象有效期或线程同步关系.
- 以极高效率为目标, 关注热路径的分配、复制、遍历、间接访问、锁竞争和工作集大小. 精简代码不能引入隐藏复制、重复遍历或额外分配.
- 重视缓存命中和访问局部性, 按实际访问模式安排数据、冷热字段及所有权. 不仅关注单个对象的大小, 还要考虑批量对象和并发访问的效果.
- 内存布局不能只追求最小 `sizeof`. 对齐、填充、缓存行共享和伪共享均属于取舍依据; 不为省少量空间引入不合适的非对齐访问或竞争.
- 为性能增加复杂度, 必须有明确的热点依据和收益说明. 区分静态推断与已经测量的结果, 不把理论优势写成实测结论.
- 性能验证仍遵循本轮测试授权规则, 不能以基准测试或优化验证为名绕过 项目授权约定.

#### 8.1.5 现代语法、精简与优雅

- 优先考虑当前目标工具链实际支持的 C++26、C++23、C++20 特性和标准库能力.
- 现代语法能提供更高效或更精简的实现时, 应当优先使用, 同时满足前述正确性和性能约束.
- 优先用语言和标准库直接表达所有权、约束、资源管理和编译期工作, 减少重复实现及无收益的中间层.
- 不把新语法本身视为成果, 不用复杂模板、宏或抽象将简单逻辑隐藏起来.
- 在性能和代码量相同的情况下, 以优雅性为先: 意图直接、表达自然、前后一致, 重要的不变量和生命周期容易看见.
- 精简指减少真实冗余和复杂度, 不以删除必要的校验、注释或测试来减少行数.
- 初始化表达式能清楚体现类型, 或使用迭代器、复杂模板类型时采用 `auto`; 类型本身传达重要语义而无法从初始化处直接看出时显式写出. 引用与 `const` 按真实语义声明, 不因类型推导隐藏复制或借用关系.
- 初始化后不再赋值的局部变量默认加 `const`; 对象需要修改或后续需要从中移动时例外. `const` 不代表深层对象不可变或天然线程安全, 不为表面统一引入额外复制.
- `const` 默认放在所限定类型之前, 例如 `const Entry&`、`const auto`. 指针本身不可重新赋值时仍按语义写为 `Entry* const`, 不为形式统一改变所限定的对象.
- 普通值初始化优先使用 `=`, 聚合与零初始化使用 `{}`; 带构造参数时按实际语义选择 `()` 或 `{}`. 注意列表初始化对重载选择、窄化及 `auto` 推导的影响, 不机械替换初始化语法.
- 普通函数使用前置返回类型, 例如 `Result read()`; 返回类型依赖参数等适合尾置表达的场景使用 `auto read(...) -> Result`, 不为形式统一将所有函数改为尾置返回.
- lambda 优先显式列出捕获, 使借用、复制和所有权移交可见. 捕获的寿命须覆盖执行期, 尤其是逃逸与异步任务; 显式捕获本身不构成生命周期安全证明.
- 可形成隐式转换的构造函数默认使用 `explicit`; 只有存在明确、合理的隐式转换语义时例外, 不为少写一次构造调用开放隐式转换.

#### 8.1.6 注释及信息完整度

- 使用第 7 节的英文注释、对象覆盖要求与 C++ 文档格式. 文件头说明职责, 类、函数及变量均有对应说明; 公共契约优先放在声明处, 实现处说明原因和不变量, 不重复整段接口文档.
- 类型、函数及成员的声明级注释统一使用前置 `///`, 枚举项也使用对应的声明级注释; 函数实现内部的局部变量和逻辑说明使用 `//`. 声明级格式同样适用于私有声明, 不因可见性不同混用块注释形式.
- 按实际接口说明无法从签名直接得出的参数单位、有效范围、返回值、失败方式、所有权、生命周期和线程约束. 特殊成员、模板参数及约束存在非显然语义时同样说明.
- 默认值与初始值分开说明, 不编造不适用的范围或保证. 简单局部变量、循环变量、结构化绑定和捕获可由相邻代码块说明共同覆盖, 用途必须明确; 借用、捕获寿命或特殊值的必要约束同时说明.
- 内部注释重点解释资源交接、锁边界、提交顺序、回退与清理等不易从语句看出的约束. 普通控制流不为每个阶段机械添加说明; 枚举类型及每个枚举项按第 7 节分别说明语义与适用约束.
- 注释允许分行, 不受代码表达式保持单行的要求限制. 保留已有有效契约信息, 不把降低重复注释理解为删除必要保证.

#### 8.1.7 长行及换行

- 缩进使用 4 个空格. 指针与引用符号贴类型, 例如 `Entry* entry`、`Entry& entry`; 不通过排版掩盖指针本身与所指对象的 `const` 差异.
- `if`、`else`、`for`、`while` 和 `do` 的语句体保留大括号, 即使只有一条语句也不省略.
- 允许超长行, 不以固定列宽强制折行.
- 函数声明和定义头保持完整单行, 包括返回类型、函数名、参数及限定部分; 定义的左花括号与函数头位于同一行.
- 普通语句、调用、参数列表和表达式不在中途因行宽折行. 函数体、lambda 体和类定义等结构正常分行, 不将整个函数体压成一行.
- `enum` 和 `enum class` 的首个元素必须从左花括号后的下一行开始; 结束的 `}` 必须单独起一行, 不得跟在枚举元素之后. 即使枚举只有一个元素, 也遵循此规则.
- 连缀调用如果不是极长, 保持单行. 极长到明显妨碍辨认链条步骤时, 允许沿调用链边界换行; 不仅因为超过旧列宽就拆开.
- 条件表达式过于复杂、存在多层嵌套时, 允许按逻辑分组换行, 保持括号、优先级和求值关系清晰. 简单条件保持单行.
- 不用拆行数量或固定字符数判断复杂程度, 不通过过度紧缩表达式来规避可读性问题.

#### 8.1.8 代码块与空行

- 这里的代码块主要指函数内的逻辑阶段, 例如检查输入、准备资源、执行逻辑、分支处理、提交状态、通知和清理. 每个阶段可能包含数行, 不单纯按大括号数量计数.
- 属于同一阶段的变量声明、注释和语句放在一起; 不因每个变量或每条语句独立成行就把它们各自视为一个代码块.
- 代码块之间必须有空行, 通常保留一行, 使阶段边界清晰.
- 函数拥有不止一个逻辑代码块时, 函数体左花括号之后的第一行必须为空行, 然后才写第一块的注释或语句.
- 极简单块函数不强制在函数体开头留空行. 多阶段函数不因整体行数短而免除开头空行.
- 分支结构及其内部阶段按真实逻辑划分, 不机械在每一对大括号之间添加空行. 注释与所属代码块保持相邻, 空行放在前一个代码块和本块注释之间.

#### 8.1.9 头文件实现与访问器返回

- 头文件默认使用 `#pragma once` 防止重复包含; 目标工具链不支持时改用宏保护, 并由项目登记兼容差异. 不为同一目的同时重复使用两套保护.
- `.cpp` 首先包含自身对应的头文件 (若有), 再依次按项目头文件、第三方头文件、标准库头文件分组, 组间空行、组内按包含路径排序. 头文件自身直接声明所需依赖, 不依赖调用方的包含顺序或偶然的间接包含; 有顺序要求的外部头文件按真实约束处理.
- 前置声明用于减少不必要的头文件依赖, 前提是不增加脆弱性且当前使用不要求完整类型. 需要完整类型时直接包含定义, 不自行猜测标准库或第三方类型声明; 只在实现使用的依赖尽量放入 `.cpp`.
- 模板及需要调用方可见定义的 `constexpr` 等按语言要求组织. 除此之外, 简单 getter、setter 和单步转发允许直接在 `.hpp` 中实现; 复杂逻辑放在 `.cpp`, 不因行数少就将包含复杂校验、同步或状态转换的函数视为简单访问器.
- 普通非模板函数放入头文件时满足单一定义规则, 按定义位置采用适用的内联方式. 定义在头文件不等于编译器必然执行调用内联, 不据此声称性能收益.
- getter 对标量和小值类型默认按值返回; 较大成员可返回只读引用或视图以避免无谓复制. 接口须说明借用的有效期和失效条件, 只读不代表底层状态不会变化或天然线程安全.
- 返回引用或视图时必须能满足调用方的生命周期与同步契约; 不能返回临时对象的引用, 也不能把仅在内部锁持有期间有效的数据当作离开函数后仍安全的只读结果. 无法提供有效借用保证时使用独立值或受控快照等合适返回方式.

#### 8.1.10 错误结果与返回值检查

- 目标语言版本及标准库支持时, 可恢复失败优先通过 `std::expected` 表达, 错误类型说明稳定分类与必要上下文. 不为采用该形式隐式升级工具链或引入第三方依赖; 兼容目标沿用项目明确的结果类型约定.
- 异常用于无法正常返回结果的路径及第三方边界, 接口明确哪些异常可能传播、在哪里转换或终止以及失败后的状态保证. 使用 `std::expected` 不自动意味着无异常或可标记 `noexcept`; 不用捕获全部异常并返回普通错误掩盖内部不变量损坏.
- 错误结果、资源获取以及忽略后容易造成误用的返回值必须用 `[[nodiscard]]` 标记, 可在适当的结果类型或函数上表达. 普通查询按实际误用风险选择, 不要求所有非 `void` 接口机械添加.
- 确有依据忽略上述结果时显式表达并保留必要原因, 不通过统一丢弃、无意义强转或诊断抑制绕过错误处理与资源责任.

<a id="c-cn"></a>

### 8.2 C

- 函数、变量和自有文件默认使用 `snake_case`, 宏使用 `UPPER_SNAKE_CASE`; 对外符号带必要的模块前缀. 不自造保留标识符, 不把 C++ 类作用域习惯照搬为全局短名.
- 自有类型名称使用小写单词, 必要多词使用下划线连接; 结构类型保留显式标签, 调用处使用 `struct entry*` 等形式, 不默认用 `typedef` 隐藏结构标签或指针所有权.
- 公开头文件只包含调用所需声明, 内部结构可使用不透明类型. 单文件私有函数和状态限制链接可见性; 头文件保护、包含关系和 C/C++ 互操作按实际支持方式固定.
- 头文件默认使用 `#pragma once`; 目标工具链不支持时采用宏保护, 由项目登记兼容差异.
- 指针、长度、容量、编码和所有权成组说明, 明确谁分配、谁释放及失败时输出参数的状态. 不依赖无界字符串操作, 不把指针强转当作已满足对齐、别名和对象表示要求.
- 错误返回与有效结果分离. 资源取得后保证每条退出路径配对释放; 可以用职责明确的统一清理出口, 不在失败分支遗漏部分初始化资源.
- 位操作、整数转换和缓冲区算术核验符号与边界. `volatile` 不作为线程同步的替代. 宏避免参数重复求值和隐藏控制流, 能用函数或常量表达时优先使用它们.
- 排版使用项目固定的现有格式化器. C++ 的函数头单行和函数体首空行约定不自动扩展为 C 的规则.

<a id="go-cn"></a>

### 8.3 Go

- 包名简短、小写并表达职责; 导出名使用 `PascalCase`, 非导出名使用 `camelCase`. 共识缩写保持 `ID`、`URL`、`HTTP` 等形式, 例如 `userID`, 不改成 `userId`; 不用 `Get`、包名前缀或空壳接收者重复已有语义.
- 方法接收者使用准确的完整单词, 例如 `(store *Store)`, 同一类型保持一致; 不用单字母截短名称代替职责表达.
- 文件名优先小写单词, 不得不用多词时直接连接, 例如 `readtimeout.go`. 平台、构建与测试使用 Go 要求的后缀, 不把必要后缀中的下划线去掉.
- 包的主要类型需要构造函数时使用 `New`, 其他类型按需使用 `NewEntry` 等形式. 不仅为命名一致而给可直接有效使用的零值强制添加构造函数.
- 使用 `gofmt` 排版, 不以手工列对齐覆盖其结果. 文件围绕包内职责组织, 测试按语言发现约定命名; 构建约束和平台文件归属明确.
- import 依次按标准库、第三方、项目内部分组, 组间空行, 组内使用现有工具排序; 不为排列导入自动安装工具.
- 公共行为优先在外部测试包验证, 内部不变量测试使用同包测试. 不为满足外部测试形式公开生产内部符号, 不把测试包选择当作缩小验证范围的理由.
- 接口围绕使用方需要的行为保持最小, 不为每个具体类型建立镜像接口. 值接收者与指针接收者依据复制成本、可变性和语义选择; 含锁等不可复制状态不得随意按值传播.
- 可恢复失败通过 `error` 表达, 增加上下文时保留需要识别的原因. 不以错误字符串比较代替稳定分类, 不用 panic/recover 隐藏外部输入和正常失败.
- 需要取消的操作显式接收并传递 Context; 请求路径不随意换成无关背景上下文. goroutine、channel、timer 和连接有明确的停止与释放责任, 不用新 goroutine 掩盖阻塞和错误.
- 对外暴露的 slice、map、指针和接口值说明可变性与共享关系; 返回集合不自动意味着调用方得到独立副本. 不把接口包含带类型空指针的情况当作必然等同于空接口.
- 纯数据包不在初始化时隐式发起网络、派生任务或修改进程环境. 导出声明说明行为与失败约束, 复杂内部阶段按第 7 节说明.

<a id="rust-cn"></a>

### 8.4 Rust

- 模块、函数、变量和文件默认 `snake_case`, 类型与 trait 使用 `PascalCase`, 常量和静态项使用 `UPPER_SNAKE_CASE`; 排版使用 `rustfmt`, 诊断采用项目已配置的检查入口.
- 无子模块时优先使用 `entry.rs`, 存在子模块时优先使用 `entry/mod.rs` 作为父模块入口; 不同时声明互相冲突的两种入口.
- 常规构造操作统一可命名为 `new`, 是否可能失败由 `Self` 或 `Result<Self, Error>` 等返回类型明确表达, 不强制为可失败构造添加 `try_` 前缀.
- 简单读取访问器使用 `timeout()` 等属性名称; 修改操作按具体行为命名, 不设统一 setter 形式, 不为所有字段自动提供修改接口.
- 导入依次按标准库、第三方、当前 crate 分组, 组间空行, 组内使用现有工具排序.
- 默认私有, 按真实使用面选择局部或 crate 可见性. trait、泛型和宏用于实际约束与复用, 不以宏隐藏普通控制流或构造只有一个透传实现的框架.
- 可恢复错误使用 `Result`, 缺失使用 `Option`. 不对外部输入或正常 I/O 失败使用 `unwrap/expect`; 内部不变量失效遵循开发规范的故障隔离要求.
- 库与领域层使用明确的错误类型, 应用边缘可按实际需要聚合错误并保留原因; 该选择不授权引入任何错误处理依赖.
- 优先借用和明确所有权, 不用无依据的 `clone`、泄漏或无限共享延长寿命来消除编译错误. `Arc` 只表达共享所有权, 不代替内部同步设计.
- `unsafe` 封装在最小边界, 说明调用者及实现各自维持的安全前提. 原始指针、跨语言接口、别名和释放责任必须对应具体约束, 不能只写“为了性能”.
- 持锁期间的异步等待、future 取消与析构职责显式说明. 不假定丢弃任务句柄就已完成任务排干, 不让析构遗漏必须异步完成的外部清理.

<a id="python-cn"></a>

### 8.5 Python

- 模块、函数和变量使用 `snake_case`, 类使用 `PascalCase`, 常量使用 `UPPER_SNAKE_CASE`; 导入按包边界组织, 不用通配符导入或修改全局搜索路径掩盖依赖.
- 枚举项与模块常量使用全大写单词, 例如 `READY`、`LIMIT`; 不得不用多词时用下划线连接, 例如 `READ_TIMEOUT`. 枚举类型本身仍使用 `PascalCase`.
- docstring 使用 Google 风格, 按实际需要组织 `Args`、`Returns`、`Raises` 等部分; 简单说明不填充空章节, 不重复类型标注或编造错误保证.
- 包内默认使用绝对导入, 仅在少量有明确需要的场景使用显式相对导入; 导入路径与真实包边界一致.
- 新代码的文件路径处理默认使用 `pathlib.Path`, 外部接口需要其他表示时在边界转换. 不为采用该形式改写无关的既有路径代码.
- 简单数据记录优先使用 `dataclass`, 是否不可变按实际语义决定; 封装复杂行为或不变量时按职责选择合适类型, 不机械将所有类改为数据类.
- 配置项、布尔开关及容易混淆的多个同类型参数优先使用仅关键字参数. 已发布接口的调用兼容性仍须遵守, 不直接破坏原有位置参数调用.
- 使用项目现有 Black 配置, 修改后立即格式化并做静态语法核对. 函数和公共数据边界有类型标注; 动态外部输入仍做运行时校验, 不把类型标注当作执行保护.
- 不使用可变默认参数承载跨调用状态; 明确 `None`、空容器和假值的差异. 对任意对象序列化、路径和子进程输入明确类型与边界, 不通过字符串化消除歧义.
- 使用上下文管理或明确的 `try/finally` 释放资源. 只捕获能够处理或转换的异常; 保留异常原因, 不用空 `except`、默认成功或吞掉取消掩盖失败.
- 异步任务、线程和子进程归属明确. 取消后等待完成及清理; 不以后台任务或同步阻塞调用绕过生命周期约束.
- 导入模块不执行维护操作、网络连接或测试. 可执行入口与可复用计算分离, shell 参数与协议字节按原始语义传递, 不吞掉失败、跳过或清理错误.

<a id="typescript-cn"></a>

### 8.6 JavaScript / TypeScript

- 函数、变量和字段使用 `camelCase`, 类型和类使用 `PascalCase`; 不为接口统一添加无语义的前缀. 常量同样使用 `camelCase`, 不因模块级固定值而改用全大写.
- 普通模块文件名使用小写单词, 不得不用多词时使用小写开头的驼峰, 例如 `readTimeout.ts`; 组件文件按组件约定命名.
- TypeScript 对象契约优先使用 `interface`, 联合、映射及组合类型使用 `type`. 不为统一形式改写外部类型或借声明合并隐藏接口变化.
- 模块级具名操作优先使用 `function`, 回调使用箭头函数. TypeScript 的所有具名函数均显式标注返回类型, 包括具名方法及绑定到名称的箭头函数; 匿名回调按上下文推导. 普通 JavaScript 不插入 TypeScript 语法, 按其文档与检查方式表达契约.
- 默认使用 `const`, 需要重新赋值时使用 `let`; 避免依赖隐式类型转换和全局状态. 明确 `null`、`undefined`、空字符串和零的区别, 不以真假判断覆盖所有缺失语义.
- 自有可选值默认用 `undefined` 表示缺失, `null` 留给明确的业务状态或外部协议; 不为内部偏好改写协议的空值语义.
- TypeScript 的有限状态集合优先使用 `enum`, 每个枚举项按第 7 节分别说明. 需要仅可擦除语法或其他运行环境限制的项目须按第 1.1 节登记兼容差异, 不为采用 `enum` 静默关闭编译检查或更换执行方式.
- 类私有字段默认采用 TypeScript 的 `private field`, 不默认改成 `#field`; 类型层面的访问限制不冒充运行时安全隔离. 普通 JavaScript 按实际封装能力处理, 不使用 TypeScript 专属修饰符.
- 自有模块优先具名导出, 框架规定的默认导出保留. 类型导入与运行时导入区分, 模块解析、文件扩展名及浏览器/服务端目标按项目配置固定.
- TypeScript 启用适用的严格类型检查, 包括空值、索引和可选字段边界. 不以 `any`、非空断言、类型强转或忽略注释隐藏未确认状态; 外部数据先作为未知输入校验再进入领域类型.
- Promise 的失败必须被调用方或明确的任务所有者观察; 避免未等待的异步回调和隐藏的 fire-and-forget. 取消、过期响应与组件销毁分别处理, 不把发出取消信号当作任务已结束.
- 对外数据默认表达只读意图, 共享引用及运行时可变性仍须说明. 不假定类型层面的 `readonly` 已冻结深层对象, 不用对象引用相等代替业务身份.
- 语句保留分号, 普通字符串默认使用单引号, 插值使用模板字符串. 多行结构在目标语法允许的位置保留尾逗号, 包括适用的参数列表; 外部固定格式按其语法处理.
- 单元及跨模块测试统一放入独立 `tests/` 目录, 按源码职责组织并纳入实际测试发现入口. 不因目录集中而开放生产私有接口, 不在规范整理时自动迁移已有测试.
- 排版使用既定 Prettier 或项目已有等价配置, 编译和静态检查不放宽诊断来制造通过. 业务库导入不隐式启动服务器或修改宿主环境.

<a id="web-cn"></a>

### 8.7 HTML、CSS 与前端组件

- HTML 使用符合语义的元素, 表单控件与名称、键盘操作及焦点行为明确. 不用纯视觉表现代替可访问名称, 不通过直接拼接不可信 HTML 插入内容.
- CSS 命名表达组件与状态, 公共设计值由统一变量或令牌维护. 控制全局样式范围、选择器耦合与层叠关系, 不靠持续叠加高优先级或 `!important` 修补结构问题.
- CSS 类名与样式隔离沿用所属公共组件或组件库已确认的惯例, 包括其命名空间、状态表示及对外样式接口, 不全局强制采用 BEM. 新增组件先明确归属并与同类组件一致, 不因个人偏好更换命名体系; 没有现成约定时由项目明确, 不借此引入组件库.
- 组件、事件、输入与输出具有明确契约, 组件名默认 `PascalCase`. 状态按所有权划分, 不无依据复制可推导状态或以数组位置作为可变实体的稳定身份.
- 组件文件名跟随组件名称, 例如 `Entry.vue`、`Entry.tsx`; 普通模块仍遵循 JS/TS 文件命名. 事件处理函数采用 `on` 前缀, 例如 `onClick`、`onChange`, 实际业务操作仍按职责命名.
- 使用 Vue 时优先 Composition API、显式 props/emits 和 TypeScript 单文件组件; composable 使用 `use` 前缀. 其他框架保持自身惯用生命周期, 不要求全部项目迁移到 Vue.
- Vue 单文件组件按 `script`、`template`、`style` 排列, 不需要的部分省略.
- 监听器、观察器、定时器、动画帧、网络任务与 GPU 资源随所属生命周期回收. 高频计算和外部可变引擎与界面响应式状态有清晰边界, 避免深层代理造成隐式更新与所有权混乱.
- 业务状态、渲染和数据适配分工明确. 具体框架、目录、样式隔离方式及构建目标由项目登记, 不把单个应用的目录名称固定为所有前端的结构.

<a id="jvm-cn"></a>

### 8.8 Java / Kotlin

- 包名小写, 类型使用 `PascalCase`, 方法和字段使用 `camelCase`, 常量使用 `UPPER_SNAKE_CASE`. 可见性按真实契约收窄, 不把公共 setter 和可变集合当作默认 API.
- Java 对空值边界明确约定, Kotlin 优先不可变 `val` 与可空类型表达; 不以无依据的非空断言绕过校验. 跨 Java/Kotlin 及反射边界的输入仍需核验.
- 异常与错误结果沿调用边界一致表达, 保留原因及中断/取消语义. 不捕获所有异常后返回成功默认值, 不把受损状态继续交给调用方.
- 文件、连接及其他可关闭资源用结构化作用域释放; 垃圾回收不代替关闭. 线程、executor、协程 scope 和任务有明确所有者, 不使用无归属全局任务掩盖生命周期.
- 集合只读接口与底层可变性区分, 并发访问按真实同步机制处理. 不为相似方法建立深继承树, 不用注解或反射隐藏关键提交和资源责任.
- 格式与静态诊断由已有项目工具固定, 平台版本和语言互操作范围另行登记.

<a id="csharp-cn"></a>

### 8.9 C#

- 类型、方法和属性使用 `PascalCase`, 参数和局部变量使用 `camelCase`; 接口沿语言惯例使用 `I` 前缀, 可等待的异步方法通常使用 `Async` 后缀. 私有实例字段使用尾下划线, 例如 `entry_`.
- 命名空间统一使用大括号块, 不默认采用文件作用域形式. 局部变量在初始化处类型明显时使用 `var`, 否则显式写出类型.
- 数据模型默认使用普通类, 根据实际值相等、复制及不可变需求逐项决定是否使用 `record`, 不将所有数据类机械改为记录类型.
- 在目标支持范围内使用可空引用分析, 对外输入仍需校验. 不用空值抑制运算符消除未证明的风险; 值类型、引用类型和可变集合的复制语义明确.
- 可释放资源采用 `using` 或适用的异步释放作用域. 原生句柄和跨语言缓冲明确所有权, 不依赖终结器保证及时释放或业务提交.
- Task 的错误与完成由调用方观察, 显式传递 CancellationToken. 除框架要求的事件边界外不使用 `async void`; 不以同步阻塞等待替代正确的异步调用链.
- 异常转换保留原因, 取消与普通失败区分. 锁内不调用未知回调, UI 线程约束与上下文切换不能被后台任务隐式破坏.
- 命名空间、源码和测试项目按职责组织, 格式与分析器由已有项目配置控制. ABI、封送和原生依赖属于显式接口契约.

<a id="lua-cn"></a>

### 8.10 Lua

- 自有函数、变量和文件默认 `snake_case`; 局部状态使用 `local`, 模块显式返回公共接口. 不通过隐式全局变量共享业务状态.
- 缩进使用 4 个空格. 可恢复失败默认返回 `nil, error`, 明确成功结果与失败结果的区分; 程序错误及宿主要求的异常边界按实际契约处理, 不把所有错误一律降为普通失败返回.
- 明确 `nil`、`false`、空 table 和数值零的语义. 列表、映射和含空洞 table 不混用隐含长度假设, 不依赖未约定的遍历次序.
- table 是共享引用, 对外只读视图和拷贝语义必须明确. 元表和动态分派用于实际需求, 不隐藏资源所有权和错误边界.
- 对外错误约定固定, `pcall/xpcall` 只用于有明确处理能力的边界; 不吞掉失败或把协程暂停当作任务已经结束.
- 文件、连接、原生 userdata 和协程按实际运行时能力清理. 语言版本、宿主沙箱和数值表示由项目声明, 不假定所有 Lua 环境具有相同标准库或整数行为.

<a id="sql-cn"></a>

### 8.11 SQL 与数据库迁移

- 自有表、列和约束默认 `snake_case`, SQL 关键字使用大写, 例如 `SELECT`、`FROM`. 新表名使用复数, 例如 `entries`; 不为统一单双数重命名已发布 Schema. 方言、标识符大小写及引用规则遵循目标数据库.
- 方言支持的位置显式使用 `AS` 声明别名, 包括适用的表别名和列别名, 不为形式统一写出目标方言不接受的语法.
- 数据值通过参数绑定传递, 动态标识符须来自明确允许集合并按方言正确引用. 不拼接外部文本生成可执行 SQL.
- 查询显式选择需要的列, 对需要稳定次序或分页的结果声明排序和并列处理. 避免在稳定接口中依赖 `SELECT *` 或数据库偶然返回次序.
- 事务、约束和应用校验共同维护承诺, 失败路径明确回滚及结果未知的处理. 批量修改核对目标范围, 不以捕获错误后继续执行掩盖部分成功.
- 迁移脚本按固定顺序与内容身份管理; 已部署迁移不原地改写. 明确兼容期、锁和运行成本、可回退点及不可逆边界, 不因脚本放在仓库中就自动执行.

<a id="shell-cn"></a>

### 8.12 Shell / PowerShell

- 脚本声明适用的解释器和平台, 不混淆 POSIX shell、Bash 与 PowerShell 的语法及退出规则. 自有函数和变量按该环境惯例统一, 不复用系统保留变量承载任务状态.
- PowerShell 内部函数使用简短、完整的职责名称, 对外命令使用动词—名词形式并优先采用批准动词. Shell 自有函数使用小写单词, 不得不用多词时使用小写开头的驼峰.
- 自有 CLI 以可读长选项为主, 常用项可提供明确的短选项; 各解释器或参数解析器使用其实际支持的选项语法, 不强制不同平台采用同一前缀形式. 别名具有相同语义并在帮助中说明, 不改写外部工具的固定参数.
- 参数优先按数组或字面路径传递, 明确通配符、空格、编码和标准输入语义. 不对外部输入使用 eval 或字符串拼接执行, 不把 JSON 转义当作 shell 转义.
- 检查实际命令退出码及管道各阶段的失败, 不只依赖全局严格模式; 输出解析失败也必须传播. 日志不得打印凭据或含秘密的完整命令.
- 临时文件、目录和后代进程归属明确. 退出和取消走可核验的清理路径; 删除或移动先确认解析后的目标边界, 不跨 shell 拼接破坏性文件操作.
- 可复用逻辑与命令入口分离. 脚本不修改用户或系统环境来解决本次路径问题, 不隐式下载依赖, 不把一次性操作变成导入或启动副作用.

<a id="protocol-cn"></a>

### 8.13 协议、配置与生成模板

- 字段名称、类型、单位、默认值、缺失值与未知值策略显式定义. Schema 是结构来源, 产品契约补充跨字段语义, 不维护互相矛盾的手写结构副本.
- 协议的字段编号、枚举值和保留项遵循实际兼容规则; 不复用已保留的字段身份. JSON、YAML、TOML 等配置拒绝含糊解释, 解析器能力与重复字段策略明确.
- 生成模板按生产代码的安全与编码要求维护; 模板输入与输出编码按各自上下文处理, 不通过拼接未转义文本生成源码、SQL、HTML 或 shell.
- 输入、生成器和输出的对应关系可追溯, 生成结果不手改. 项目决定是否跟踪产物, 不能因文件自动生成就排除其消费验证.
- 配置不包含真实秘密, 示例值清楚标明用途; 缺少必需配置明确报错, 不静默切换到宽松权限、外部服务或生产环境.

## 9. 文档与设计归属

README 负责用途、导航和必要的开始方式; 开发规范负责流程与验收; 本文负责编码和组织, 各语言具体写法集中在第 8 节; 功能文档负责该功能的完整设计; 项目配置负责可执行参数; 验证记录负责实际证据及边界. 同一命令或阈值从唯一配置引用或生成, 避免手工维护多份.

新功能及公共行为变化遵循开发规范第 2.4 节: 明确功能文档, 完成关键细节与边界决策, 获准后实施. 本文的目录示例不构成已批准功能设计, 文档中的命令也不构成执行授权.

保留当前有效文档和必要兼容资料, 过期过程通过版本控制查询. 不能把已发布接口说明、迁移指南、法律文本或历史协议样本当作无效 Markdown 批量删除. 移动文件时同步导航和引用, 对外链接若属于兼容承诺须有迁移安排.

## 10. 整理、迁移与清理

组织整理以明确的使用者和目标为边界. 移动、重命名或删除前检查构建入口、导入、包发现、测试发现、生成器、资源加载、安装清单、文档及外部消费路径. 必须区分纯路径整理和行为/发布变化, 不把后者藏在“大扫除”中.

死代码、旧开关和临时实现经过引用与行为确认后移除; 动态加载、平台专用文件和反向兼容路径不能只凭文本搜索无引用判定无用. 不保留 `old`、`backup` 等源码副本代替版本控制.

缓存可再生不等于当前可删除. 清理前核验路径和自有资源, 保留正在使用的输出、原始证据、用户文件、兼容夹具与许可. 忽略规则只控制跟踪范围, 不证明内容可删除, 也不代替发布清单或秘密检查.

不要仅为统一风格全仓库重命名、转换文件或升级依赖. 整理必须保留用户现有改动, 验证与提交权限继续遵循开发规范和项目约定.

## 11. 规则如何进入自动检查

项目登记本文的内容身份及适用语言章节, 将实际要求映射到现有配置与检查入口. 不因本文件新增而直接修改既有 Schema、虚构新字段或宣称门禁已经支持; 所需适配作为明确的项目接入工作.

| 要求 | 合适的落实方式 |
| --- | --- |
| 排版、命名形式、导入和语言诊断 | 已配置的格式化器、编译器或静态检查, 明确工具版本和文件范围 |
| 模块公开面及依赖方向 | 语言可见性、构建目标依赖及适用的边界检查 |
| 测试、示例、生成和发布文件的归属 | 实际发现清单、构建/打包输入及生成一致性检查 |
| 语义命名、职责、抽象及复杂度取舍 | 设计与契约依据、相关反例及必要的定向审查; 不伪造静态工具能理解全部设计 |
| 规则抑制、排除和迁移 | 版本化配置与精确范围, 按开发规范核验保护变化及例外 |

项目可以复用开发规范中的 DEV-CONFIG、DEV-CORE、DEV-TRACE、DEV-ORACLE、DEV-IMPACT 等规则索引关联具体义务. 只写规则 ID 不算执行, 也不因此改写这些 ID 的既有含义; 必要扩展按项目命名空间正式登记.

检查范围须覆盖受维护的手写生产代码、测试和工具; 生成/第三方等排除项有明确来源, 不借移动目录逃避检查. 不设脱离职责的统一行数、文件数或目录深度门槛, 不为满足指标制造空包装.

文档准备的完成条件是: 两份通用规范职责清楚, 语言与项目登记内容明确, 引用一致, 没有把尚未存在的工具能力写成事实. 项目接入及发布达标另按开发规范验收. 后续仅在出现无法由现有规则表达的实际缺口时修改通用层, 已有规则能覆盖的问题落实为项目配置、实现或反例.
