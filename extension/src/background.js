// SpiderPet background: one connection to the SpiderPet app (through
// SpiderHost.exe, native messaging). The app is the control center: it says
// where the spider goes and what it hunts. This script tells the app which
// tab you are on and carries messages between the app and the spider in each tab.
'use strict';

const api = globalThis.browser ?? globalThis.chrome;
const IS_FIREFOX = typeof globalThis.browser !== 'undefined' && navigator.userAgent.includes('Firefox');
const HOST = 'com.spiderpet.host';

let port = null;
let retryTimer = null;
// What the app said last. goal and terms steer the hunt in every tab.
const app = { connected: false, error: '', settings: {}, status: {}, version: '', goal: '', terms: [] };
const spiderTabs = new Set();
const tabUrls = new Map(); // tabId -> the page its spider is on
const pendingReveal = new Map(); // tabId -> find id, for pages opened from the library
let current = null; // the tab you are looking at: { tabId, url, title, web }

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

// ------------------------------------------------------------- the app

// SpiderHost waits for the app when it is not running, so this connection is
// always open and the app can send the spider out the moment you start it.
// start: true asks SpiderHost to start the app in the tray (you pressed the
// spider button); false only waits for it.
function connect(start = false) {
  if (port) {
    if (start && !app.connected) send({ type: 'wake' });
    return true;
  }
  clearTimeout(retryTimer);
  try {
    port = api.runtime.connectNative(HOST);
  } catch (e) {
    port = null;
    app.error = String(e?.message || e);
    retryTimer = setTimeout(connect, 30000);
    return false;
  }
  port.onMessage.addListener(fromApp);
  port.onDisconnect.addListener((p) => {
    const err = api.runtime.lastError?.message || p?.error?.message || '';
    port = null;
    const missing = err.includes('not found') || err.includes('No such native application');
    app.connected = false;
    app.error = missing ? 'SpiderPet is not installed for this browser. Start SpiderPet.exe once.' : err;
    tellSpiders({ type: 'app', app });
    clearTimeout(retryTimer);
    retryTimer = setTimeout(connect, missing ? 30000 : 1500);
  });
  port.postMessage({ type: 'hello', browser: browserName(), version: api.runtime.getManifest().version, start });
  return true;
}

function send(msg) {
  if (!port) return false;
  try {
    port.postMessage(msg);
    return true;
  } catch {
    return false;
  }
}

// Any message to an app that is not running yet wakes it, so the tab report
// waits until the app says hello.
function toApp(msg) {
  if (!port) connect();
  return send(msg);
}

function sendTab(tabId, msg) {
  api.tabs.sendMessage(tabId, msg).catch(() => {});
}

function tellSpiders(msg) {
  for (const id of spiderTabs) sendTab(id, msg);
}

function fromApp(m) {
  switch (m.type) {
    case 'state':
    case 'settings': {
      const first = !app.connected;
      app.connected = true;
      app.error = '';
      if (m.settings) app.settings = m.settings;
      if (m.status) app.status = m.status;
      if (m.app) app.version = m.app;
      if (m.goal !== undefined) app.goal = m.goal;
      if (m.terms) app.terms = m.terms;
      tellSpiders({ type: 'app', app });
      if (first) report(true);
      break;
    }
    case 'goal':
      setGoal(m.goal, m.terms);
      break;
    case 'hunt':
      // You typed a search in the app: the spider goes to the page you are on.
      setGoal(m.goal, m.terms);
      if (current?.web) injectSpider(current.tabId);
      break;
    case 'spider': {
      const tabId = m.tabId ?? current?.tabId;
      if (tabId == null) break;
      if (m.on) injectSpider(tabId);
      else stopSpider(tabId);
      break;
    }
    case 'verdict':
    case 'scores':
    case 'known':
    case 'gist':
    case 'answer':
    case 'blockers': {
      const url = cleanUrl(m.url);
      for (const [id, u] of tabUrls) if (u === url) sendTab(id, m);
      break;
    }
    case 'reveal':
      reveal(cleanUrl(m.url), m.id);
      break;
    case 'error':
      app.error = m.text || 'SpiderPet reported a problem.';
      break;
  }
}

function setGoal(goal, terms) {
  app.goal = goal || '';
  app.terms = app.goal ? terms || [] : [];
  tellSpiders({ type: 'goal', goal: app.goal, terms: app.terms });
}

// ------------------------------------------------------------- where you are

let reportTimer = null;
function report(now = false) {
  clearTimeout(reportTimer);
  reportTimer = setTimeout(async () => {
    try {
      const [tab] = await api.tabs.query({ active: true, lastFocusedWindow: true });
      if (!tab) return;
      const url = cleanUrl(tab.url || '');
      current = { tabId: tab.id, url, title: tab.title || '', web: /^https?:/.test(url) };
      if (app.connected) send({ type: 'tab', ...current, spider: spiderTabs.has(tab.id) });
    } catch {}
  }, now ? 0 : 150);
}

api.tabs.onActivated.addListener(() => report());
api.windows.onFocusChanged.addListener((w) => {
  if (w !== api.windows.WINDOW_ID_NONE) report();
});
api.tabs.onUpdated.addListener((tabId, info, tab) => {
  if (tab.active && (info.status === 'complete' || info.title || info.url)) report();
  if (!spiderTabs.has(tabId) || info.status !== 'complete' || !/^https?:/.test(tab.url || '')) return;
  // A new page in a tab the spider was on: it follows you there.
  injectSpider(tabId);
});
api.tabs.onRemoved.addListener((tabId) => {
  spiderTabs.delete(tabId);
  tabUrls.delete(tabId);
  pendingReveal.delete(tabId);
  saveSpiderTabs();
});

// ------------------------------------------------------------- the spider

async function injectSpider(tabId) {
  spiderTabs.add(tabId);
  saveSpiderTabs();
  try {
    // Already there (asleep or awake)? Just make sure it is awake.
    const [probe] = await api.scripting.executeScript({
      target: { tabId },
      func: () => (window.__spiderpet ? (window.__spiderpet.running() ? 'running' : 'asleep') : 'none'),
    });
    if (probe?.result === 'none') {
      await api.scripting.insertCSS({ target: { tabId }, files: ['content.css'] });
      await api.scripting.executeScript({ target: { tabId }, files: ['content.js'] });
    } else if (probe?.result === 'asleep') {
      await api.scripting.executeScript({ target: { tabId }, func: () => window.__spiderpet.start() });
    } else {
      sendTab(tabId, { type: 'goal', goal: app.goal, terms: app.terms });
    }
    report(true);
    return true;
  } catch {
    spiderTabs.delete(tabId);
    saveSpiderTabs();
    toApp({ type: 'notice', text: "The spider can't go on this page (browser pages and the extension store are off limits)." });
    report(true);
    return false;
  }
}

function stopSpider(tabId) {
  spiderTabs.delete(tabId);
  saveSpiderTabs();
  sendTab(tabId, { type: 'stop' });
  report(true);
}

// Open (or find) the page, bring the spider, and let it point at the find.
async function reveal(url, id) {
  const all = await api.tabs.query({});
  const hit = all.find((t) => cleanUrl(t.url) === url);
  if (hit) {
    await api.tabs.update(hit.id, { active: true });
    await api.windows.update(hit.windowId, { focused: true });
    if (!spiderTabs.has(hit.id)) {
      pendingReveal.set(hit.id, id);
      await injectSpider(hit.id);
    } else {
      sendTab(hit.id, { type: 'reveal', id });
    }
  } else if (url.startsWith('http')) {
    const tab = await api.tabs.create({ url, active: true });
    pendingReveal.set(tab.id, id);
    spiderTabs.add(tab.id);
    saveSpiderTabs();
  }
}

// The spider follows you to the next page in the same tab, even after the
// browser put this script to sleep.
function saveSpiderTabs() {
  api.storage.session?.set({ spiderTabs: [...spiderTabs] }).catch?.(() => {});
}

async function loadSpiderTabs() {
  try {
    const got = await api.storage.session.get('spiderTabs');
    for (const id of got.spiderTabs || []) spiderTabs.add(id);
  } catch {}
}

// The toolbar button: a shortcut for the app's Start/Stop button.
api.action.onClicked.addListener((tab) => {
  if (tab.id == null) return;
  connect(true);
  if (spiderTabs.has(tab.id)) stopSpider(tab.id);
  else injectSpider(tab.id);
});

// ------------------------------------------------------------- the spider's messages

api.runtime.onMessage.addListener((m, sender, reply) => {
  const tabId = sender.tab?.id;
  if (tabId == null) return false;
  switch (m.type) {
    case 'spider-hello':
      spiderTabs.add(tabId);
      tabUrls.set(tabId, cleanUrl(m.url));
      saveSpiderTabs();
      connect();
      reply({ app, reveal: pendingReveal.get(tabId) || null });
      pendingReveal.delete(tabId);
      return false;
    case 'page':
      tabUrls.set(tabId, cleanUrl(m.url));
      toApp({ ...m, url: cleanUrl(m.url) });
      return false;
    case 'finds':
      tabUrls.set(tabId, cleanUrl(m.url));
      toApp({ type: 'finds', url: cleanUrl(m.url), title: m.title, lang: m.lang, finds: m.finds });
      return false;
    case 'eaten':
      toApp({ type: 'soon', id: m.id });
      return false;
    case 'stopped':
      spiderTabs.delete(tabId);
      saveSpiderTabs();
      report(true);
      return false;
    case 'check':
      toApp({ type: 'check', id: m.id });
      return false;
    case 'blockers':
      // Things floating over the page: the app's AI says which are popups.
      if (!app.connected) reply({ fallback: true });
      else toApp({ type: 'blockers', url: cleanUrl(m.url), items: m.items });
      return false;
  }
  return false;
});

// Firefox puts this script to sleep when it is idle; the alarm brings it
// back to reconnect. Chrome keeps it awake while the connection is open.
api.alarms?.create('spiderpet-keepalive', { periodInMinutes: 0.5 });
api.alarms?.onAlarm.addListener(() => {
  if (!port) connect();
});
api.runtime.onStartup.addListener(() => connect());
loadSpiderTabs().then(() => connect());
