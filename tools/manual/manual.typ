// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
//
// Pandoc template for the PDF user manual (tools/build_manual.sh): a title page, the contents, one chapter per
// docs/user file, each starting on a new page. Uses only the fonts typst carries, so the output is the same on every
// machine.

#let title = [$title$]
#let subtitle = [$subtitle$]
#let date = [$date$]
#let horizontalrule = line(start: (25%, 0%), end: (75%, 0%), stroke: 0.5pt + luma(160))

#set document(title: "$title$")
#set text(lang: "en", region: "US", size: 10.5pt)
#set par(justify: false, leading: 0.62em, spacing: 0.9em)
#set page(
  paper: "us-letter",
  margin: (x: 0.9in, top: 0.85in, bottom: 0.9in),
  footer: context {
    if counter(page).get().first() > 1 {
      set text(size: 8.5pt, fill: luma(110))
      [RadioFoundry User Manual, $subtitle$]
      h(1fr)
      counter(page).display("1")
    }
  },
)

#show raw: set text(font: "DejaVu Sans Mono", size: 0.9em)
#show raw.where(block: true): block.with(fill: luma(245), inset: 7pt, radius: 2pt, width: 100%)
#show link: set text(fill: rgb("#1f4e9c"))

#set heading(numbering: "1.1")
#show heading.where(level: 1): it => {
  pagebreak(weak: true)
  set text(size: 20pt, weight: "bold")
  block(above: 0pt, below: 1.1em)[
    #if it.numbering != none [#counter(heading).display("1") #h(0.4em)]
    #it.body
  ]
}
#show heading.where(level: 2): set text(size: 13.5pt)
#show heading.where(level: 3): set text(size: 11.5pt)
#show heading.where(level: 4): set heading(numbering: none)

#set table(
  inset: (x: 5pt, y: 4pt),
  stroke: (x, y) => (top: if y <= 1 { 0.6pt } else { 0.3pt + luma(190) }, bottom: 0.6pt),
  fill: (x, y) => if y == 0 { luma(235) },
)
#show table: set text(size: 9.5pt)
#show table.cell: set align(left)
#show figure.where(kind: table): set align(left)
#show table.cell.where(y: 0): set text(weight: "bold")
#show figure.where(kind: table): set figure.caption(position: top)
#show figure: set block(breakable: true)

#show terms: it => {
  it.children.map(child => [
    #strong[#child.term]
    #block(inset: (left: 1.5em, top: -0.4em))[#child.description]
  ]).join()
}

// Title page
#page(footer: none)[
  #v(2.2in)
  #align(center)[
    #text(size: 30pt, weight: "bold")[#title]
    #v(0.6em)
    #text(size: 15pt)[#subtitle]
    #v(0.4em)
    #text(size: 11pt, fill: luma(90))[#date]
    #v(2.4in)
    #text(size: 10pt, fill: luma(90))[
      Free, MIT-licensed firmware for DMR radios. \
      This manual is built from the Markdown files in docs/user of the source repository.
    ]
  ]
]

$if(toc)$
#outline(title: [Contents], depth: $toc-depth$, indent: 1.2em)
#pagebreak(weak: true)
$endif$

$body$
