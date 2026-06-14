# mtk++ — dependency setup

All dependencies live under `third_party/` as **git submodules**. No package
manager (vcpkg, conan, apt, brew) is required at build time. Everything builds
from source against pinned commits, fully offline-once-cloned.

## Preferred: clone via git submodules

If your machine has internet:

```sh
git clone https://github.com/<you>/mtk.git
cd mtk
git submodule update --init --recursive --depth 1
```

To update a pinned dep later:

```sh
cd third_party/<name>
git fetch && git checkout <tag-or-sha>
cd ../..
git add third_party/<name>
git commit -m "bump <name> to <tag>"
```

## Fallback: manual download (no submodule access)

If your build environment cannot reach `github.com` directly, fetch each
dependency on a machine that can, then drop the contents into the matching
`third_party/<name>/` directory.

| Dep            | Repo                                                  | Drop into                   |
|----------------|-------------------------------------------------------|-----------------------------|
| CLI11          | https://github.com/CLIUtils/CLI11                     | `third_party/CLI11/`        |
| toml++         | https://github.com/marzer/tomlplusplus                | `third_party/tomlplusplus/` |
| reproc         | https://github.com/DaanDeMeyer/reproc                 | `third_party/reproc/`       |
| fmt            | https://github.com/fmtlib/fmt                         | `third_party/fmt/`          |
| doctest        | https://github.com/doctest/doctest                    | `third_party/doctest/`      |
| nlohmann/json  | https://github.com/nlohmann/json                      | `third_party/json/`         |

Each directory must contain the upstream `CMakeLists.txt` at its root
(i.e. `third_party/fmt/CMakeLists.txt` exists, not
`third_party/fmt/fmt-master/CMakeLists.txt`).

You can fetch a snapshot as a tarball via:

```sh
curl -L https://github.com/fmtlib/fmt/archive/refs/tags/10.2.1.tar.gz \
  | tar -xz -C third_party/fmt --strip-components=1
```

(adapt the tag per project)

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Binary lands at `build/mtk`.
