---
name: basalt-code-review
description: Pre-commit review checklist for Basalt changes. Use on the staged diff before every commit.
---

# Pre-commit review

Run `git diff --staged` (or `git diff`) and check every hunk against this list. Fix findings before
committing; do not commit "fix later" TODOs for correctness problems.

**Style** (AGENTS.md "Code style")
- [ ] Naming: PascalCase types/functions, camelCase locals/params, `m_`/`s_` prefixes, `BS_` macros.
- [ ] `scripts/format.sh` produces no changes.
- [ ] Includes use `Basalt/...` paths and the documented order.

**Correctness**
- [ ] Every fallible call (file I/O, JSON parse, Lua call, GPU resource creation) has its failure handled and logged.
- [ ] No asserts on user-data conditions; asserts only for programmer errors.
- [ ] Ownership is clear (`Scope`/`Ref`/nvrhi handles); no leaks on early-return paths.
- [ ] GPU resources referenced by in-flight frames are not destroyed early (nvrhi handles defer release; raw Vulkan does not).
- [ ] Thread safety: anything called from the automation server thread goes through `Application::SubmitToMainThread`.
- [ ] Shader bindings follow the zero-offset convention; no duplicate slot numbers in one layout.

**Tests**
- [ ] New behaviour has unit tests; bug fixes have regression tests.
- [ ] Lua/automation-visible features are covered by the feature test scene.
- [ ] `scripts/test.sh` passes; rendering changes were run with zero validation errors.

**Docs**
- [ ] AGENTS.md / skills updated when workflows, conventions, or public APIs change.
