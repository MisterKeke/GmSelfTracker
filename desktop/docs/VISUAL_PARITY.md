# Visual parity notes

The provided screenshots and the current source CSS were used as static references. The native UI keeps the exact base palette: night `#1b172b`, background `#211b35`, sidebar `#2c2244`, plum `#463363`, plum-dark `#36264f`, purple `#7657a7`, lavender `#c8aaff`, cream `#fff2d6`, cream-deep `#f4ddb0`, ink `#30233d`, gold `#f5d277`, green `#65b96c`, blue `#66a6e4`, red `#e67f75`, line `#c6a876`, and shadow `#110e1d`.

The layout uses the source geometry as the starting point: 78 px topbar, 260 px game rail, 125 px hero, and approximately 210 px nested section navigation. The Qt shell includes hard-edged borders, the sky/mountain strip, pixel-like headings, selected tabs, local profile controls, game search, section navigation, and cream empty-state panels.

The current version uses Courier New as the pixel-heading fallback. It does not bundle Press Start 2P or another third-party font. Native object, field, and goal creation dialogs and profile archive recovery are present. Migration and template screens are not yet implemented. The supplied reference screenshots informed layout and palette; no browser/server screenshot comparison was performed.

The preview uses a disposable SQLite file. To render with Windows system fonts, set `QT_QPA_PLATFORM=windows` and run the native app with `--render-preview=<path> --preview-size=1920x1080 --preview-page=goals`. The offscreen platform in the local kit rendered missing glyphs for Windows fonts. The Goals page was inspected with the Windows platform at logical window sizes of 1920×1080, 1440×900, and 1280×720, with Qt scale factors of 100%, 125%, and 150%, respectively. These runs simulate scaling and use native desktop rendering. Full source visual parity remains open.
