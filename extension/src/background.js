// SpiderPet background: one connection to the SpiderPet app (through
// SpiderHost.exe, native messaging), and the go-between for the spider in
// each tab and the side panel.
'use strict';

const api = globalThis.browser ?? globalThis.chrome;
const IS_FIREFOX = typeof globalThis.browser !== 'undefined' && navigator.userAgent.includes('Firefox');
const HOST = 'com.spiderpet.host';

let port = null;
let reconnectTimer = null;
const app = { connected: false, error: '', settings: {}, status: {}, version: '' };
// Per tab: the page the spider is on, what it found, and what the app said.
const tabs = new Map(); // tabId -> { url, title, finds: Map(id -> find), gist, spider: bool }
const pendingReveal = new Map(); // tabId -> find id, for pages opened from the library
let lastSpiderTab = null; // the panel falls back to it when the active tab is not a web page

function cleanUrl(u) {
  try {
    const x = new URL(u);
    x.hash = '';
    return x.href;
  } catch {
    return u || '';
  }
}

function browserName() {
  if (IS_FIREFOX) return 'Firefox';
  const brands = navigator.userAgentData?.brands?.map((b) => b.brand) ?? [];
  if (navigator.brave || brands.includes('Brave')) return 'Brave';
  if (brands.some((b) => b.includes('Edge'))) return 'Edge';
  if (brands.some((b) => b.includes('Chrome'))) return 'Chrome';
  return 'Chromium';
}

function tabState(tabId) {
  let t = tabs.get(tabId);
  if (!t) {
    t = { url: '', title: '', finds: new Map(), gist: null, spider: false };
    tabs.set(tabId, t);
  }
  return t;
}

// ------------------------------------------------------------- the app

function connect() {
  if (port) return true;
  try {
    port = api.runtime.connectNative(HOST);
  } catch (e) {
    port = null;
    app.connected = false;
    app.error = String(e?.message || e);
    tellPanels();
    return false;
  }
  port.onMessage.addListener(fromApp);
  port.onDisconnect.addListener((p) => {
    const err = api.runtime.lastError?.message || p?.error?.message || '';
    port = null;
    app.connected = false;
    app.error = err.includes('not found') || err.includes('No such native application')
      ? 'SpiderPet is not installed for this browser. Start SpiderPet.exe once.'
      : err || 'Lost the connection to SpiderPet.';
    tellPanels();
    tellSpiders({ type: 'app', app });
    clearTimeout(reconnectTimer);
    if ([...tabs.values()].some((t) => t.spider)) reconnectTimer = setTimeout(connect, 4000);
  });
  port.postMessage({ type: 'hello', browser: browserName(), version: api.runtime.getManifest().version });
  return true;
}

function toApp(msg) {
  if (!connect()) return false;
  try {
    port.postMessage(msg);
    return true;
  } catch {
    return false;
  }
}

function tabsFor(url) {
  const out = [];
  for (const [id, t] of tabs) if (t.url === url) out.push(id);
  return out;
}

function sendTab(tabId, msg) {
  api.tabs.sendMessage(tabId, msg).catch(() => {});
}

function tellSpiders(msg) {
  for (const [id, t] of tabs) if (t.spider) sendTab(id, msg);
}

function tellPanels(extra) {
  api.runtime.sendMessage({ type: 'panel-update', ...(extra || {}) }).catch(() => {});
}

function fromApp(m) {
  switch (m.type) {
    case 'state':
    case 'settings':
      app.connected = true;
      app.error = '';
      if (m.settings) app.settings = m.settings;
      if (m.status) app.status = m.status;
      if (m.app) app.version = m.app;
      tellSpiders({ type: 'app', app });
      tellPanels();
      break;
    case 'verdict':
    case 'scores':
    case 'known':
    case 'gist': {
      const url = cleanUrl(m.url);
      for (const id of tabsFor(url)) {
        const t = tabs.get(id);
        if (m.type === 'verdict') {
          const f = t.finds.get(m.id);
          if (f) f.verdict = m.verdict;
        } else if (m.type === 'scores') {
          for (const [fid, s] of Object.entries(m.scores || {})) {
            const f = t.finds.get(fid);
            if (f) f.score = s;
          }
        } else if (m.type === 'known') {
          for (const [fid, k] of Object.entries(m.items || {})) {
            const f = t.finds.get(fid);
            if (!f) continue;
            if (k.verdict) f.verdict = k.verdict;
            if (k.score >= 0) f.score = k.score;
          }
        } else {
          t.gist = m.gist;
        }
        sendTab(id, m);
      }
      tellPanels();
      break;
    }
    case 'reveal':
      reveal(cleanUrl(m.url), m.id, true);
      break;
    case 'error':
      app.error = m.text || 'SpiderPet reported a problem.';
      tellPanels();
      break;
  }
}

// ------------------------------------------------------------- the spider

async function injectSpider(tabId) {
  const t = tabState(tabId);
  t.spider = true;
  saveSpiderTabs();
  connect();
  try {
    // Already there (asleep or awake)? Just make sure it is awake.
    const [probe] = await api.scripting.executeScript({
      target: { tabId },
      func: () => (window.__spiderpet ? (window.__spiderpet.running() ? 'running' : 'asleep') : 'none'),
    });
    if (probe?.result === 'running') return true;
    if (probe?.result === 'asleep') {
      await api.scripting.executeScript({ target: { tabId }, func: () => window.__spiderpet.start() });
      return true;
    }
    await api.scripting.insertCSS({ target: { tabId }, files: ['content.css'] });
    await api.scripting.executeScript({ target: { tabId }, files: ['content.js'] });
    return true;
  } catch (e) {
    t.spider = false;
    saveSpiderTabs();
    app.error = "The spider can't go on this page (browser pages and the extension store are off limits).";
    tellPanels();
    return false;
  }
}

async function toggleSpider(tabId) {
  const t = tabState(tabId);
  if (t.spider) {
    t.spider = false;
    saveSpiderTabs();
    sendTab(tabId, { type: 'stop' });
    tellPanels();
    return false;
  }
  return injectSpider(tabId);
}

// Open (or find) the page, bring the spider, and let it point at the find.
async function reveal(url, id, fromApp = false) {
  const all = await api.tabs.query({});
  const hit = all.find((t) => cleanUrl(t.url) === url);
  if (hit) {
    await api.tabs.update(hit.id, { active: true });
    await api.windows.update(hit.windowId, { focused: true });
    const t = tabState(hit.id);
    if (!t.spider) {
      pendingReveal.set(hit.id, id);
      await injectSpider(hit.id);
    } else {
      sendTab(hit.id, { type: 'reveal', id });
    }
  } else if (fromApp && url.startsWith('http')) {
    const tab = await api.tabs.create({ url, active: true });
    pendingReveal.set(tab.id, id);
    tabState(tab.id).spider = true;
    saveSpiderTabs();
  }
}

// The spider follows you to the next page in the same tab.
function saveSpiderTabs() {
  const ids = [...tabs].filter(([, t]) => t.spider).map(([id]) => id);
  api.storage.session?.set({ spiderTabs: ids }).catch?.(() => {});
}

async function loadSpiderTabs() {
  try {
    const got = await api.storage.session.get('spiderTabs');
    for (const id of got.spiderTabs || []) tabState(id).spider = true;
  } catch {}
}

api.tabs.onUpdated.addListener((tabId, info, tab) => {
  const t = tabs.get(tabId);
  if (!t || !t.spider) return;
  if (info.status === 'complete' && tab.url?.startsWith('http')) {
    // A new page in a tab the spider was on: it follows you there.
    if (cleanUrl(tab.url) !== t.url) {
      t.finds = new Map();
      t.gist = null;
    }
    injectSpider(tabId);
  }
});

api.tabs.onRemoved.addListener((tabId) => {
  tabs.delete(tabId);
  pendingReveal.delete(tabId);
  saveSpiderTabs();
});

api.tabs.onActivated.addListener(() => tellPanels());

api.action.onClicked.addListener(async (tab) => {
  // Open the panel first: both browsers only allow it right inside the click.
  try {
    if (IS_FIREFOX) api.sidebarAction.open();
    else await api.sidePanel.open({ windowId: tab.windowId });
  } catch {}
  if (tab.id != null) toggleSpider(tab.id);
});

// ------------------------------------------------------------- messages

api.runtime.onMessage.addListener((m, sender, reply) => {
  const tabId = sender.tab?.id;
  switch (m.type) {
    case 'spider-hello': {
      lastSpiderTab = tabId;
      const t = tabState(tabId);
      t.spider = true;
      if (cleanUrl(m.url) !== t.url) {
        t.url = cleanUrl(m.url);
        t.finds = new Map();
        t.gist = null;
      }
      t.title = m.title;
      connect();
      reply({ app, reveal: pendingReveal.get(tabId) || null });
      pendingReveal.delete(tabId);
      tellPanels();
      return false;
    }
    case 'page': {
      const t = tabState(tabId);
      t.url = cleanUrl(m.url);
      t.title = m.title;
      toApp({ ...m, url: t.url });
      return false;
    }
    case 'finds': {
      const t = tabState(tabId);
      t.url = cleanUrl(m.url);
      for (const f of m.finds) {
        const old = t.finds.get(f.id);
        t.finds.set(f.id, { ...f, verdict: old?.verdict ?? null, score: old?.score ?? -1 });
      }
      // The app only needs what to check, not where it sits on the page.
      const finds = m.finds.map(({ id, kind, text, label, href, context }) => ({ id, kind, text, label, href, context }));
      toApp({ type: 'finds', url: t.url, title: m.title, lang: m.lang, finds });
      tellPanels();
      return false;
    }
    case 'eaten': {
      const t = tabs.get(tabId);
      const f = t?.finds.get(m.id);
      if (f) f.eaten = true;
      toApp({ type: 'soon', id: m.id });
      tellPanels();
      return false;
    }
    case 'stopped': {
      const t = tabs.get(tabId);
      if (t) t.spider = false;
      saveSpiderTabs();
      tellPanels();
      return false;
    }
    case 'check':
      toApp({ type: 'check', id: m.id });
      return false;

    // The side panel.
    case 'panel-get': {
      (async () => {
        let [tab] = await api.tabs.query({ active: true, currentWindow: true });
        if ((!tab || !tab.url?.startsWith('http')) && lastSpiderTab != null) tab = await api.tabs.get(lastSpiderTab).catch(() => tab);
        const t = tab ? tabs.get(tab.id) : null;
        reply({
          app,
          tabId: tab?.id ?? null,
          tabUrl: tab?.url ?? '',
          page: t ? { url: t.url, title: t.title, gist: t.gist, spider: t.spider, finds: [...t.finds.values()] } : null,
        });
      })();
      return true;
    }
    case 'panel-toggle':
      toggleSpider(m.tabId).then((on) => reply({ on }));
      return true;
    case 'panel-settings':
      toApp({ type: 'settings', settings: m.settings });
      return false;
    case 'panel-reveal':
      if (m.tabId != null) sendTab(m.tabId, { type: 'reveal', id: m.id });
      return false;
    case 'panel-open-app':
      toApp({ type: 'open-app' });
      return false;
    case 'panel-connect':
      connect();
      return false;
  }
  return false;
});

if (!IS_FIREFOX) api.sidePanel?.setPanelBehavior?.({ openPanelOnActionClick: false }).catch(() => {});
loadSpiderTabs();
