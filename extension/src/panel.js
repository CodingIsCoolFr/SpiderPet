// The side panel: what the spider found on this tab, with the app's checks.
'use strict';
const api = globalThis.browser ?? globalThis.chrome;
const $ = (id) => document.getElementById(id);

const KIND_COLOR = { sentence: '#E64CF2', heading: '#E64CF2', title: '#F2836B', doi: '#93F5AE', isbn: '#7D8BFF', id: '#4E8FF0', link: '#7FD6FF' };
const PILL = {
  verified: 'verified', mismatch: 'wrong', not_found: 'not found', unverified: 'unclear',
  opinion: 'opinion', promo: 'ad', queued: 'checking', checking: 'checking', error: 'not checked',
};
const CHECKABLE = new Set(['doi', 'isbn', 'id', 'title', 'sentence']);
let state = null;
let filter = 'all';
let goalTyping = false;

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}

function renderStatus(app) {
  const s = $('status');
  s.textContent = '';
  const item = (ok, text) => {
    const span = el('span');
    span.append(el('span', 'dot ' + (ok ? 'ok' : 'no')), text);
    s.append(span);
  };
  item(app.connected, app.connected ? `app ${app.version || ''} connected` : 'app not connected');
  if (app.connected) {
    const ai = app.status?.ollama && app.status?.model;
    item(ai, ai ? `AI: ${app.status.model}` : app.status?.ollama ? 'AI: no model' : 'AI: Ollama is off');
    item(app.settings?.online !== false, app.settings?.online !== false ? 'online checks on' : 'online checks off');
  }
  if (app.error) s.append(el('span', 'err', app.error));
}

function matches(f) {
  const st = f.verdict?.status;
  switch (filter) {
    case 'problems': return st === 'mismatch' || st === 'not_found';
    case 'verified': return st === 'verified';
    case 'ids': return ['doi', 'isbn', 'id', 'title'].includes(f.kind);
    case 'sentence': return f.kind === 'sentence';
    default: return true;
  }
}

function render() {
  if (!state) return;
  renderStatus(state.app);
  const page = state.page;
  const on = !!page?.spider;
  const t = $('toggle');
  t.textContent = on ? 'Stop the spider' : 'Drop the spider here';
  t.classList.toggle('stop', on);
  if (!goalTyping && document.activeElement !== $('goal')) $('goal').value = state.app.settings?.goal || '';

  const g = $('gist');
  g.textContent = '';
  if (page?.gist?.summary) {
    g.hidden = false;
    g.append(el('div', 'type', page.gist.type || ''), el('div', '', page.gist.summary));
    if (page.gist.points?.length) {
      const ul = el('ul');
      for (const p of page.gist.points) ul.append(el('li', '', p));
      g.append(ul);
    }
  } else g.hidden = true;

  const list = $('list');
  list.textContent = '';
  const finds = (page?.finds || []).slice();
  // What you are after first; problems before quiet finds; then page order.
  finds.sort((a, b) => (b.score ?? -1) - (a.score ?? -1) || bad(b) - bad(a) || (a.page ? -1 : 0) - (b.page ? -1 : 0) || a.order - b.order);
  let verified = 0, problems = 0;
  for (const f of finds) {
    if (f.verdict?.status === 'verified') verified++;
    if (bad(f)) problems++;
  }
  $('counts').textContent = finds.length ? `${finds.length} finds · ${verified} verified · ${problems} problems` : '';
  $('empty').hidden = finds.length > 0;
  for (const f of finds) {
    if (!matches(f)) continue;
    list.append(row(f));
  }
}

function bad(f) {
  const st = f.verdict?.status;
  return st === 'mismatch' || st === 'not_found' ? 1 : 0;
}

function row(f) {
  const r = el('div', 'find');
  const top = el('div', 'top');
  const kind = el('span', 'kind', f.kind === 'id' && f.label ? f.label : f.page ? `${f.kind} · this page` : f.kind);
  kind.style.color = KIND_COLOR[f.kind] || '#C8CCD8';
  top.append(kind);
  const st = f.verdict?.status;
  if (st && PILL[st]) top.append(el('span', 'pill ' + st, PILL[st]));
  if (f.score >= 0) top.append(el('span', 'rel', `relevance ${f.score}/10`));
  r.append(top, el('div', 'text' + (f.eaten ? ' eaten' : ''), f.text));
  const v = f.verdict;
  if (v?.note && st !== 'queued' && st !== 'checking') r.append(el('div', 'note', v.note));
  if (v?.quote) r.append(el('div', 'quote', `“${v.quote}”`));
  if (v?.source?.url) {
    const a = el('a', '', `source: ${v.source.name}${v.source.title ? ' — ' + v.source.title : ''}`);
    a.href = v.source.url;
    a.target = '_blank';
    a.rel = 'noopener';
    a.addEventListener('click', (e) => e.stopPropagation());
    r.append(a);
  }
  if (f.href) {
    const a = el('a', '', f.href.length > 70 ? f.href.slice(0, 70) + '…' : f.href);
    a.href = f.href;
    a.target = '_blank';
    a.rel = 'noopener';
    a.addEventListener('click', (e) => e.stopPropagation());
    r.append(el('div'), a);
  }
  if (CHECKABLE.has(f.kind)) {
    const act = el('div', 'actions');
    const b = el('button', '', st && st !== 'queued' && st !== 'checking' ? 'Check again' : 'Check now');
    b.addEventListener('click', (e) => {
      e.stopPropagation();
      api.runtime.sendMessage({ type: 'check', id: f.id });
    });
    act.append(b);
    r.append(act);
  }
  if (!f.page) r.addEventListener('click', () => api.runtime.sendMessage({ type: 'panel-reveal', tabId: state.tabId, id: f.id }));
  return r;
}

let pending = null;
function refresh() {
  if (pending) return;
  pending = setTimeout(async () => {
    pending = null;
    try {
      state = await api.runtime.sendMessage({ type: 'panel-get' });
      render();
    } catch {}
  }, 120);
}

$('toggle').addEventListener('click', async () => {
  if (state?.tabId == null) return;
  await api.runtime.sendMessage({ type: 'panel-toggle', tabId: state.tabId });
  refresh();
});
$('openApp').addEventListener('click', () => api.runtime.sendMessage({ type: 'panel-open-app' }));
$('goal').addEventListener('input', () => (goalTyping = true));
$('goal').addEventListener('change', () => {
  goalTyping = false;
  api.runtime.sendMessage({ type: 'panel-settings', settings: { goal: $('goal').value.trim() } });
});
$('filters').addEventListener('click', (e) => {
  const f = e.target?.dataset?.f;
  if (!f) return;
  filter = f;
  for (const b of $('filters').querySelectorAll('button')) b.classList.toggle('on', b.dataset.f === f);
  render();
});

api.runtime.onMessage.addListener((m) => {
  if (m.type === 'panel-update') refresh();
});
api.runtime.sendMessage({ type: 'panel-connect' });
refresh();
setInterval(refresh, 4000);
