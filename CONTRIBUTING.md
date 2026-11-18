# Contributing to xal

This file is for anyone sending a change to xal, with or without a coding agent.
Build instructions and the API are described in the [README](README.md).

## Scope

xal is an efficient library for accessing file extent maps. Please keep these
limits in mind before starting on a larger change:

- XFS is the only supported file system, including for the FIEMAP backend. FIEMAP
  may work elsewhere, but behaviour specific to other file systems is out of scope.
- Old Linux kernels are not supported. A problem that only occurs on an old kernel
  is not a bug in xal.
- Forensics, recovery and repair are out of scope.
- Only directories and regular files are indexed, see Limits in the README.

## Pull requests

Work on a topic branch and open a pull request against `main`. Pull requests are
merged by rebasing, so every commit lands on `main` as you wrote it. Each commit
should therefore be self-contained, and review fixes should be folded into the
commit they belong to rather than added on top.

The description should name what a reviewer should look at carefully, if anything
needs specific attention. It should not repeat the diff. If CI does not cover the
change, say why, what you ran by hand instead, and what remains unexercised.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/). Common scopes are
`xfs`, `fiemap`, `inotify`, `shm`, `pool`, `api`, `cijoe`, `toolbox` and `ci/gha`.
Mark a breaking change with `!`, as in `feat(api)!: ...`.

The message explains why the change exists and gives an overview. The diff shows
the details. One to three short paragraphs is normally enough.

Do not put test results or build details in the message; they belong in the pull
request. Do not refer to pull requests, issues or links on the hosting service.
Name the behaviour, the function, or the commit by its abbreviated hash and subject.

Every commit ends with a sign-off from the person responsible for it:

```
fix(xfs): read numrecs of a btree directory root from its own field

Explain why the change is needed.

Assisted-by: Claude Code:claude-opus-5-5
Signed-off-by: Full Name <address>
```

## Coding agents

Agents are welcome, but you remain the author. You must have read the change,
understand it, be able to say why it is correct, and answer for it in review.

Record the assistance with an `Assisted-by:` trailer naming the agent and the model
actually used. Put it before `Signed-off-by:`. Do not use `Co-Authored-By:` for an
agent, and do not add "Generated with" text to commits or pull requests.

## Licensing

xal is licensed under LGPL-3.0-or-later, see [LICENSE.txt](LICENSE.txt). Do not add
code whose origin you cannot account for.
