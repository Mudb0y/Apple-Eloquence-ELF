# filters/ — anti-crash regex tables (GPL-2.0-or-later)

The per-language regex tables under this directory are derived from the
[NVDA-IBMTTS-Driver](https://github.com/davidacm/NVDA-IBMTTS-Driver)
project — specifically
`addon/synthDrivers/ibmeci.py` (`english_fixes`, `english_ibm_fixes`,
`ibm_global_fixes`, `spanish_fixes`, `spanish_ibm_fixes`,
`spanish_ibm_anticrash`, `french_fixes`, `french_ibm_fixes`,
`german_fixes`, `german_ibm_fixes`, `portuguese_ibm_fixes`,
`ibm_pause_re`).

Original copyright: © 2009-2026 David CM (`dhf360@gmail.com`) and
contributors, released under the GNU General Public License version 2.
Full GPLv2 text: `../LICENSE.GPL` (project-wide for the sd_eloquence
subtree).

The pattern strings have been translated from Python `re` byte-mode
syntax to PCRE2 8-bit syntax. Where the original used `\xNN` byte
escapes for cp1252 characters, those translate directly. Inline `(?i)`
flags become `PCRE2_CASELESS` on the rule.
