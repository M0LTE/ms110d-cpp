# Working in this repo

## Epics and stories

All work is tracked as GitHub issues: one epic per stage of [docs/plan.md](docs/plan.md), with stories under it as sub-issues. Keep them up to date as you go, not at the end.

- Before starting work, find its story. If there is none, create one under the right epic first (label `story`, linked as a sub-issue).
- A PR names its story (`Closes #N` when it finishes the story, `Part of #N` when it does not).
- Tick a story's checklist items as they land. Close the story when its last PR merges.
- Work found along the way becomes a new story, not a hidden extra in someone else's PR.
- When scope changes, edit the story or epic body to say what it now is, and add a comment saying why.
- When an epic's last story closes, check its gate in the plan is really met before closing the epic.
- Plan changes go in the plan's amendment log, and the affected epics and stories are updated to match.

## Compatibility

The C# package [M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d) is the reference and MIL-STD-188-110D is the yardstick. Don't re-check the standard's tables (already verified for the C#); do read each stage's logic against the standard and raise disagreements. Every accepted difference is a named test with an issue number; none exist only in prose.

## Copyright

Every C, C++ and C# source file starts with these two lines (the `copyright_headers` test enforces it):

    // Copyright (C) 2026 Thomas Fanning
    // SPDX-License-Identifier: AGPL-3.0-or-later

## Public repo

This repo is public. Keep internal hostnames, IP addresses, container numbers and private names out of code, docs, issues and PRs.
