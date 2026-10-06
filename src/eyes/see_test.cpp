// SpiderSee.exe "<part of a window title>" [ocr]
// Prints what the spider's eyes see in that window: the app, the text, the
// links, what can be clicked. A test tool; SpiderPet itself does not need it.
#include "eyes/eyes.hpp"
#include "eyes/shot.hpp"
#include "core/util.hpp"

#include <cstdio>
#include <string>

using namespace sp;

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (argc < 2) {
    printf("usage: SpiderSee \"<title part>\" [ocr]\n");
    return 1;
  }
  struct Find {
    std::wstring want;
    HWND h = nullptr;
  } f{argv[1]};
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* f = reinterpret_cast<Find*>(lp);
    wchar_t t[512] = {};
    GetWindowTextW(h, t, 512);
    if (IsWindowVisible(h) && wcsstr(t, f->want.c_str()) && !eyes::is_ours(h)) {
      f->h = h;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&f));
  if (!f.h) {
    printf("no window\n");
    return 1;
  }
  eyes::AppInfo a = eyes::identify(f.h);
  printf("APP name=%s exe=%s class=%s engine=%s browser=%d\nTITLE %s\n", a.name.c_str(), a.exe.c_str(), a.cls.c_str(), a.engine.c_str(), a.browser,
         a.title.c_str());
  eyes::Eyes e;
  std::string err;
  double t0 = now_seconds();
  if (!e.attach(f.h, 8000, &err)) {
    printf("attach failed: %s\n", err.c_str());
    return 1;
  }
  double t1 = now_seconds();
  eyes::Look l = e.look();
  double t2 = now_seconds();
  printf("ATTACH %.2fs LOOK %.2fs ok=%d how=%s elements=%zu error=%s\nURL %s\nTITLE %s LANG %s\n", t1 - t0, t2 - t1, l.ok, l.how.c_str(), l.elements,
         l.error.c_str(), l.url.c_str(), l.title.c_str(), l.lang.c_str());
  printf("BLOCKS %zu\n", l.blocks.size());
  for (size_t i = 0; i < l.blocks.size() && i < 40; ++i)
    printf("  [%zu]%s (%.0f,%.0f %.0fx%.0f) %.150s\n", i, l.heading[i] ? (" h" + std::to_string(l.heading[i])).c_str() : "", l.spans[i].x, l.spans[i].y,
           l.spans[i].w, l.spans[i].h, l.blocks[i].c_str());
  printf("LINKS %zu\n", l.links.size());
  for (size_t i = 0; i < l.links.size() && i < 15; ++i) printf("  %.60s | %.90s\n", l.links[i].text.c_str(), l.links[i].href.c_str());
  printf("CONTROLS %zu:", l.controls.size());
  for (size_t i = 0; i < l.controls.size() && i < 40; ++i) printf(" [%s]", l.controls[i].c_str());
  printf("\n");
  double t3 = now_seconds();
  auto items = e.items(60);
  printf("ITEMS %zu (%.2fs)\n", items.size(), now_seconds() - t3);
  for (auto& it : items)
    printf("  %d %s%s%s \"%.60s\" %s%s%s%s\n", it.i, it.kind.c_str(), it.in_view ? "" : " (off)", it.secret ? " SECRET" : "", it.label.c_str(),
           it.value.empty() ? "" : ("=" + it.value).c_str(), it.row.empty() ? "" : (" row: " + it.row.substr(0, 60)).c_str(),
           it.href.empty() ? "" : (" -> " + it.href.substr(0, 50)).c_str(), it.dialog >= 0 ? (" [pop-up " + std::to_string(it.dialog) + "]").c_str() : "");
  auto dl = e.dialogs();
  printf("POP-UPS %zu\n", dl.size());
  for (size_t i = 0; i < dl.size(); ++i)
    printf("  %zu \"%.40s\" (%.0f,%.0f %.0fx%.0f) %.160s\n", i, dl[i].label.c_str(), dl[i].r.x, dl[i].r.y, dl[i].r.w, dl[i].r.h, dl[i].text.c_str());
  if (argc > 2 && std::wstring(argv[2]) == L"ocr") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    double t4 = now_seconds();
    eyes::Pixels px = eyes::grab(f.h);
    auto lines = eyes::read_pixels(px);
    std::string jpg = eyes::jpeg_base64(px);
    printf("OCR %zu lines (%.2fs) picture %dx%d jpeg %zu KB\n", lines.size(), now_seconds() - t4, px.w, px.h, jpg.size() * 3 / 4 / 1024);
    for (size_t i = 0; i < lines.size() && i < 25; ++i) printf("  %.120s\n", lines[i].text.c_str());
  }
  return 0;
}
