<!-- SPDX-License-Identifier: MIT -->

# The User Manual PDF

How the user manual becomes the PDF a release carries. The manual is the Markdown in [docs/user/](../user/README.md);
`tools/build_manual.sh` turns it into one PDF:

```
tools/build_manual.sh                         # build/manual/RadioFoundry-manual-<git describe>.pdf
tools/build_manual.sh --version v0.1.0        # the version on the title page and in the file name
tools/build_manual.sh --out manual.pdf
tools/build_manual.sh --typ-only              # stop after the typst source, for debugging the template
```

| Part | Does |
|---|---|
| `tools/build_manual.sh` | Collects `docs/user/[0-9][0-9]-*.md` in order, runs pandoc, then typst |
| `tools/manual/manual.lua` | pandoc filter: turns `<img width>` into images, maps links between chapters to PDF links, turns links into the repository into web links (with `RFOUNDRY_REPO_URL`) or plain text |
| `tools/manual/manual.typ` | typst template: title page, contents, numbered chapters, page footer, tables |

Chapter files are numbered (`01-introduction.md`, ...); a new chapter is a new numbered file plus a line in
[docs/user/README.md](../user/README.md). Images are `<img src="../ux/screens/160x120i5/NAME.png" width="200" alt="...">`,
which GitHub and the PDF both show. A missing image is left out of the PDF with a warning. The PDF goes to `build/`,
which git ignores: never commit it.

Set `RFOUNDRY_REPO_URL` (for example `https://github.com/OWNER/RadioFoundry/blob/v0.1.0`) to make links to files
outside the manual into web links. `$PANDOC` and `$TYPST` choose the binaries.

## Toolchain: pandoc and typst

| Option | Why not |
|---|---|
| pandoc with LaTeX | A TeX distribution is gigabytes to install in CI, slow, and its errors are hard to read |
| A headless browser (wkhtmltopdf, Chromium) | Heavy, poor page control (headers, page breaks, contents) |
| mdBook, MkDocs | Built for web sites; their PDF output goes through a browser anyway |
| pandoc with typst | Two single static binaries (about 30 MB each), seconds per build, page layout in one readable template, fonts bundled with typst |

pandoc reads GitHub Markdown, so the files stay readable on GitHub. typst lays out the pages. Versions in use:
pandoc 3.6.4 and typst 0.13.1 in CI (`.github/workflows/manual.yml`); `tools/build_manual.sh` needs pandoc 3.1.12 and typst 0.12 or later. Install them:

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
