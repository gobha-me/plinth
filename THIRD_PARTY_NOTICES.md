# Third-party notices

Plinth is distributed under the MIT license in `LICENSE`. It uses or includes
the components below under their respective licenses. The corresponding
version-matched license texts are retained in `third_party/licenses/`; exact
versions, commits, and vendored-file hashes are recorded in
`third_party/dependencies.json` and `sbom.cdx.json`.

| Component | Version | License | Use |
|---|---:|---|---|
| Drogon | 1.9.12 | MIT | Fetched and patched at build time |
| nlohmann/json | 3.11.3 | MIT | Fetched at build time |
| spdlog | 1.15.3 | MIT | Fetched at build time |
| argparse | 3.1 | MIT | Fetched at build time |
| Catch2 | 3.7.1 | BSL-1.0 | Test dependency |
| libzip | 1.11.4 | BSD-3-Clause | Fetched at build time |
| QuickJS-NG | 0.14.0 | MIT | Fetched at build time |
| Google Benchmark | 1.9.1 | Apache-2.0 | Optional benchmark dependency |
| Preact and Preact Hooks | 10.22.0 | MIT | Vendored browser modules |
| HTM | 3.1.1 | Apache-2.0 | Vendored browser module |

`third_party/drogon-patches/ftype-accessor.patch` modifies Drogon 1.9.12 and is
distributed under Drogon's MIT terms. Preact Hooks has one local import-path
rewrite, documented alongside its checksum in
`client/shell/client/vendor/VERSIONS.txt`; it remains under Preact's MIT terms.

This inventory covers dependencies distributed with or fetched by the project.
Operating-system libraries installed by the user or container package manager
retain the licenses supplied by those distributions.
