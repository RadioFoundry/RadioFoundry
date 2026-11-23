#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Fails if any source file outside docs/ and LICENSES/ carries a marker of
# code that must not be in this repository (see docs/CLEANROOM.md). The READMEs'
# Acknowledgements (致谢) sections name people on purpose and are skipped. FORKING.md shows
# how a project under another license (the GPL among them) marks a file it took from here.
# The qdmr codeplug files use qdmr's YAML keys (openGD77 blocks), which must match
# what qdmr writes and reads; they name the key, not any code.
cd "$(dirname "$0")/.." || exit 2
pattern='commercial purposes|Micrium|GNU General Public|GPL-2|GPL-3|G4KLX|Naylor|Roger Clark|VK3KYY|Kai Ludwig|DG4KLU|F1RMB|Caujolle|G4EML|DL4LEX|open[g]d77|Hangzhou C-SKY|Wang Huibin'
hits=$(git ls-files | grep -Ev '^(docs/|LICENSES/|FORKING.md|PROVENANCE.md|README.md|README.zh.md|tools/license_grep.sh|tools/codeplug_port\.py$|site/static/js/codeplug/qdmr\.js$|site/tests/qdmr\.test\.mjs$)' |
       xargs grep -niIE "$pattern" 2>/dev/null)
readme=$(awk '/^## /{skip=($0=="## Acknowledgements")} !skip{print FILENAME":"FNR":"$0}' README.md |
         grep -iE "$pattern")
readme_zh=$(awk '/^## /{skip=($0=="## 致谢")} !skip{print FILENAME":"FNR":"$0}' README.zh.md |
            grep -iE "$pattern")
hits=$(printf '%s\n%s\n%s' "$hits" "$readme" "$readme_zh" | sed '/^$/d')
if [ -n "$hits" ]; then
    echo "$hits"
    echo "license_grep: markers found" >&2
    exit 1
fi
# A private project's names: no tracked file mentions them, docs included
priv=$(git ls-files | grep -v '^tools/license_grep.sh$' | xargs grep -niIE 'radiodesk|\brfgw\b' 2>/dev/null)
if [ -n "$priv" ]; then
    echo "$priv"
    echo "license_grep: private project names found" >&2
    exit 1
fi
echo "license_grep: clean"
