#!/bin/sh
# SPDX-License-Identifier: MIT
# Fails if any source file outside docs/ and LICENSES/ carries a marker of
# code that must not be in this repository (see docs/CLEANROOM.md). The README's
# Acknowledgements section names people on purpose and is skipped.
cd "$(dirname "$0")/.." || exit 2
pattern='commercial purposes|Micrium|GNU General Public|GPL-2|GPL-3|G4KLX|Naylor|Roger Clark|VK3KYY|Kai Ludwig|DG4KLU|F1RMB|Caujolle|G4EML|DL4LEX|open[g]d77|Hangzhou C-SKY|Wang Huibin'
hits=$(git ls-files | grep -Ev '^(docs/|LICENSES/|PROVENANCE.md|README.md|tools/license_grep.sh)' |
       xargs grep -niIE "$pattern" 2>/dev/null)
readme=$(awk '/^## /{skip=($0=="## Acknowledgements")} !skip{print FILENAME":"FNR":"$0}' README.md |
         grep -iE "$pattern")
hits=$(printf '%s\n%s' "$hits" "$readme" | sed '/^$/d')
if [ -n "$hits" ]; then
    echo "$hits"
    echo "license_grep: markers found" >&2
    exit 1
fi
echo "license_grep: clean"
