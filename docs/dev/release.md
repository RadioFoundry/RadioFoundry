<!-- SPDX-License-Identifier: MIT -->

# Releases

A release is a git tag `vMAJOR.MINOR.PATCH` on main and a GitHub release with the user manual as a PDF. There are no
firmware binaries yet: users build the image and merge their own codec.

## Checklist

1. Main is green: `JOBS=8 tools/ci.sh` ends with "all checks passed", on a machine with both compilers and `reuse`.
2. The project lead runs the similarity scan against the reference trees and reviews every hit:
   `ODR_SIMILARITY_REFS="..." tools/ci.sh` ([docs/testing/README.md](../testing/README.md) 1).
3. The HIL tests pass on each radio the release claims, with the flags its features need.
4. The user manual matches the release: the status of each setting in [Settings](../user/12-settings.md), the
   supported radios in the [Introduction](../user/01-introduction.md), the screenshots (`make -C tests/host ui-gallery`).
5. The README's radio status table is current.
6. Build the manual locally and read it through: `tools/build_manual.sh`.
7. Tag and push the tag:

   ```
   git tag -a v0.1.0 -m "OpenDigitalRadio 0.1.0"
   git push origin v0.1.0
   ```

8. The manual workflow builds the PDF and attaches it to the release (below). Edit the release notes on GitHub.

## The user manual PDF

The manual is the Markdown in [docs/user/](../user/README.md). `tools/build_manual.sh` turns it into one PDF:

```
tools/build_manual.sh                         # build/manual/OpenDigitalRadio-manual-<git describe>.pdf
tools/build_manual.sh --version v0.1.0        # the version on the title page and in the file name
tools/build_manual.sh --out manual.pdf
tools/build_manual.sh --typ-only              # stop after the typst source, for debugging the template
```

| Part | Does |
|---|---|
| `tools/build_manual.sh` | Collects `docs/user/[0-9][0-9]-*.md` in order, runs pandoc, then typst |
| `tools/manual/manual.lua` | pandoc filter: turns `<img width>` into images, maps links between chapters to PDF links, turns links into the repository into web links (with `ODR_REPO_URL`) or plain text |
| `tools/manual/manual.typ` | typst template: title page, contents, numbered chapters, page footer, tables |

Chapter files are numbered (`01-introduction.md`, ...); a new chapter is a new numbered file plus a line in
[docs/user/README.md](../user/README.md). Images are `<img src="../ux/screens/160x120i5/NAME.png" width="200" alt="...">`,
which GitHub and the PDF both show. A missing image is left out of the PDF with a warning. The PDF goes to `build/`,
which git ignores: never commit it.

Set `ODR_REPO_URL` (for example `https://github.com/OWNER/OpenDigitalRadio/blob/v0.1.0`) to make links to files
outside the manual into web links. `$PANDOC` and `$TYPST` choose the binaries.

### Toolchain: pandoc and typst

| Option | Why not |
|---|---|
| pandoc with LaTeX | A TeX distribution is gigabytes to install in CI, slow, and its errors are hard to read |
| A headless browser (wkhtmltopdf, Chromium) | Heavy, poor page control (headers, page breaks, contents) |
| mdBook, MkDocs | Built for web sites; their PDF output goes through a browser anyway |
| pandoc with typst | Two single static binaries (about 30 MB each), seconds per build, page layout in one readable template, fonts bundled with typst |

pandoc reads GitHub Markdown, so the files stay readable on GitHub. typst lays out the pages. Versions in use:
pandoc 3.6, typst 0.13. Install them:

| System | Command |
|---|---|
| macOS | `brew install pandoc typst` |
| Debian, Ubuntu | `pandoc` from the [release page](https://github.com/jgm/pandoc/releases) (the distribution's is often too old), `typst` from its [release page](https://github.com/typst/typst/releases) |
| Any | Download both release archives, unpack, and point `$PANDOC` and `$TYPST` at the binaries |

## The manual workflow

`.github/workflows/manual.yml`, independent of the firmware build:

| Trigger | Does |
|---|---|
| Push of a `v*` tag | Checks the doc links, builds the PDF with the tag as its version, uploads it as a workflow artifact, creates the GitHub release if it doesn't exist and attaches the PDF |
| Pull request or push to main touching `docs/`, `README.md` or the manual tools | Checks the doc links and builds the PDF as an artifact; nothing is published |
| Manual run (`workflow_dispatch`) | On a `v*` tag, the same as a tag push; on a branch, the same as a pull request |

It downloads fixed versions of pandoc and typst, so the PDF doesn't change with the runner image. To rebuild the PDF
for an existing tag, run the workflow by hand on the tag (`gh workflow run manual.yml --ref v0.1.0`); the upload
replaces the asset.
