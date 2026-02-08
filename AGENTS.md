# Repository Guidelines

## Project Structure & Module Organization
This repository is currently minimal. As implementation is added, keep a predictable layout:
- `src/` for application or library code
- `tests/` for automated tests (prefer mirroring `src/`)
- `assets/` for static files and fixtures
- `docs/` for architecture notes and decisions

Favor small, focused modules and feature-based grouping once multiple components exist.

## Build, Test, and Development Commands
No build system is configured yet. When you add one, define all core tasks in a single entry point (`Makefile` or `package.json`) and update this file in the same PR.

Recommended baseline commands:
- `make dev` or `npm run dev`: run locally
- `make test` or `npm test`: run the full test suite
- `make lint` or `npm run lint`: run static checks/format validation

## Coding Style & Naming Conventions
- Use 4-space indentation unless language tooling requires otherwise.
- Use language-idiomatic naming, with these defaults:
  - files: `kebab-case` (scripts/assets) or language standard
  - classes/types: `PascalCase`
  - functions/variables: `camelCase`
- Keep functions single-purpose and avoid deep nesting.

Adopt formatter/linter tooling early (for example, Prettier/ESLint or Black/Ruff).

## Testing Guidelines
- Put tests in `tests/`.
- Use consistent names such as `test_<module>.py` or `<module>.spec.ts`.
- Add tests for every bug fix and non-trivial feature.
- Prefer deterministic tests; mock network/time when needed.

## Workflow (Adapted from IQIYI_subtitler)
- Create or update an Issue before non-trivial work.
- Use issue titles:
  - bug: `[Bug][<Area>] <component>: <symptom>`
  - feature: `[Feat][<Area>] <component>: <capability>`
  - chore: `[Chore] <component>: <change>`
- Issue bodies should include: Background/Goal, Current behavior, Expected behavior, Steps to reproduce (bugs), Acceptance criteria, and Notes/links.
- Labels: `type: bug|feature|chore` required; `priority: P0|P1|P2` and `area: ui|backend|infra|docs` optional.
- Branch naming: `fix/<issue>-<slug>`, `feat/<issue>-<slug>`, `chore/<issue>-<slug>`.
- PR title format: `<type>(<scope>): <summary> (#<issue>)`.
- PR body should include linked issue, test evidence, and impact/risk notes.
- Use `Fixes #<issue>` only when fully complete; otherwise use `Refs #<issue>`.
- Do not merge until acceptance criteria are met; delete merged local/remote branches afterward.

## Security & Configuration Tips
- Never commit secrets; use environment variables and a `.env.example`.
- Keep local artifacts in `.gitignore`.
- Pin critical dependency versions where reproducibility matters.
