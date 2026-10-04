<p align="center">
  <img src="docs/media/banner.png" alt="SpiderPet: a tiny digital organism that crawls the page" width="100%">
</p>

<p align="center">
  <img alt="Firefox, Brave, Chrome, Edge" src="https://img.shields.io/badge/browser-Firefox%20%7C%20Brave%20%7C%20Chrome%20%7C%20Edge-0B0C14?style=flat-square&logoColor=7FD6FF">
  <img alt="Windows 10/11" src="https://img.shields.io/badge/Windows-10%20%7C%2011-0B0C14?style=flat-square&logo=windows&logoColor=7FD6FF">
  <img alt="Local AI" src="https://img.shields.io/badge/local%20AI-Qwen%203.8-0B0C14?style=flat-square&logoColor=93F5AE">
  <img alt="MIT License" src="https://img.shields.io/badge/license-MIT-0B0C14?style=flat-square">
</p>

<p align="center">
  <b>Drop a spider on any web page. It walks across the text, harvests the DOIs, ISBNs, paper ids, titles,<br>
  links and key facts, restyles them right in the page, and checks every one against a real source.</b>
</p>

<p align="center">
  <img src="docs/media/page.png" alt="The spider on a reference list: DOIs in green code, ISBNs in big blue, ids as tags, titles in salmon, each with a verified badge" width="820">
</p>

---

## What it does

- **Reads the real page.** It runs inside the browser and reads the page itself, so every DOI, title and sentence is spelled exactly as written. Nothing is guessed from pixels. Hidden text, menus, cookie banners and ads are left out.
- **Walks to what matters.** Eight jointed legs, a magenta lasso, bursts of speed like a real spider. It goes for what you are looking for first, reads key sentences with a sweep, and scrolls the page by itself.
- **Restyles the real text.** Like the clip that inspired it: DOIs turn into green code, ISBNs into big blue code, ids into tags, titles into salmon serif, links get a frame. The page itself changes; nothing is painted over it.
- **Checks everything.** Each find gets a badge in the page: **✓ verified**, **⚠ wrong**, **✗ not found**, **? unclear**, **opinion** or **ad**. Hover for the reason and the source; click to open the source.
- **Keeps a library.** The SpiderPet app keeps every find from every browser, with its check, its source and the page it came from. Search it, filter it, save it as JSON or CSV.

## How the checking works

The local AI is the judge, never the source. It only compares a find with a record or a passage that was looked up, and it has to quote what it relied on; a verdict whose quote is not really in the source is thrown out.

| Find | Looked up in | Wrong when |
|---|---|---|
| DOI | Crossref, the DOI system | the DOI does not exist, or it belongs to a different paper than the citation names |
| ISBN | check digit, OpenLibrary, Google Books | the check digit is wrong, or the book is a different one |
| PMID, PMC, arXiv id | PubMed, PubMed Central, arXiv | no such record, or a different paper |
| Citation title | Crossref | (shown as verified when a published work has that title) |
| Fact in a sentence | Wikipedia, chosen by the AI | the evidence says something incompatible |

The AI also sorts sentences into facts, opinions and advertising, writes a short summary of each page with its key points, and ranks finds against what you typed in **What are you looking for?**

Only the find leaves your PC (an id, a title or a few search words), never the page. Turn **Online checks** off in the app and nothing leaves at all.

<table>
  <tr>
    <td width="50%"><img src="docs/media/panel.png" alt="The side panel: page summary, finds with verdicts and sources"></td>
    <td width="50%"><img src="docs/media/app.png" alt="The SpiderPet app: status, goal, tabs"></td>
  </tr>
  <tr>
    <td align="center"><sub>the side panel in the browser</sub></td>
    <td align="center"><sub>the SpiderPet app: the library and the brain</sub></td>
  </tr>
</table>

## Install

1. Install [Ollama](https://ollama.com) and a model. The smartest one SpiderPet knows is Qwen 3.8 27B (`qwen3.8-27b-uncensored-64k`, about 13 GB, runs at ~35 tokens/s on an RTX 4080); `qwen3:8b` works on smaller cards.
2. Download **SpiderPet.exe** and **SpiderHost.exe** from the [latest release](https://github.com/CodingIsCoolFr/SpiderPet/releases/latest) into one folder and run SpiderPet.exe once. It tells your browsers where it is.
3. Add the extension:
   - **Brave, Chrome, Edge:** unzip `spiderpet-chromium.zip`, open `brave://extensions` (or `chrome://`, `edge://`), turn on Developer mode, click **Load unpacked** and pick the folder.
   - **Firefox:** drag `spiderpet-firefox.xpi` into a Firefox window and click **Add**. (Building it yourself: `extension\sign-firefox.ps1` gets it signed by Mozilla with your free addons.mozilla.org API key.)
4. On any page, click the spider button in the toolbar, or press **Alt + Shift + S**.

The app sits in the tray while the browser is connected. Close its window and it keeps working; quit from the tray icon.

## Build from source

Needs Visual Studio 2022 (C++), CMake 3.21+ and [vcpkg](https://vcpkg.io) with `imgui` and `nlohmann-json` for `x64-windows`.

```powershell
vcpkg install imgui nlohmann-json --triplet x64-windows
powershell -ExecutionPolicy Bypass -File build.ps1
```

That builds `SpiderPet.exe`, `SpiderHost.exe` and the extension (`extension\chromium`, `extension\firefox`).

```
extension/src   the spider in the page: reading, walking, restyling, badges, the side panel
src/app         SpiderPet.exe (window, tray, library), SpiderHost.exe (the browser's bridge)
src/mind        the checker: Crossref, OpenLibrary, PubMed, arXiv, Wikipedia, and the local model
src/core        small helpers
```

The browser starts SpiderHost.exe (native messaging); it relays everything over a private named pipe to SpiderPet.exe, which owns the library and the checks.

## Privacy

The extension sends what it harvests to SpiderPet.exe on your own PC. SpiderPet.exe sends each find (never the page) to the public sources above when Online checks are on, and talks to Ollama on `127.0.0.1`. The library stays in `Documents\SpiderPet`, settings in `%APPDATA%\SpiderPet`.

## Credits

- Inspired by the node-and-line spider in [this clip by @Ufosoldier](https://x.com/Ufosoldier/status/2105902567846756439).
- Page in the screenshots: the Wikipedia article [Spider](https://en.wikipedia.org/wiki/Spider), CC BY-SA 4.0.
- [Dear ImGui](https://github.com/ocornut/imgui) Win32/DX11 backends (MIT), [nlohmann/json](https://github.com/nlohmann/json) (MIT).
- Lookups: [Crossref](https://www.crossref.org), [OpenLibrary](https://openlibrary.org), [Google Books](https://books.google.com), [NCBI E-utilities](https://www.ncbi.nlm.nih.gov/books/NBK25501/), [arXiv](https://arxiv.org), [Wikipedia](https://www.wikipedia.org).

## License

[MIT](LICENSE)
