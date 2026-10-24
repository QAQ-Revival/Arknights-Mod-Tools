# Build and verification

Initialize the submodules and update their tracked branches. Windows x64,
Visual Studio C++ build tools, Git LFS, Node.js 24+, and Python 3.12 are required.
Run `git -C vendor/UnityPy lfs pull` to materialize its type-definition data.

Run `node tools/build.cjs --test`. Set `QAQR_PYTHON` to a Python executable if it
is not on PATH. `QAQR_PY_PACKAGES` may point to a prepared directory containing
the packages pinned in `python/requirements.txt`.

The loader and runtime DLL are compiled from `native/` plus the MinHook submodule.
UnityPy is imported from its source checkout. During staging, its eager
AudioClipConverter import is removed from `export/__init__.py`; the upstream
checkout is unchanged. AudioClipConverter, fmod_toolkit and pyfmodex are excluded
from the frozen worker, and the output inventory rejects FMOD and legacy M4A
binaries. This build supports image and Spine conversion, not AudioClip decoding.

The Python packages are open-source libraries with pinned versions; their license
notices are copied into the distribution. The Windows platform and its normal
compiler/runtime components are external platform dependencies.

`dist/component.json` records the dependency commits and hashes of distribution
files. Source changes require rebuilding; do not edit the built runtime manually.
No private credentials, game data or game art belong in this repository.

Each submodule tracks the branch declared in `.gitmodules` and `sources.json`.
Use `git submodule update --init --remote --recursive` to select each branch's
latest commit, then rebuild and run verification. The superproject gitlinks and
build manifest record the checkout used for that build; no tag is enforced.

## Runtime boundary

The worker rebuilds separate PC bundles using the current local game bundles as
templates. It preserves the Windows platform and object identities. Only known
texture/Spine object bindings are replaced. Original game files are read only.

`ArkLoader --check --manifest <manifest.tsv>` verifies paths and both original and
replacement SHA-256 values without starting the game. `--launch` is a separate,
explicit operation. It starts the selected executable and loads the supplied
runtime DLL through the normal Windows process APIs. It does not alter protection
services, patch the game executable, or fall back to another injector.

The runtime resolves documented Unity AssetBundle file-loading interfaces through
exported IL2CPP APIs and redirects only exact manifest paths. Packed offsets and
memory-based loading are unsupported. Missing interfaces disable the runtime and
produce a status report. Restart the game after changing the enabled Mod set;
live reload and existing-bundle invalidation are not implemented.

Compilation, manifest tests and round-trip bundle checks do not establish game
compatibility. The current implementation is experimental until exercised in the
target game. `gameRuntimeValidated` remains false in generated component metadata.
