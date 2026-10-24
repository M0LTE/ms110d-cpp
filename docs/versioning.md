# Versioning policy

`m0lte-ms110d` follows [Semantic Versioning 2.0.0](https://semver.org/), as the C# package [M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d/blob/main/docs/versioning.md) does. The library version is the `project(... VERSION x.y.z)` line in the top-level `CMakeLists.txt`, and it is what `version.hpp` and `ms110d_version()` report.

## The public API is the contract

"The API" means the public C++ headers under `include/` and the C header `include/m0lte/ms110d/ms110d.h`. The `api_lock` test holds the headers to the snapshot `tests/api/public-api.txt` (see [dev/api-lock.md](dev/api-lock.md)), so a change to the API always shows up in the diff of a PR, together with the version bump it needs.

## What bumps which part

| Change | Version part |
| --- | --- |
| A public declaration is removed or renamed, a signature or struct layout changes, or an existing function's contract is narrowed (a breaking change) | MAJOR |
| Public declarations are only added, so existing users keep compiling and behaving | MINOR |
| The API snapshot does not change: a bug fix, performance, docs, an internal refactor | PATCH |

A behavioural break with no signature change is still a breaking change. Judgement applies; the snapshot is the floor, not the ceiling.

## While the version is 0.x

The API is still settling, and SemVer allows anything to change. In practice we keep the discipline above one step down: a breaking change bumps MINOR, and an addition or a fix bumps PATCH. `1.0.0` will be cut when the surface is declared stable.

## The C ABI version

`MS110D_ABI_VERSION` (see [dev/c-interface.md](dev/c-interface.md)) is separate from the library version. It is bumped on any change that can break an already-built caller of the shared library: a changed signature, struct layout or constant value. It is also the shared library's SOVERSION. In 0.x, adding a function does not bump it. A breaking change to the C header therefore needs both: an ABI bump and a MINOR (0.x) or MAJOR (1.x and later) library bump. A change to the C++ headers alone never touches the ABI version.

## The C# reference pin

`reference/pin.txt` names the release of M0LTE.Ms110d that the reference files and the .NET interop tests use (see [dev/reference-files.md](dev/reference-files.md)). The pin and this library's version are independent: moving the pin is not itself a release, and a release can keep the old pin. Moving the pin can change test expectations, so do it in its own PR, after the cross-checks pass. Each release is tested against the pin it carries, and the CI that runs on the tag uses that pin.

## Releasing

A release is a `vX.Y.Z` tag on green `main`. In one commit:

1. Bump the version in `CMakeLists.txt` per the table above, and the ABI version if needed.
2. Update the API snapshot if the headers changed.
3. In `CHANGELOG.md`, rename the Unreleased section to the new version and date, and start a fresh Unreleased section.

Then tag that commit and push the tag:

    git tag vX.Y.Z <full commit sha>
    git push origin vX.Y.Z

The [release](../.github/workflows/release.yml) workflow checks that the tag matches the CMake version, runs the full CI, builds a source archive (`m0lte-ms110d-X.Y.Z.tar.gz`) with a SHA-256 file, and creates a GitHub release with generated notes and the archive attached. Nothing is published to a package registry.
