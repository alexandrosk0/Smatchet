# Third-party licenses

Smatchet vendors or links the following third-party components. Each component's
upstream license terms apply to that component; nothing in this file overrides
those terms or the project's main [`LICENSE`](LICENSE) (MIT).

Components fetched at build time by CMake `FetchContent` (cpr, SQLiteCpp, nlohmann/json,
md4c, ImGui, GLFW, Lua, sol2, ghc::filesystem, cpp-httplib, doctest) carry their own
LICENSE files in their respective fetch directories — see `.fetchcontent-src/<name>-src/`
once the project has been configured.

The components listed below are either vendored directly into the source tree, or
shipped alongside the binary as runtime assets, so their licences are inlined here.

---

## IconFontCppHeaders — `Source/Core/ThirdParty/IconsFontAwesome6/IconsFontAwesome6.h`

Source: <https://github.com/juliettef/IconFontCppHeaders>
Vendored revision: header generated against Font Awesome 6.x metadata.

License: **zlib**

```
Copyright (c) 2017 Juliette Foucaut and Doug Binks

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

---

## ImGuiColorTextEdit — `Source/Core/ThirdParty/ImGuiColorTextEdit/TextEditor.{h,cpp}`

Source: <https://github.com/BalazsJako/ImGuiColorTextEdit>
Vendored revision: modified copy (reformatted to this repo's style; compiled into the
Smatchet binaries for the syntax-highlighted code view and ticket field editors).

License: **MIT**

```text
Copyright (c) 2017 BalazsJako

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
```

---

## stb (stb_image, stb_image_write) — `Source/Core/ThirdParty/stb/stb_image{,_write}.h`

Source: <https://github.com/nothings/stb>
Vendored revision: `stb_image.h` v2.30, `stb_image_write.h` (compiled into the Smatchet
binaries for attachment image preview and screenshot writing).

License: **dual-licensed — MIT OR Public Domain (Unlicense), at your option**. The full
dual-license text is retained at the bottom of each vendored header; Smatchet
redistributes under ALTERNATIVE A (MIT):

```text
Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Font Awesome 6 Free Solid — `assets/fonts/fa-solid-900.ttf`

Source: <https://github.com/FortAwesome/Font-Awesome>
Distribution: committed at `assets/fonts/fa-solid-900.ttf` and shipped next to
the Smatchet executable via CMake POST_BUILD copy (see `assets/fonts/README.md`).

License: **SIL Open Font License 1.1**

```
Copyright (c) 2024 Fonticons, Inc. (https://fontawesome.com)
```

Full SIL OFL 1.1 text: <https://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL>

The license permits redistribution, modification, and embedding of the font in
commercial products, subject to two conditions: (1) the font itself may not be
sold standalone (Smatchet ships it bundled with the application, not as a font
package), and (2) the reserved font names "Font Awesome" and "Font Awesome 6"
may not be used to identify derivative works.

## Roboto Medium — `Source/Mobile/AndroidApp/app/src/main/assets/fonts/Roboto-Medium.ttf`

Source: <https://github.com/googlefonts/roboto> (vendored copy from the Dear ImGui
`misc/fonts/` distribution)
Distribution: committed to this repo and packaged into the Android APK assets as
the base UI font (injected at startup via `SmatchetSetInjectedFontBytes`). The
desktop builds embed the same typeface as a compiled-in byte array instead.

License: **Apache License 2.0**

```
Copyright 2011 The Roboto Project Authors (https://github.com/googlefonts/roboto)
```

Full Apache 2.0 text: <https://www.apache.org/licenses/LICENSE-2.0>

The license permits redistribution, modification, and embedding of the font in
commercial products with attribution. This NOTICE entry satisfies the Apache 2.0
attribution requirement for the bundled binary.

## Mozilla CA certificate bundle — `Source/Mobile/AndroidApp/app/src/main/assets/certs/cacert.pem`

Source: <https://curl.se/docs/caextract.html> (the curl project's PEM extract of
the Mozilla NSS root CA store)
Vendored snapshot: "Certificate data from Mozilla as of: Thu May 14 03:12:02 2026 GMT"
SHA-256: `86a1f3366afac7c6f8ae9f3c779ac221129328c43f0ab2b8817eb2f362a5025c`
Distribution: committed to this repo and packaged into the Android APK assets;
extracted to the app's private dir at boot. libcurl finds it via a baked
compile-time `CURL_CA_BUNDLE` define (the on-device private-dir path, set in
`cmake/SmatchetThirdParty.cmake`) so it can verify TLS peers (a stock Android
device ships no CA bundle reachable by the NDK-built libcurl). Desktop builds
rely on the OS trust store instead. Refresh cadence: re-pull when the Mozilla
snapshot date is materially stale (the upstream is regenerated roughly quarterly).

License: **Mozilla Public License 2.0** (the certificate data originates from
Mozilla's NSS; the curl maintainers distribute the extract under MPL-2.0)

```
This Source Code Form is subject to the terms of the Mozilla Public License,
v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain
one at https://mozilla.org/MPL/2.0/.
```

Full MPL 2.0 text: <https://mozilla.org/MPL/2.0/>
