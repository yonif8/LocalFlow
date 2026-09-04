# Qt 6.8.3 redistribution texts

These directories are unmodified copies of the complete `LICENSES` directory
from each Qt module whose binaries LocalFlow deploys. They were taken from the
official Qt Git repositories at the immutable commits selected by `v6.8.3`:

- `qtbase`: `c07c2d5a527a644d36e7853d55132ae38921682f`
  (`https://code.qt.io/cgit/qt/qtbase.git/tree/LICENSES?id=c07c2d5a527a644d36e7853d55132ae38921682f`)
- `qtdeclarative`: `2ec235a2235a119887683e55787b93f99dc5f12f`
  (`https://code.qt.io/cgit/qt/qtdeclarative.git/tree/LICENSES?id=2ec235a2235a119887683e55787b93f99dc5f12f`)
- `qtshadertools`: `75786914936b73212ae342f83c9a27ae4a69f406`
  (`https://code.qt.io/cgit/qt/qtshadertools.git/tree/LICENSES?id=75786914936b73212ae342f83c9a27ae4a69f406`)
- `qtwayland`: `8ab0d5b4cd2db654eb01eb74c9a8bbf7efc87d94`
  (`https://code.qt.io/cgit/qt/qtwayland.git/tree/LICENSES?id=8ab0d5b4cd2db654eb01eb74c9a8bbf7efc87d94`)

Keep the module directories separate: a few licenses share a filename but
have module-specific copyright or exception text. When Qt changes, replace
each whole directory from the reviewed release tag rather than editing or
summarizing these upstream texts.

`MANIFEST.sha256` records every one of the 74 upstream files. Both packaging
lanes verify every digest and the exact inventory before copying this tree.
