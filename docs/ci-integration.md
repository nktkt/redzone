# CI integration: uploading redzone findings to GitHub code scanning

redzone can emit findings in [SARIF 2.1.0](https://sarifweb.azurewebsites.net/),
the format GitHub's **code scanning** feature understands. By running an
instrumented build of *your* program with `REDZONE_FORMAT=sarif` and uploading
the resulting `.sarif` file, redzone's memory-safety findings show up as code
scanning alerts on the **Security** tab of your repository and inline on pull
requests.

This guide is for **real code you want to test** — your project's own programs
and test harnesses.

> [!WARNING]
> Do **not** point this at intentionally-buggy programs (for example redzone's
> own `examples/heap_overflow.c`). Every finding you upload becomes a persistent
> code-scanning alert in the target repository, so uploading SARIF for code that
> is *supposed* to crash creates permanent, misleading alerts. Run the format
> tests (`./scripts/test_formats.sh`) against the examples instead — they
> validate the output without uploading anything.

## How redzone produces SARIF

- Findings are written to **stderr**; your program's own output stays on stdout.
- A run with a detected bug exits **nonzero**; a clean run exits **0**.
- With `REDZONE_FORMAT=sarif`, stderr is a **single** SARIF 2.1.0 document with
  one `result` per finding (overflow / use-after-free / double-free /
  invalid-free, and one result per leaked block for memory leaks).

So capture stderr to a file and feed that file to the uploader:

```bash
REDZONE_FORMAT=sarif ./your_instrumented_binary >/dev/null 2>redzone.sarif || true
```

The trailing `|| true` keeps the workflow step from failing just because the
program aborted on a detected bug — you *want* the run to continue so the SARIF
gets uploaded. (Drop it if you'd rather fail the build on any finding.)

## Example GitHub Actions workflow

This builds an instrumented binary of a program in *your* repository, runs it,
captures the SARIF report, and uploads it to code scanning.

```yaml
name: redzone

on:
  push:
    branches: [main]
  pull_request:

# Required so codeql-action can write the analysis back to your repo.
permissions:
  contents: read
  security-events: write

jobs:
  redzone:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4

      # redzone needs Homebrew LLVM (clang/opt) and cmake.
      - name: Install LLVM and CMake
        run: brew install llvm cmake

      - name: Put LLVM tools on PATH
        run: echo "$(brew --prefix llvm)/bin" >> "$GITHUB_PATH"

      # Get redzone itself. Adjust the ref/path to however you vendor it
      # (git submodule, a checkout into ./redzone, a release download, ...).
      - name: Check out redzone
        uses: actions/checkout@v4
        with:
          repository: nktkt/redzone
          path: redzone

      # Build YOUR program with redzone instrumentation.
      - name: Build instrumented binary
        run: ./redzone/scripts/redzone build src/myprogram.c -o myprogram

      # Run it, capturing redzone's SARIF findings from stderr. `|| true` lets
      # the workflow continue even when redzone detects a bug (nonzero exit) so
      # the report still gets uploaded.
      - name: Run under redzone (SARIF)
        run: |
          REDZONE_FORMAT=sarif ./myprogram >/dev/null 2>redzone.sarif || true
          cat redzone.sarif

      # Upload the report to GitHub code scanning. Findings appear under
      # Security -> Code scanning and inline on pull requests.
      - name: Upload SARIF to code scanning
        uses: github/codeql-action/upload-sarif@v3
        # `always()` so the report uploads even if an earlier step failed.
        if: always()
        with:
          sarif_file: redzone.sarif
          # Distinguishes redzone's alerts from other tools' in the same repo.
          category: redzone
```

## Notes

- **`security-events: write`** permission is required for `upload-sarif`; SARIF
  upload to a public repo also works from forked-PR runs only with the correct
  permissions, so prefer running on `push` / same-repo branches for full alerts.
- **Paths in the SARIF** come from the source paths redzone saw at build time
  (e.g. `src/myprogram.c`). Build from the repository root so those
  `artifactLocation.uri` values resolve to files GitHub can annotate.
- **Multiple programs / inputs:** run each one capturing to its own
  `*.sarif`, then upload them (the action accepts a directory via
  `sarif_file:`), or give each its own `category`.
- **Want JSON instead?** `REDZONE_FORMAT=json` emits JSON-Lines (one JSON object
  per finding) — handy for custom dashboards, but code scanning specifically
  wants SARIF.
