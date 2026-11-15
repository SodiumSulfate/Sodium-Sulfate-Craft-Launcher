# Third-Party Notices

## Material Symbols

- Project: Material Symbols (Outlined, weight 400, 24dp)
- Source: https://github.com/google/material-design-icons
- Browsing: https://fonts.google.com/icons
- Copyright: Google LLC
- License: Apache License 2.0
- Local license text: `assets/icons/LICENSE-material-symbols.txt`
- Used files: the SVG files under `assets/icons/` embedded as Windows RCDATA.
- Modifications: none to the SVG artwork; the application parses the SVG `path`
  data at runtime and fills it with its current theme color.

Apache 2.0 permits commercial use, modification, and redistribution provided the
license and attribution notices are retained.

## nlohmann/json

- Project: https://github.com/nlohmann/json
- Copyright: 2013-2022 Niels Lohmann
- License: MIT
- Used for: JSON parsing/serialization (launcher config, version manifests,
  platform APIs).

## miniz

- Project: https://github.com/richgel999/miniz
- Copyright: 2013-2014 RAD Game Tools and Valve Software;
  2010-2014 Rich Geldreich and Tenacious Software LLC
- License: MIT
- Used for: ZIP/JAR reading, modpack import/export.

## doctest

- Project: https://github.com/doctest/doctest
- Copyright: 2016-2023 Viktor Kirilov
- License: MIT
- Used for: unit tests only; it is not linked into the released `SSCL.exe`.

## MIT License (applies to nlohmann/json, miniz, doctest)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Windows system components

Direct2D, DirectWrite, Direct3D 11, DXGI, WIC, DWM, WinHTTP, Crypt32, BCrypt
and other Windows APIs are provided by Microsoft Windows and are used under
the Windows SDK/system component terms. SSCL does not redistribute them.
