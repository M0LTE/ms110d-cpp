# Public API lock

Any change to the public headers has to be deliberate. The ctest `api_lock` compares the public headers with the checked-in snapshot `tests/api/public-api.txt` and fails if they differ.

## How it works

`cmake/ApiLock.cmake` reads every `*.hpp` under `include/` plus the generated headers in the build tree (for example `version.hpp`), removes `//` comments, trailing whitespace and blank lines, and joins the results in path order under `==== <path>` markers. Reformatting and comment edits therefore pass; any change to a declaration fails. The result is compared byte for byte with the snapshot.

Public headers must use `//` comments only (no `/* */`), because the script only strips the former.

The snapshot is the same for both precisions, since the choice of `float` or `double` is made by a compile definition rather than by generated text.

## When it fails

The failure message names a file in the build directory (`tests/public-api.actual.txt`) holding what the headers look like now. Diff it against the snapshot to see what changed.

If the change is intended, rewrite the snapshot and commit it in the same PR as the header change:

    cmake --build build --target update-api-lock

Then review the diff of `tests/api/public-api.txt` as part of the PR; that diff is the API change. A change to the public API also needs the right version bump (semantic versioning).
