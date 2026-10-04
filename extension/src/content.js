// SpiderPet in the page: reads the real page (the DOM, so nothing is guessed),
// walks to what matters with eight jointed legs, lassoes it, restyles the real
// text and shows the check the SpiderPet app made for it.
(() => {
  'use strict';
  const api = globalThis.browser ?? globalThis.chrome;
  // Injected again (the page finished loading, or you pressed the button):
  // wake up if asleep, never switch off. Only the Stop button stops it.
  if (window.__spiderpet) {
    window.__spiderpet.start();
    return;
  }

  // ------------------------------------------------------------------ utils

  const now = () => performance.now() / 1000;
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const lerp = (a, b, t) => a + (b - a) * t;
  const rand = (a, b) => a + Math.random() * (b - a);
  const pick = (arr) => arr[Math.floor(Math.random() * arr.length)];
  const V = (x, y) => ({ x, y });
  const add = (a, b) => V(a.x + b.x, a.y + b.y);
  const sub = (a, b) => V(a.x - b.x, a.y - b.y);
  const mul = (a, k) => V(a.x * k, a.y * k);
  const len = (a) => Math.hypot(a.x, a.y);
  const dist = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
  const norm = (a) => {
    const l = len(a);
    return l > 1e-6 ? V(a.x / l, a.y / l) : V(0, 0);
  };
  const perp = (a) => V(-a.y, a.x);
  const fromAngle = (r) => V(Math.cos(r), Math.sin(r));
  const wrapPi = (r) => {
    while (r > Math.PI) r -= 2 * Math.PI;
    while (r < -Math.PI) r += 2 * Math.PI;
    return r;
  };
  const easeInOut = (t) => (t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2);

  function fnv(s) {
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
      h ^= s.charCodeAt(i);
      h = Math.imul(h, 0x01000193);
    }
    return (h >>> 0).toString(36);
  }

  function isbnOk(raw) {
    const d = raw.replace(/[^0-9Xx]/g, '').toUpperCase();
    if (d.length === 10) {
      let s = 0;
      for (let i = 0; i < 10; i++) s += (d[i] === 'X' ? (i === 9 ? 10 : NaN) : +d[i]) * (10 - i);
      return s % 11 === 0;
    }
    if (d.length === 13 && !d.includes('X')) {
      let s = 0;
      for (let i = 0; i < 13; i++) s += +d[i] * (i % 2 ? 3 : 1);
      return s % 10 === 0;
    }
    return false;
  }

  const pageUrl = () => location.href.split('#')[0];
  const squash = (s) => s.replace(/\s+/g, ' ').trim();
  const stripRefs = (s) => squash(s.replace(/\[(?:\d+|[a-z]|citation needed|note \d+)\]/gi, ''));

  // ------------------------------------------------------------------ reading

  // Furniture, not content. References stay: they hold the DOIs and titles.
  const SKIP =
    'nav, header, footer, aside, form, button, select, textarea, input, label, script, style, noscript, template, svg, canvas, ' +
    '[role=navigation], [role=banner], [role=contentinfo], [role=complementary], [role=search], [role=dialog], [aria-hidden=true], ' +
    '.navbox, .vertical-navbox, .sidebar, .mw-editsection, .toc, #toc, .mw-jump-link, .hatnote, .metadata, .noprint, .catlinks, ' +
    '[class*=cookie], [id*=cookie], [class*=consent], [class*=newsletter], [class*=advert], [class*=sponsor], [class*=promo], ' +
    '[id^=ad-], [class^=ad-], .ad, .ads, .share, [class*=social], .spiderpet-badge, #spiderpet-host';
  const skipMemo = new WeakMap();
  function skipped(el) {
    if (!el || el.nodeType !== 1) return false;
    let v = skipMemo.get(el);
    if (v === undefined) {
      v = !!el.closest(SKIP);
      skipMemo.set(el, v);
    }
    return v;
  }

  function shown(el) {
    if (!el || !el.getClientRects().length) return false;
    const cs = getComputedStyle(el);
    if (cs.visibility === 'hidden' || cs.display === 'none' || +cs.opacity === 0) return false;
    const r = el.getBoundingClientRect();
    // Screen-reader-only text is squeezed into a 1-pixel box.
    return r.width > 2 && r.height > 2;
  }

  function mainRoot() {
    const cands = [...document.querySelectorAll(
      '#mw-content-text, main, article, [role=main], #content, .post-content, .entry-content, .article-body, .article__body, #main'
    )].filter(shown);
    let best = null;
    let most = 0;
    for (const c of cands) {
      const n = (c.innerText || '').length;
      if (n > most * 1.15) {
        most = n;
        best = c;
      }
    }
    return best && most > 400 ? best : document.body;
  }

  // The citation or paragraph a find sits in: what the checker compares against.
  function contextOf(node) {
    const el = node.nodeType === 1 ? node : node.parentElement;
    const box = el?.closest('cite, .citation, .csl-entry, li, p, td, dd, figcaption, blockquote') || el;
    return squash(box?.innerText || box?.textContent || '').slice(0, 700);
  }

  function rangeShown(range) {
    const r = range.getBoundingClientRect();
    return r.width > 1 && r.height > 1 && shown(range.startContainer.parentElement);
  }

  const KIND_PRIO = { doi: 3, isbn: 3, id: 2.6, title: 2.4, sentence: 1.4, heading: 1.2, link: 0.8 };

  // Everything worth harvesting on the page, with where it is.
  function scan() {
    const url = pageUrl();
    const root = mainRoot();
    const out = [];
    const byId = new Map();
    let order = 0;
    const put = (f) => {
      f.text = squash(f.text);
      if (!f.text) return null;
      f.id = 'f' + fnv(`${f.kind}|${f.label || ''}|${f.text}|${url}`);
      const first = byId.get(f.id);
      if (first) {
        // Cited twice: restyle both, harvest once.
        if (f.el || f.range) (first.more ||= []).push(f);
        return null;
      }
      f.order = order++;
      byId.set(f.id, f);
      out.push(f);
      return f;
    };
    const used = new WeakSet();

    // The page's own citation (journal and book pages publish it in <meta>).
    const meta = (n) => document.querySelector(`meta[name="${n}"], meta[property="${n}"]`)?.content?.trim() || '';
    const ctitle = meta('citation_title') || meta('dc.title') || meta('DC.title');
    const authors = [...document.querySelectorAll('meta[name="citation_author"]')].map((m) => m.content).slice(0, 4).join('; ');
    const pageCtx = squash(`${authors} ${ctitle} ${meta('citation_journal_title')} ${meta('citation_publication_date') || meta('citation_date')}`);
    const mdoi = (meta('citation_doi') || meta('dc.identifier') || meta('DC.identifier')).replace(/^(doi:|https?:\/\/(dx\.)?doi\.org\/)/i, '');
    if (/^10\.\d{4,9}\//.test(mdoi)) put({ kind: 'doi', text: mdoi, context: pageCtx, page: true });
    const misbn = meta('citation_isbn') || meta('book:isbn');
    if (misbn && isbnOk(misbn)) put({ kind: 'isbn', label: 'ISBN', text: misbn, context: pageCtx, page: true });
    if (ctitle && ctitle.split(/\s+/).length >= 3) put({ kind: 'title', text: ctitle, context: pageCtx, page: true });

    // Ids that are links (Wikipedia, most reference lists).
    for (const a of root.querySelectorAll('a[href]')) {
      if (skipped(a) || !shown(a)) continue;
      const href = a.href;
      let m;
      let f = null;
      if ((m = href.match(/doi\.org\/(10\.\d{4,9}\/[^?#\s]+)/i))) {
        f = { kind: 'doi', text: decodeURIComponent(m[1]).replace(/[.,;]+$/, '') };
      } else if ((m = href.match(/pubmed\.ncbi\.nlm\.nih\.gov\/(\d{4,9})/))) {
        f = { kind: 'id', label: 'PMID', text: m[1] };
      } else if ((m = href.match(/(?:ncbi\.nlm\.nih\.gov\/pmc\/articles|pmc\.ncbi\.nlm\.nih\.gov\/articles)\/PMC(\d+)/i))) {
        f = { kind: 'id', label: 'PMC', text: m[1] };
      } else if ((m = href.match(/arxiv\.org\/abs\/([^?#\s]+)/i))) {
        f = { kind: 'id', label: 'arXiv', text: m[1] };
      } else if ((m = href.match(/Special:BookSources\/([\dXx-]{10,17})/) || href.match(/openlibrary\.org\/isbn\/([\dXx-]{10,17})/i))) {
        if (isbnOk(m[1])) f = { kind: 'isbn', label: 'ISBN', text: m[1] };
      }
      if (!f) continue;
      // A title that links to its paper is a title, not an id: the id has its own link nearby.
      const shownText = (a.textContent || '').trim();
      const digits = f.text.replace(/\D/g, '');
      if (shownText.split(/\s+/).length >= 4 && !(digits && shownText.replace(/\D/g, '').includes(digits))) continue;
      used.add(a);
      put({ ...f, el: a, context: contextOf(a) });
    }

    // Ids written as plain text.
    const ID_RX = [
      ['doi', '', /\b10\.\d{4,9}\/[^\s"'<>]+[^\s"'<>.,;:)\]]/g],
      ['isbn', 'ISBN', /\bISBN(?:-1[03])?:?\s*((?:97[89][-\s]?)?\d{1,5}[-\s]?\d{1,7}[-\s]?\d{1,7}[-\s]?[\dXx])\b/g],
      ['id', 'PMID', /\bPMID:?\s*(\d{4,9})\b/g],
      ['id', 'PMC', /\bPMC\s?(\d{5,9})\b/g],
      ['id', 'arXiv', /\barXiv:\s*(\d{4}\.\d{4,5}(?:v\d+)?)/gi],
    ];
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
      const t = n.nodeValue;
      if (t.length < 6 || !/\d/.test(t)) continue;
      const pe = n.parentElement;
      if (!pe || skipped(pe) || pe.closest('a') && used.has(pe.closest('a'))) continue;
      for (const [kind, label, rx] of ID_RX) {
        rx.lastIndex = 0;
        let m;
        while ((m = rx.exec(t))) {
          const value = m[1] || m[0];
          if (kind === 'isbn' && !isbnOk(value)) continue;
          const start = m.index + m[0].indexOf(value);
          const range = document.createRange();
          range.setStart(n, start);
          range.setEnd(n, start + value.length);
          if (!rangeShown(range)) continue;
          put({ kind, label, text: value, range, context: contextOf(n) });
        }
      }
    }

    // Titles in citations: the quoted article title, or the book title in italics.
    for (const c of root.querySelectorAll('cite, .csl-entry, .citation:not(cite)')) {
      if (skipped(c) || !shown(c)) continue;
      let el = null;
      for (const a of c.querySelectorAll('a, span, q')) {
        const tx = (a.textContent || '').trim();
        if (/^["“].{10,250}["”]$/.test(tx)) {
          el = a;
          break;
        }
      }
      if (!el && /book/.test(c.className)) el = c.querySelector('i');
      if (!el) {
        // A quoted run inside plain text: find it as a range.
        const tw = document.createTreeWalker(c, NodeFilter.SHOW_TEXT);
        for (let n = tw.nextNode(); n; n = tw.nextNode()) {
          const m = n.nodeValue.match(/["“]([^"”]{10,250})["”]/);
          if (!m) continue;
          const range = document.createRange();
          range.setStart(n, m.index);
          range.setEnd(n, m.index + m[0].length);
          put({ kind: 'title', text: m[1], range, context: contextOf(c) });
          break;
        }
        continue;
      }
      const text = (el.textContent || '').replace(/^["“\s]+|["”.\s]+$/g, '');
      if (text.split(/\s+/).length >= 2) put({ kind: 'title', text, el, context: contextOf(c) });
    }

    // Headings: the shape of the page.
    for (const h of document.querySelectorAll('h1, h2, h3')) {
      if (!(root.contains(h) || h.tagName === 'H1') || skipped(h) || !shown(h)) continue;
      const text = (h.innerText || '').replace(/\[\s*edit[^\]]*\]/gi, '').trim();
      if (text.length >= 3 && text.length <= 140) put({ kind: 'heading', text, el: h });
    }

    // Key sentences: the one or two sentences that carry each paragraph.
    const titleWords = new Set(document.title.toLowerCase().split(/[^\p{L}\p{N}]+/u).filter((w) => w.length > 3));
    const seg = typeof Intl.Segmenter === 'function' ? new Intl.Segmenter(document.documentElement.lang || 'en', { granularity: 'sentence' }) : null;
    const sentences = [];
    let pIndex = 0;
    for (const p of root.querySelectorAll('p')) {
      if (skipped(p) || !shown(p) || p.closest('cite, .references, .reflist, table, blockquote')) continue;
      const nodes = [];
      let raw = '';
      const tw = document.createTreeWalker(p, NodeFilter.SHOW_TEXT);
      for (let n = tw.nextNode(); n; n = tw.nextNode()) {
        if (skipped(n.parentElement)) continue;
        nodes.push({ n, at: raw.length });
        raw += n.nodeValue;
      }
      if (raw.trim().length < 100) continue;
      const parts = seg ? [...seg.segment(raw)].map((s) => ({ at: s.index, s: s.segment })) : splitSentences(raw);
      const cands = [];
      parts.forEach((part, i) => {
        const text = stripRefs(part.s);
        const lt = text.toLowerCase();
        if (text.length < 40 || text.length > 420 || !/[.!?]["”')\]]*$/.test(text)) return;
        let s = text.length >= 60 && text.length <= 260 ? 1 : 0.35;
        if (i === 0) s += 0.7;
        let hits = 0;
        for (const w of titleWords) if (lt.includes(w)) hits++;
        s += Math.min(1.5, 0.5 * hits);
        if (/ (is|are|was|were) (a|an|the) /.test(lt)) s += 0.35;
        if (/\d/.test(text) && !/^\d/.test(text)) s += 0.3;
        if (/^(it|this|these|they|he|she|that|such)\b/.test(lt)) s -= 0.3;
        if (/(cookie|subscribe|sign in|log in|newsletter|javascript)/.test(lt)) s = -9;
        const startAt = part.at + (part.s.length - part.s.trimStart().length);
        const endAt = part.at + part.s.trimEnd().length;
        cands.push({ text, s, startAt, endAt });
      });
      cands.sort((a, b) => b.s - a.s);
      const keep = raw.length > 900 ? 3 : raw.length > 450 ? 2 : 1;
      for (const c of cands.slice(0, keep)) {
        if (c.s < 0.9) break;
        const range = rangeFor(nodes, c.startAt, c.endAt);
        if (range) sentences.push({ kind: 'sentence', text: c.text, range, el: p, key: c.s >= 2.2, score0: c.s + (pIndex < 3 ? 0.4 : 0) });
      }
      pIndex++;
    }
    // Enough to read, not the whole book.
    sentences.sort((a, b) => b.score0 - a.score0);
    const keepSentences = new Set(sentences.slice(0, 30));
    for (const s of sentences) if (keepSentences.has(s)) put(s);

    // Links people would follow: real words, real addresses, in the text.
    let links = 0;
    for (const a of root.querySelectorAll('p a[href], li a[href], dd a[href]')) {
      if (links >= 30) break;
      if (used.has(a) || skipped(a) || a.closest('cite, .references, .reflist') || !shown(a)) continue;
      const text = (a.innerText || '').trim();
      if (!/^https?:/.test(a.href) || a.getAttribute('href').startsWith('#')) continue;
      if (text.split(/\s+/).length < 2 && text.length < 10) continue;
      if (put({ kind: 'link', text, href: a.href, el: a })) links++;
    }
    return out;
  }

  function splitSentences(raw) {
    const out = [];
    const rx = /[^.!?]+[.!?]+["”')\]]*\s*/g;
    let m;
    while ((m = rx.exec(raw))) out.push({ at: m.index, s: m[0] });
    return out;
  }

  // A DOM range for characters [a, b) of a paragraph built from text nodes.
  function rangeFor(nodes, a, b) {
    let sn = null, so = 0, en = null, eo = 0;
    for (const { n, at } of nodes) {
      const l = n.nodeValue.length;
      if (!sn && a < at + l) {
        sn = n;
        so = a - at;
      }
      if (b <= at + l) {
        en = n;
        eo = b - at;
        break;
      }
    }
    if (!sn || !en) return null;
    const r = document.createRange();
    try {
      r.setStart(sn, so);
      r.setEnd(en, eo);
    } catch {
      return null;
    }
    return r;
  }

  // Where a find is right now, in page coordinates (the page may have reflowed).
  function boxOf(f) {
    const r = (f.node || f.el)?.isConnected && f.kind !== 'sentence' ? (f.node || f.el).getBoundingClientRect() : f.range?.getBoundingClientRect();
    if (!r || r.width < 1 || r.height < 1) return null;
    return { x: r.left + scrollX, y: r.top + scrollY, w: r.width, h: r.height };
  }

  // ------------------------------------------------------------------ the spider

  // Measured off the reference clip: hips in the front half, front legs reach
  // forward, back legs trail, every knee bends outward.
  const ALONG = [0.36, 0.15, -0.06, -0.27];
  const ANGLE = [0.55, 1.22, 1.95, 2.62];
  const REST = [0.95, 0.84, 0.86, 0.98];
  const SEG = [0.44, 0.41, 0.37];

  class Spider {
    constructor(scale) {
      this.pos = V(0, 0);
      this.vel = V(0, 0);
      this.heading = Math.PI / 2;
      this.goal = null;
      this.maxSpeed = 0;
      this.arrive = 1;
      this.airborne = false;
      this.time = 0;
      this.still = 0;
      this.burstT = 0;
      this.bursting = true;
      this.legs = [];
      this.configure(scale);
    }
    configure(s) {
      this.scale = s;
      this.bodyLen = 50 * s;
      this.bodyWid = 11.5 * s;
      this.reach = 118 * s;
      this.legs = [];
      for (let side = 0; side < 2; side++)
        for (let i = 0; i < 4; i++)
          this.legs.push({
            side: side === 0 ? -1 : 1, index: i, along: ALONG[i] * this.bodyLen, angle: ANGLE[i], rest: REST[i] * this.reach,
            pole: ANGLE[i] + (Math.PI / 2 - ANGLE[i]) * 0.4, seg: SEG.map((k) => k * this.reach),
            foot: V(0, 0), from: V(0, 0), to: V(0, 0), j: [V(0, 0), V(0, 0), V(0, 0), V(0, 0)],
            stepping: false, t: 0, dur: 0.15, lift: 0, init: false, phase: rand(0.1, 6.2), fv: V(0, 0),
          });
    }
    reachOf(l) { return l.seg[0] + l.seg[1] + l.seg[2]; }
    hip(l) {
      const f = fromAngle(this.heading);
      return add(add(this.pos, mul(f, l.along)), mul(perp(f), l.side * this.bodyWid * 0.5));
    }
    restPoint(l, curl = 1) { return add(this.pos, mul(fromAngle(this.heading + l.side * l.angle), l.rest * curl)); }
    head() { return add(this.pos, mul(fromAngle(this.heading), this.bodyLen * 0.5 + 3 * this.scale)); }
    speed() { return len(this.vel); }
    place(p, heading = this.heading) {
      this.pos = p;
      this.vel = V(0, 0);
      this.heading = heading;
      for (const l of this.legs) {
        l.foot = this.restPoint(l, this.airborne ? 0.55 : 1);
        l.stepping = false;
        l.init = false;
        this.solve(l);
      }
    }
    setGoal(p, maxSpeed, arrive) {
      this.goal = p;
      this.maxSpeed = maxSpeed;
      this.arrive = Math.max(1, arrive);
    }
    neighborStepping(l) {
      return this.legs.some((o) => o !== l && o.stepping && ((o.side === l.side && Math.abs(o.index - l.index) === 1) || (o.side !== l.side && o.index === l.index)));
    }
    tick(dt) {
      dt = clamp(dt, 0, 0.05);
      this.time += dt;
      const s = this.scale;
      if (!this.airborne && this.goal) {
        const d = sub(this.goal, this.pos);
        const dd = len(d);
        let want = this.maxSpeed * clamp(dd / this.arrive, 0, 1);
        // Real spiders run in bursts: long trips get short freezes.
        if (dd > 150 * s) {
          this.burstT -= dt;
          if (this.burstT <= 0) {
            this.bursting = !this.bursting;
            this.burstT = this.bursting ? rand(0.35, 0.95) : rand(0.07, 0.2);
          }
          if (!this.bursting) want *= 0.1;
        } else this.bursting = true;
        const desired = dd > 0.4 ? mul(norm(d), want) : V(0, 0);
        const dv = sub(desired, this.vel);
        const acc = 1500 * s * dt;
        const l = len(dv);
        this.vel = l > acc ? add(this.vel, mul(dv, acc / l)) : desired;
      } else if (!this.airborne) {
        this.vel = mul(this.vel, Math.exp(-9 * dt));
      }
      this.pos = add(this.pos, mul(this.vel, dt));
      const sp = this.speed();
      let want = this.heading;
      if (sp > 18 * s) want = Math.atan2(this.vel.y, this.vel.x);
      else if (this.face != null) want = this.face;
      const rate = (sp > 18 * s ? 7.5 : 4.5) * dt;
      this.heading = wrapPi(this.heading + clamp(wrapPi(want - this.heading), -rate, rate));
      this.still = sp < 20 * s ? this.still + dt : 0;
      let stepping = this.legs.filter((l) => l.stepping).length;
      for (const l of this.legs) stepping = this.updateLeg(l, dt, stepping);
    }
    updateLeg(l, dt, stepping) {
      const s = this.scale;
      const h = this.hip(l);
      if (this.airborne) {
        const wig = Math.sin(this.time * 13 + l.phase * 5) * 0.32;
        const target = add(h, mul(fromAngle(this.heading + l.side * (l.angle + wig)), l.rest * 0.55));
        l.fv = mul(add(l.fv, mul(sub(target, l.foot), 170 * dt)), Math.exp(-16 * dt));
        l.foot = add(l.foot, mul(l.fv, dt));
        l.lift = 0.6;
        this.solve(l);
        return stepping;
      }
      if (l.stepping) {
        l.t += dt / l.dur;
        const k = easeInOut(Math.min(1, l.t));
        const base = V(lerp(l.from.x, l.to.x, k), lerp(l.from.y, l.to.y, k));
        l.lift = Math.sin(Math.PI * Math.min(1, l.t));
        l.foot = add(base, mul(norm(sub(base, this.pos)), l.lift * 7 * s));
        if (l.t >= 1) {
          l.foot = l.to;
          l.stepping = false;
          l.lift = 0;
          stepping--;
        }
        this.solve(l);
        return stepping;
      }
      const sp = this.speed();
      const moving = sp > 20 * s;
      const want = add(this.restPoint(l), mul(this.vel, moving ? 0.2 : 0));
      const err = dist(l.foot, want);
      const over = dist(l.foot, h) > this.reachOf(l) * 0.97;
      const threshold = moving ? this.reach * 0.34 : this.reach * 0.2;
      const settle = !moving && this.still > 0.3;
      if ((err > threshold && (moving || settle)) || over) {
        if (over || (stepping < 4 && !this.neighborStepping(l))) {
          l.stepping = true;
          l.from = l.foot;
          l.to = add(want, V(rand(-5, 5) * s, rand(-5, 5) * s));
          l.t = 0;
          l.dur = clamp(0.17 - sp / (2600 * s), 0.075, 0.17);
          stepping++;
        }
      }
      this.solve(l);
      return stepping;
    }
    // FABRIK on a three-segment chain, knees pulled toward a pole beside the body.
    solve(l) {
      const j = l.j;
      const h = this.hip(l);
      const target = l.foot;
      const tt = sub(target, h);
      const tl = len(tt);
      const pole = add(add(h, mul(fromAngle(this.heading + l.side * l.pole), this.reach * 0.6)), mul(norm(tt), tl * 0.25));
      j[0] = h;
      if (!l.init) {
        j[1] = V(lerp(h.x, pole.x, 0.7), lerp(h.y, pole.y, 0.7));
        j[2] = V(lerp(pole.x, target.x, 0.5), lerp(pole.y, target.y, 0.5));
        j[3] = target;
        l.init = true;
      }
      j[1] = V(lerp(j[1].x, pole.x, 0.3), lerp(j[1].y, pole.y, 0.3));
      const mid = V(lerp(pole.x, target.x, 0.6), lerp(pole.y, target.y, 0.6));
      j[2] = V(lerp(j[2].x, mid.x, 0.15), lerp(j[2].y, mid.y, 0.15));
      if (tl >= this.reachOf(l)) {
        const n = norm(tt);
        j[1] = add(h, mul(n, l.seg[0]));
        j[2] = add(j[1], mul(n, l.seg[1]));
        j[3] = add(j[2], mul(n, l.seg[2]));
        return;
      }
      for (let it = 0; it < 10; it++) {
        j[3] = target;
        for (let i = 2; i >= 0; i--) j[i] = add(j[i + 1], mul(norm(sub(j[i], j[i + 1])), l.seg[i]));
        j[0] = h;
        for (let i = 1; i <= 3; i++) j[i] = add(j[i - 1], mul(norm(sub(j[i], j[i - 1])), l.seg[i - 1]));
        if (dist(j[3], target) < 0.25) break;
      }
    }
  }

  // ------------------------------------------------------------------ persona

  const short = (s, n) => (s.length <= n ? s : s.slice(0, n - 1) + '…');
  const say = {
    land: () => pick(['new habitat.', 'touching down.', 'host acquired.', 'tasting the surface…']),
    sense: () => pick(['sensing…', 'probing.', 'scanning substrate.', 'signals…']),
    crawl: () => pick(['crawling deeper.', 'more substrate below.', 'descending the page.']),
    drained: () => pick(['habitat drained.', 'nothing left in reach.']),
    more: () => pick(['more below. scroll me.', 'page goes on. scroll?', 'hungry. more below.']),
    lifted: () => pick(['!! displaced', 'lifted. hostile?', 'put me down.']),
    landed: () => pick(['relocated.', 'recalibrating…', 'new coordinates.']),
    offline: () => 'no link to the hive. start SpiderPet.exe',
    eat(f) {
      const t = short(f.text, 34);
      switch (f.kind) {
        case 'doi': return pick([`doi: ${t}`, `signal doi: ${t}`]);
        case 'isbn': return pick([`isbn: ${t}`, `book spore: ${t}`]);
        case 'id': return `${(f.label || 'id').toLowerCase()}: ${t}`;
        case 'title': return pick([`tasting: ${t}`, `title: ${t}`, `ooh: ${t}`]);
        case 'heading': return pick([`new region: ${t.toLowerCase()}`, `territory: ${t.toLowerCase()}`]);
        case 'link': {
          let host = '';
          try { host = new URL(f.href).hostname.replace(/^www\./, ''); } catch {}
          return `${pick(['tasting', 'eating', 'sampling'])}: ${short(f.text, 24)}${host ? ' → ' + host : ''}`;
        }
        default: return pick(['parsing…', 'ingesting text…', 'reading…']);
      }
    },
    verdict(f, v) {
      const t = short(f.text, 26);
      switch (v.status) {
        case 'verified': return pick([`confirmed: ${t}`, `tastes real: ${t}`, 'verified. nutritious.']);
        case 'mismatch': return pick([`poison! ${t} is wrong`, 'spoiled: the source disagrees.', 'that one lies.']);
        case 'not_found': return pick([`${t}: does not exist`, 'phantom prey. not real.']);
        case 'opinion': return 'just an opinion. spat out.';
        case 'promo': return 'advertising. inedible.';
        default: return null;
      }
    },
  };

  // ------------------------------------------------------------------ drawing

  const host = document.createElement('div');
  host.id = 'spiderpet-host';
  host.style.cssText = 'all:initial;position:fixed;inset:0;pointer-events:none;z-index:2147483646;';
  const shadow = host.attachShadow({ mode: 'closed' });
  shadow.innerHTML = `
    <style>
      canvas { position: fixed; inset: 0; width: 100vw; height: 100vh; pointer-events: none; }
      .bubble { position: fixed; max-width: 320px; padding: 6px 11px; border-radius: 12px; font: 500 13px/1.35 "Cascadia Mono", Consolas, ui-monospace, monospace;
        background: rgba(12,14,28,.93); color: #F4F6FF; border: 1.5px solid #DA3CEC; box-shadow: 0 4px 18px rgba(0,0,0,.35);
        transition: opacity .4s; opacity: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; pointer-events: none; }
      .bubble.light { background: rgba(255,255,255,.96); color: #14151C; border-color: #C42AD8; }
      .grab { position: fixed; width: 44px; height: 44px; margin: -22px 0 0 -22px; border-radius: 50%; pointer-events: auto; cursor: grab; }
      .grab:active { cursor: grabbing; }
    </style>
    <canvas></canvas><div class="bubble"></div><div class="grab" title="SpiderPet: drag me"></div>`;
  const canvas = shadow.querySelector('canvas');
  const ctx = canvas.getContext('2d');
  const bubble = shadow.querySelector('.bubble');
  const grab = shadow.querySelector('.grab');

  let dark = false;
  let pal = null;
  function readTheme() {
    const bgOf = (el) => {
      const c = getComputedStyle(el).backgroundColor.match(/[\d.]+/g);
      if (!c || (c.length === 4 && +c[3] === 0)) return null;
      return (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255;
    };
    const l = bgOf(document.body) ?? bgOf(document.documentElement) ?? (matchMedia('(prefers-color-scheme: dark)').matches ? 0.1 : 1);
    dark = l < 0.45;
    document.documentElement.toggleAttribute('data-spiderpet-dark', dark);
    bubble.classList.toggle('light', !dark);
    pal = dark
      ? { leg: '#EE7A68', joint: '#8BF5A6', body: '#6173F2', fill: 'rgba(10,12,26,.92)', head: '#E33CD2', tether: '#DA3CEC', silk: 'rgba(232,238,255,.25)', read: 'rgba(230,76,242,.22)' }
      : { leg: '#E0604C', joint: '#22C463', body: '#4152E0', fill: 'rgba(16,18,42,.9)', head: '#D02CC0', tether: '#C42AD8', silk: 'rgba(20,24,40,.22)', read: 'rgba(196,42,216,.16)' };
  }

  let dpr = 1;
  function resize() {
    dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(innerWidth * dpr);
    canvas.height = Math.round(innerHeight * dpr);
  }

  function draw(t) {
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, innerWidth, innerHeight);
    ctx.translate(-scrollX, -scrollY);
    const s = spider;
    const k = s.scale;
    // Silk thread while it rappels or hangs from your hand.
    if (mode === 'drop' || mode === 'held') {
      ctx.strokeStyle = pal.silk;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(s.pos.x, mode === 'drop' ? scrollY : s.pos.y - 300 * k);
      ctx.lineTo(s.pos.x, s.pos.y);
      ctx.stroke();
    }
    // Sentences being read, when the page cannot highlight them itself.
    if (!HIGHLIGHTS && readingRange) {
      ctx.fillStyle = pal.read;
      for (const r of readingRange.getClientRects()) ctx.fillRect(r.left + scrollX, r.top + scrollY, r.width, r.height);
    }
    // The lasso.
    if (tether && tether.amount > 0) {
      const b = boxOf(tether.f);
      if (b) {
        const from = s.head();
        const to = V(clamp(from.x, b.x, b.x + b.w), clamp(from.y, b.y, b.y + b.h));
        const end = V(lerp(from.x, to.x, tether.amount), lerp(from.y, to.y, tether.amount));
        ctx.strokeStyle = pal.tether;
        ctx.lineWidth = Math.max(1.2, 1.6 * k);
        ctx.setLineDash(tether.amount >= 1 ? [] : [5 * k, 4 * k]);
        ctx.beginPath();
        ctx.moveTo(from.x, from.y);
        ctx.lineTo(end.x, end.y);
        ctx.stroke();
        ctx.setLineDash([]);
        if (tether.amount >= 1) {
          const pulse = 0.55 + 0.45 * Math.sin(t * 9);
          ctx.globalAlpha = pulse;
          ctx.strokeRect(b.x - 3, b.y - 2, b.w + 6, b.h + 4);
          ctx.globalAlpha = 1;
        }
      }
    }
    // Legs, joints, body, head: the node-and-line look of the reference clip.
    ctx.lineCap = 'round';
    ctx.strokeStyle = pal.leg;
    ctx.lineWidth = Math.max(1.1, 1.9 * k);
    ctx.beginPath();
    for (const l of s.legs) {
      ctx.moveTo(l.j[0].x, l.j[0].y);
      for (let i = 1; i < 4; i++) ctx.lineTo(l.j[i].x, l.j[i].y);
    }
    ctx.stroke();
    const r = Math.max(1.8, 3.1 * k);
    ctx.fillStyle = pal.joint;
    for (const l of s.legs)
      for (let i = 1; i < 4; i++) {
        ctx.beginPath();
        ctx.arc(l.j[i].x, l.j[i].y, i === 3 ? r * (1 + 0.35 * l.lift) : r, 0, Math.PI * 2);
        ctx.fill();
      }
    ctx.save();
    ctx.translate(s.pos.x, s.pos.y);
    ctx.rotate(s.heading);
    ctx.fillStyle = pal.fill;
    ctx.strokeStyle = pal.body;
    ctx.lineWidth = Math.max(1.2, 1.7 * k);
    ctx.fillRect(-s.bodyLen / 2, -s.bodyWid / 2, s.bodyLen, s.bodyWid);
    ctx.strokeRect(-s.bodyLen / 2, -s.bodyWid / 2, s.bodyLen, s.bodyWid);
    ctx.restore();
    const hd = s.head();
    ctx.fillStyle = pal.head;
    ctx.beginPath();
    ctx.arc(hd.x, hd.y, Math.max(2.2, 3.7 * k), 0, Math.PI * 2);
    ctx.fill();

    // The thought bubble rides above the spider.
    const bx = clamp(s.pos.x - scrollX - 20, 8, innerWidth - 340);
    const by = clamp(s.pos.y - scrollY - s.reach * 0.9 - 30, 8, innerHeight - 40);
    bubble.style.left = bx + 'px';
    bubble.style.top = by + 'px';
    bubble.style.opacity = t - thoughtAt < 3.6 ? '1' : '0';
    grab.style.left = s.pos.x - scrollX + 'px';
    grab.style.top = s.pos.y - scrollY + 'px';
  }

  let thoughtAt = -100;
  function think(text) {
    if (!text) return;
    bubble.textContent = text;
    thoughtAt = now();
  }

  // ------------------------------------------------------------------ the page's look

  const HIGHLIGHTS = typeof CSS !== 'undefined' && CSS.highlights && typeof Highlight === 'function';
  const eatenSentences = HIGHLIGHTS ? new Highlight() : null;
  const readingHighlight = HIGHLIGHTS ? new Highlight() : null;
  if (HIGHLIGHTS) {
    CSS.highlights.set('spiderpet-eaten', eatenSentences);
    CSS.highlights.set('spiderpet-reading', readingHighlight);
  }
  let readingRange = null;

  function glitch(node) {
    if (!node) return;
    node.classList.remove('spiderpet-glitch');
    void node.offsetWidth;
    node.classList.add('spiderpet-glitch');
    setTimeout(() => node.classList.remove('spiderpet-glitch'), 520);
  }

  // Restyle the real text, the way the clip does: no overlay, the page itself changes.
  function restyle(f) {
    const all = [f, ...(f.more || [])];
    for (const x of all) {
      if (x.styled) {
        glitch(x.node);
        continue;
      }
      x.styled = true;
      if (x.kind === 'sentence') {
        if (HIGHLIGHTS) eatenSentences.add(x.range);
        x.node = null;
        continue;
      }
      let node = x.el || null;
      if (x.range && !x.el) {
        try {
          const span = document.createElement('span');
          x.range.surroundContents(span);
          node = span;
        } catch {
          node = x.range.startContainer.parentElement;
        }
      }
      if (!node) continue;
      node.classList.add('spiderpet-on', 'spiderpet-' + x.kind);
      x.node = node;
      glitch(node);
    }
  }

  const BADGE = {
    verified: ['✓ verified', 'ok'],
    mismatch: ['⚠ wrong', 'bad'],
    not_found: ['✗ not found', 'bad'],
    unverified: ['? unclear', 'meh'],
    opinion: ['opinion', 'tag'],
    promo: ['ad', 'tag'],
    queued: ['checking…', 'wait'],
    checking: ['checking…', 'wait'],
    error: ['not checked', 'meh'],
  };
  const CHECKABLE = new Set(['doi', 'isbn', 'id', 'title', 'sentence']);

  function badge(f) {
    if (!f.eaten || !CHECKABLE.has(f.kind) || f.page) return;
    const v = f.verdict;
    const spec = v && BADGE[v.status];
    if (!spec) {
      f.badge?.remove();
      f.badge = null;
      return;
    }
    if (!f.badge || !f.badge.isConnected) {
      f.badge = document.createElement('span');
      f.badge.className = 'spiderpet-badge';
      f.badge.addEventListener('click', (e) => {
        e.preventDefault();
        e.stopPropagation();
        const u = f.verdict?.source?.url;
        if (u) window.open(u, '_blank', 'noopener');
      });
      try {
        if (f.kind === 'sentence') {
          const end = f.range.cloneRange();
          end.collapse(false);
          end.insertNode(f.badge);
        } else (f.node || f.el)?.after(f.badge);
      } catch {}
    }
    f.badge.textContent = spec[0];
    f.badge.dataset.tone = spec[1];
    const src = v.source?.name ? `\nsource: ${v.source.name}${v.source.title ? ' — ' + v.source.title : ''}` : '';
    const quote = v.quote ? `\n“${v.quote}”` : '';
    f.badge.title = `${v.note || ''}${quote}${src}${v.source?.url ? '\n(click to open the source)' : ''}`;
  }

  // ------------------------------------------------------------------ behaviour

  let finds = [];
  const byId = new Map();
  let app = { connected: false, settings: {} };
  let spider = null;
  let mode = 'drop'; // drop, walk, lasso, eat, rest, held, crawl
  let target = null;
  let tether = null;
  let modeAt = 0;
  let userScrolledAt = -100;
  let ourScrollAt = -100;
  let lastScanAt = 0;
  let running = false;
  let raf = 0;
  let lastT = now();
  let revealId = null;
  let lastUrl = pageUrl();
  let restingSaid = false;

  function setMode(m) {
    mode = m;
    modeAt = now();
  }

  function view() {
    return { x: scrollX, y: scrollY, w: innerWidth, h: innerHeight };
  }

  function rescan(sendAll) {
    lastScanAt = now();
    const found = scan();
    const fresh = [];
    for (const f of found) {
      const old = byId.get(f.id);
      if (old) {
        // Same find, maybe a new place in a re-rendered page.
        if (!old.styled) {
          old.el = f.el;
          old.range = f.range;
        }
        continue;
      }
      f.verdict = null;
      f.score = -1;
      byId.set(f.id, f);
      finds.push(f);
      fresh.push(f);
    }
    const list = sendAll ? finds : fresh;
    if (list.length)
      api.runtime.sendMessage({
        type: 'finds', url: pageUrl(), title: document.title, lang: document.documentElement.lang || '',
        finds: list.map(({ id, kind, text, label, href, context, page, key, order }) => ({ id, kind, text, label: label || '', href: href || '', context: context || '', page: !!page, key: !!key, order })),
      }).catch(() => {});
    return fresh.length;
  }

  function sendPage() {
    const root = mainRoot();
    const text = squash((root.innerText || '').replace(/\[\d+\]/g, '')).slice(0, 8000);
    api.runtime.sendMessage({ type: 'page', url: pageUrl(), title: document.title, lang: document.documentElement.lang || '', text }).catch(() => {});
  }

  // What next: the most valuable find near the spider, in view, in reading order.
  function chooseTarget() {
    const v = view();
    let best = null;
    let bestScore = -1e9;
    for (const f of finds) {
      if (f.eaten || f.page) continue;
      const b = boxOf(f);
      if (!b) continue;
      if (b.y + b.h < v.y + 4 || b.y > v.y + v.h - 20 || b.x > v.x + v.w || b.x + b.w < v.x) continue;
      let s = KIND_PRIO[f.kind] ?? 1;
      if (f.key) s += 0.8;
      if (f.score >= 0) s += f.score * 0.35 - 1.2; // the app knows what you are after
      s -= dist(spider.pos, V(b.x, b.y + b.h / 2)) / (900 * spider.scale);
      s -= (b.y - v.y) / 2400;
      if (s > bestScore) {
        bestScore = s;
        best = f;
      }
    }
    return best;
  }

  function approachPoint(f) {
    const b = boxOf(f);
    if (!b) return null;
    const k = spider.scale;
    // Stand just left of the start of the find, or above it if there is no room.
    let x = b.x - 26 * k;
    if (x - scrollX < spider.reach * 0.7) x = b.x + Math.min(b.w, 60 * k);
    const y = b.y + b.h / 2 + (x > b.x ? -spider.reach * 0.45 : 0);
    return V(clamp(x, scrollX + 20, scrollX + innerWidth - 20), clamp(y, scrollY + 20, scrollY + innerHeight - 20));
  }

  function atBottom() {
    return scrollY + innerHeight >= document.documentElement.scrollHeight - 8;
  }

  function step(t, dt) {
    const k = spider.scale;
    if (pageUrl() !== lastUrl) {
      // A single-page app moved on without a reload.
      lastUrl = pageUrl();
      reset();
      return;
    }
    // Left behind (you jumped down the page, or it scrolled away): drop back
    // in on a thread instead of walking the whole way.
    if (mode !== 'drop' && mode !== 'held') {
      const off = spider.pos.y < scrollY - 120 || spider.pos.y > scrollY + innerHeight + 120;
      if (off) {
        const x = clamp(spider.pos.x, scrollX + 80, scrollX + innerWidth - 80);
        spider.airborne = true;
        spider.place(V(x, scrollY - 40), Math.PI / 2);
        spider.goal = V(x, scrollY + clamp(innerHeight * 0.3, 160, 360));
        tether = null;
        readingRange = null;
        if (target && mode !== 'walk') target = null;
        setMode('drop');
        return;
      }
    }
    switch (mode) {
      case 'drop': {
        spider.airborne = true;
        spider.pos = V(spider.pos.x, Math.min(spider.goal.y, spider.pos.y + 900 * k * dt));
        if (spider.pos.y >= spider.goal.y - 1) {
          spider.airborne = false;
          spider.place(spider.pos, Math.PI / 2);
          think(app.connected ? say.land() : say.offline());
          setMode('walk');
          target = null;
        }
        break;
      }
      case 'held':
        break;
      case 'walk': {
        if (revealId) {
          const f = byId.get(revealId);
          revealId = null;
          if (f) {
            target = f;
            const b = boxOf(f);
            if (b) {
              ourScrollAt = t;
              window.scrollTo({ top: b.y - innerHeight * 0.4, behavior: 'smooth' });
            }
            f.eaten = false;
          }
        }
        if (!target || target.eaten) {
          target = chooseTarget();
          if (!target) {
            // Out of food in view: look again (lazy pages), then crawl down.
            if (t - lastScanAt > 2 && rescan(false)) break;
            const crawl = app.settings?.crawl !== false;
            if (crawl && !atBottom() && t - userScrolledAt > 4 && t - ourScrollAt > 1.2) {
              think(say.crawl());
              ourScrollAt = t;
              window.scrollBy({ top: innerHeight * 0.7, behavior: 'smooth' });
              spider.setGoal(V(spider.pos.x, scrollY + innerHeight * 0.75), 260 * k, 80 * k);
            } else if (t - ourScrollAt > 1.5 && !restingSaid) {
              restingSaid = true;
              think(atBottom() ? say.drained() : say.more());
              setMode('rest');
            }
            break;
          }
          restingSaid = false;
          if (Math.random() < 0.25) think(say.sense());
        }
        const goal = approachPoint(target);
        if (!goal) {
          target.eaten = true; // it vanished from the page
          target = null;
          break;
        }
        spider.setGoal(goal, 560 * k, 70 * k);
        const b = boxOf(target);
        const near = V(clamp(spider.pos.x, b.x, b.x + b.w), clamp(spider.pos.y, b.y, b.y + b.h));
        if (dist(spider.pos, near) < spider.reach * 1.05 || (dist(spider.pos, goal) < 12 * k && spider.speed() < 40 * k)) {
          spider.face = Math.atan2(near.y - spider.pos.y, near.x - spider.pos.x);
          tether = { f: target, amount: 0 };
          setMode('lasso');
        }
        break;
      }
      case 'lasso': {
        spider.setGoal(spider.pos, 0, 1);
        tether.amount = Math.min(1, (t - modeAt) / 0.22);
        if (tether.amount >= 1) {
          restyle(target);
          target.eaten = true;
          think(say.eat(target));
          api.runtime.sendMessage({ type: 'eaten', id: target.id }).catch(() => {});
          badge(target);
          if (target.kind === 'sentence') startReading(target);
          setMode('eat');
        }
        break;
      }
      case 'eat': {
        const hold = target.kind === 'sentence' ? clamp(target.text.length / 160, 0.7, 1.6) : 0.55;
        if (target.kind === 'sentence') readProgress(target, (t - modeAt) / hold);
        if (t - modeAt > hold) {
          readingRange = null;
          if (HIGHLIGHTS) readingHighlight.clear();
          tether = null;
          spider.face = null;
          target = null;
          setMode('walk');
        }
        break;
      }
      case 'rest': {
        spider.setGoal(spider.pos, 0, 1);
        // New things in view (you scrolled, the page grew): back to work.
        if (t - modeAt > 1.5 && (chooseTarget() || (t - lastScanAt > 3 && rescan(false)))) {
          restingSaid = false;
          setMode('walk');
        } else if (t - modeAt > 6 && app.settings?.crawl !== false && !atBottom() && t - userScrolledAt > 4) {
          setMode('walk');
        }
        break;
      }
    }
    spider.tick(dt);
  }

  // The sentence sweep: a highlight that grows as the spider reads.
  function startReading(f) {
    readingRange = f.range.cloneRange();
    readingRange.collapse(true);
  }
  function readProgress(f, p) {
    p = clamp(p, 0, 1);
    const full = f.range;
    const r = document.createRange();
    try {
      r.setStart(full.startContainer, full.startOffset);
      // Walk forward through the text nodes to the right point.
      const total = full.toString().length;
      let want = Math.round(total * p);
      const tw = document.createTreeWalker(full.commonAncestorContainer.nodeType === 1 ? full.commonAncestorContainer : full.commonAncestorContainer.parentNode, NodeFilter.SHOW_TEXT);
      let n = tw.currentNode = full.startContainer;
      let off = full.startOffset;
      while (n) {
        const room = (n === full.endContainer ? full.endOffset : n.nodeValue.length) - off;
        if (want <= room) {
          r.setEnd(n, off + want);
          break;
        }
        want -= room;
        if (n === full.endContainer) {
          r.setEnd(n, full.endOffset);
          break;
        }
        n = tw.nextNode();
        off = 0;
      }
    } catch {
      return;
    }
    readingRange = r;
    if (HIGHLIGHTS) {
      readingHighlight.clear();
      readingHighlight.add(r);
    }
  }

  function frame() {
    if (!running) return;
    const t = now();
    const dt = Math.min(0.05, t - lastT);
    lastT = t;
    try {
      step(t, dt);
      draw(t);
    } catch (e) {
      console.warn('SpiderPet:', e);
    }
    raf = requestAnimationFrame(frame);
  }

  // ------------------------------------------------------------------ input

  let dragOffset = V(0, 0);
  grab.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    grab.setPointerCapture(e.pointerId);
    dragOffset = sub(spider.pos, V(e.clientX + scrollX, e.clientY + scrollY));
    spider.airborne = true;
    tether = null;
    target = null;
    readingRange = null;
    setMode('held');
    think(say.lifted());
  });
  grab.addEventListener('pointermove', (e) => {
    if (mode !== 'held') return;
    spider.pos = add(V(e.clientX + scrollX, e.clientY + scrollY), dragOffset);
  });
  grab.addEventListener('pointerup', () => {
    if (mode !== 'held') return;
    spider.airborne = false;
    spider.place(spider.pos);
    think(say.landed());
    userScrolledAt = now();
    setMode('walk');
  });

  const markUser = () => {
    if (now() - ourScrollAt > 1.2) userScrolledAt = now();
  };
  const onKey = (e) => {
    if (['PageDown', 'PageUp', 'ArrowDown', 'ArrowUp', ' ', 'Home', 'End'].includes(e.key)) markUser();
    if (e.key === 'Escape' && e.shiftKey) stop();
  };

  // ------------------------------------------------------------------ messages

  function onMessage(m) {
    switch (m.type) {
      case 'app':
        app = m.app || app;
        break;
      case 'verdict': {
        const f = byId.get(m.id);
        if (!f) break;
        const before = f.verdict?.status;
        f.verdict = m.verdict;
        badge(f);
        if (f.eaten && before !== m.verdict?.status) {
          const line = say.verdict(f, m.verdict || {});
          if (line && mode !== 'drop') think(line);
        }
        break;
      }
      case 'known':
        for (const [id, k] of Object.entries(m.items || {})) {
          const f = byId.get(id);
          if (!f) continue;
          if (k.verdict) f.verdict = k.verdict;
          if (k.score >= 0) f.score = k.score;
          badge(f);
        }
        break;
      case 'scores':
        for (const [id, s] of Object.entries(m.scores || {})) {
          const f = byId.get(id);
          if (f) f.score = s;
        }
        break;
      case 'gist':
        if (m.gist?.summary) think('habitat: ' + short(m.gist.summary.toLowerCase().replace(/\.$/, ''), 44));
        break;
      case 'reveal':
        // Finish the bite in progress; the walk picks the request up next.
        revealId = m.id;
        if (mode === 'rest') setMode('walk');
        if (mode === 'walk') target = null;
        break;
      case 'stop':
        stop();
        break;
    }
  }

  // ------------------------------------------------------------------ life

  function reset() {
    finds = [];
    byId.clear();
    target = null;
    tether = null;
    readingRange = null;
    if (HIGHLIGHTS) {
      eatenSentences.clear();
      readingHighlight.clear();
    }
    hello();
  }

  function hello() {
    readTheme();
    api.runtime.sendMessage({ type: 'spider-hello', url: pageUrl(), title: document.title }).then((r) => {
      if (r?.app) app = r.app;
      if (r?.reveal) revealId = r.reveal;
      sendPage();
      rescan(true);
    }).catch(() => {
      rescan(false);
    });
  }

  function start() {
    running = true;
    document.documentElement.appendChild(host);
    resize();
    readTheme();
    const fs = parseFloat(getComputedStyle(mainRoot()).fontSize) || 16;
    spider = new Spider(clamp(fs / 16, 0.85, 1.5) * 0.55);
    const x = scrollX + clamp(innerWidth * 0.22, 120, 420);
    spider.place(V(x, scrollY - 40), Math.PI / 2);
    spider.goal = V(x, scrollY + clamp(innerHeight * 0.3, 160, 360));
    spider.airborne = true;
    setMode('drop');
    addEventListener('resize', resize);
    addEventListener('wheel', markUser, { passive: true });
    addEventListener('touchmove', markUser, { passive: true });
    addEventListener('keydown', onKey);
    api.runtime.onMessage.addListener(onMessage);
    hello();
    lastT = now();
    raf = requestAnimationFrame(frame);
  }

  function stop() {
    if (!running) return;
    running = false;
    cancelAnimationFrame(raf);
    host.remove();
    removeEventListener('resize', resize);
    removeEventListener('wheel', markUser);
    removeEventListener('touchmove', markUser);
    removeEventListener('keydown', onKey);
    api.runtime.onMessage.removeListener(onMessage);
    if (HIGHLIGHTS) readingHighlight.clear();
    api.runtime.sendMessage({ type: 'stopped' }).catch(() => {});
    // Restyled text and checks stay, so you can still read them.
  }

  window.__spiderpet = {
    start() {
      if (!running) start();
    },
    running: () => running,
  };
  start();
})();
