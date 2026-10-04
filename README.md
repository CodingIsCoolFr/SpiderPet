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
  <b>Type what you are looking for, and a spider drops onto the page you have open and hunts for it. It walks<br>
  across the text, harvests the DOIs, ISBNs, paper ids, titles, links and key facts, restyles them right in<br>
  the page, and checks every one against a real source.</b>
</p>

<p align="center">
  <img src="docs/media/page.png" alt="The spider hunting for 'silk' on the Wikipedia Spider page: every match lit up, the sentence it is reading underlined, its name and thought in a cloud" width="820">
</p>

---

## What it does

- **Reads the real page.** It runs inside the browser and reads the page itself, so every DOI, title and sentence is spelled exactly as written. Nothing is guessed from pixels. Hidden text, menus, cookie banners and ads are left out.
- **Hunts for what you ask.** Type what you are looking for in the SpiderPet app. The spider drops onto the page you have open, every match on the page lights up at once, and it goes from match to match down the page. The local AI adds synonyms a moment later.
- **Answers questions about the page.** Type a question instead, like *what's this page about?* or *what do spiders eat?*, and the app answers it from the page you have open, quoting the page. When the AI can't run, you still get a quick answer built from the page's own sentences.
- **Does simple things for you, after you say yes.** Tell it what to do and press Enter: *click Sign in*, *type cats into the search box and press enter*, *go to wikipedia.org*, *scroll down*, *go back*, or several steps with *then*. The spider walks to the button or box and holds it with its lasso; the app shows exactly what it will do, and nothing is clicked or typed until you press **Do it**. When the wording is unclear (*click the second article*), your local AI picks from the page's buttons and links, and you still confirm. It never types into password or card boxes, and it warns you before buttons like Buy, Send or Delete.
- **Gets past popups.** Cookie banners, sign-up walls, newsletter popups and dark backdrops that cover the page are tucked out of sight, and a locked page can scroll again. Unclear cases go to your local AI. The spider never clicks Accept on a popup; stop it and everything comes back.
- **Walks like a spider.** Eight jointed legs, a magenta lasso, bursts of speed, a thought cloud with its name, silk between the things it ate. Without a search it reads the key sentences of the page with a sweep. It never scrolls your page: it works on the part you are looking at while the rest of the page is read out of sight.
- **One control center.** Everything is in the SpiderPet app: the search, Start/Stop spider, what it found on the page you are looking at, and the library. The browser needs no menus.
- **Restyles the real text.** Like the clip that inspired it: DOIs turn into green code, ISBNs into big blue code, ids into tags, titles into salmon serif, links get a frame. The page itself changes; nothing is painted over it.
- **Checks everything.** Each find gets a badge in the page: **✓ verified**, **⚠ wrong**, **✗ not found**, **? unclear**, **opinion** or **ad**. Hover for the reason and the source; click to open the source.
- **Keeps a library.** The SpiderPet app keeps every find from every browser, with its check, its source and the page it came from. Search it, filter it, save it as JSON or CSV. **Forget** buttons clear one page or everything, including the remembered checks.
- **Reads any site and any language.** Sites built without paragraphs (X, Reddit, Pinterest) are read too, and the app shows Japanese, Chinese, Korean, Indian scripts and emoji.
- **Stays out of the way.** It draws at most 60 frames a second and re-reads a page only when the page really changed. Before it loads the AI it checks free graphics memory: when a game or VRChat has taken it, the AI waits instead of making your PC stutter, and ids and titles are still checked online.

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

<p align="center">
  <img src="docs/media/app.png" alt="The SpiderPet app: the search, Start/Stop spider, the tab you are on, and the finds that match with their checks" width="420"><br>
  <sub>the SpiderPet app: the search, the spider's switch, and every find with its check</sub>
</p>

## Install

1. Install [Ollama](https://ollama.com) and a model. The smartest one SpiderPet knows is Qwen 3.8 27B (`qwen3.8-27b-uncensored-64k`, about 13 GB, runs at ~35 tokens/s on an RTX 4080); `qwen3:8b` works on smaller cards.
2. Download **SpiderPet.exe** and **SpiderHost.exe** from the [latest release](https://github.com/CodingIsCoolFr/SpiderPet/releases/latest) into one folder and run SpiderPet.exe once. It tells your browsers where it is.
3. Add the extension:
   - **Brave, Chrome, Edge:** unzip `spiderpet-chromium.zip`, open `brave://extensions` (or `chrome://`, `edge://`), turn on Developer mode, click **Load unpacked** and pick the folder.
   - **Firefox:** install SpiderPet from [addons.mozilla.org](https://addons.mozilla.org) once it is listed there (`extension\sign-firefox.ps1` sends it to Mozilla's store). Before that: unzip `spiderpet-firefox-unsigned.zip`, open `about:debugging` → This Firefox → **Load Temporary Add-on**, and pick its `manifest.json` (it lasts until Firefox restarts).
4. Open a page. In the SpiderPet window, type what you are looking for: the spider drops onto that page and hunts for it. Or press **Start spider**. The spider button in the browser toolbar (**Alt + Shift + S**) does the same.

The app sits in the tray. Close its window and it keeps working; quit from the tray icon. The browser connects by itself whenever the app is running.

## Build from source

Needs Visual Studio 2022 (C++), CMake 3.21+ and [vcpkg](https://vcpkg.io) (set `VCPKG_ROOT`). The libraries are listed in `vcpkg.json` (ImGui with 32-bit characters for emoji, nlohmann/json); the first build installs them into `build\`.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

That builds `SpiderPet.exe`, `SpiderHost.exe` and the extension (`extension\chromium`, `extension\firefox`).

```
extension/src   the spider in the page: reading, walking, hunting, restyling, badges
src/app         SpiderPet.exe (window, tray, library), SpiderHost.exe (the browser's bridge)
src/mind        the checker: Crossref, OpenLibrary, PubMed, arXiv, Wikipedia, and the local model
src/core        small helpers
```

The browser starts SpiderHost.exe (native messaging); it relays everything over a private named pipe to SpiderPet.exe, which owns the library, the checks and the controls. When the app is not running, SpiderHost waits for it, so the app can send the spider out the moment you start it.

## Privacy

The extension sends what it harvests to SpiderPet.exe on your own PC. SpiderPet.exe sends each find (never the page) to the public sources above when Online checks are on, and talks to Ollama on `127.0.0.1`. The library stays in `Documents\SpiderPet`, settings in `%APPDATA%\SpiderPet`.

## Credits

- Inspired by the node-and-line spider in [this clip by @Ufosoldier](https://x.com/Ufosoldier/status/2105902567846756439).
- Page in the screenshots: the Wikipedia article [Spider](https://en.wikipedia.org/wiki/Spider), CC BY-SA 4.0.
- [Dear ImGui](https://github.com/ocornut/imgui) Win32/DX11 backends (MIT), [nlohmann/json](https://github.com/nlohmann/json) (MIT).
- Lookups: [Crossref](https://www.crossref.org), [OpenLibrary](https://openlibrary.org), [Google Books](https://books.google.com), [NCBI E-utilities](https://www.ncbi.nlm.nih.gov/books/NBK25501/), [arXiv](https://arxiv.org), [Wikipedia](https://www.wikipedia.org).

## License

[MIT](LICENSE)
