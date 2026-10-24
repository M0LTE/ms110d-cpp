# Transcribed tables of MIL-STD-188-110D Appendix D

These files are copies of the tables in `docs/dev/ms110d/tables` of [packet-net/pdn-soundmodem](https://github.com/packet-net/pdn-soundmodem), taken at commit `56f7d2c2ae5e8ca12e29f7b324d44bac964efd13` (the last commit to touch that directory as of 2026-10-24), under the same copyright holder and licence as this repo. The standard prints these tables as images with no text layer, so they were transcribed by hand from MIL-STD-188-110D (29 December 2017, Distribution A), twice and independently, and the two transcriptions were compared with zero value conflicts. The pdn-soundmodem README (`docs/dev/ms110d/README.md`) tells that story and holds the standard itself.

They are the independent check of the C++ tables against the standard: `tests/spec_tables_test.cpp` loads them at test time and compares every table entry. They are committed, so those tests never skip. Do not edit them to make a test pass; a disagreement is a finding, to be raised as an issue (see CLAUDE.md).

Only the tables this library uses are copied (Tables D-I to D-VII, D-XI to D-XXV, D-XXXVII, D-XLIX and D-LI, plus the preamble prose for the Fixed-subsection di-bits). The files are verbatim, including their header comments, which name the transcriber and the standard's page.
