// SpiderPet in the page: reads the real page (the DOM, so nothing is guessed),
// walks to what matters with eight jointed legs, lassoes it, restyles the real
// text and shows the check the SpiderPet app made for it. With a search from
// the app it hunts: matching words light up and it goes only for matches.
(() => {
  'use strict';
  const api = globalThis.browser ?? globalThis.chrome;
  // Injected again (the page finished loading, or the app said go): wake up
  // if asleep, never switch off. Only a stop command stops it.
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
  const vlerp = (a, b, t) => V(lerp(a.x, b.x, t), lerp(a.y, b.y, t));
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
  const easeOutBack = (t) => 1 + 2.7 * Math.pow(t - 1, 3) + 1.7 * Math.pow(t - 1, 2);
  const damp = (rate, dt) => 1 - Math.exp(-rate * dt);

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
  // Reference marks ([13], [a], [citation needed]), and the halves of one a
  // sentence break can leave at either end ("13] The families…").
  const stripRefs = (s) =>
    squash(s.replace(/\[(?:\d+|[a-z]|citation needed|note \d+)\]/gi, '').replace(/^\s*(?:\d+|[a-z])\]\s*/i, '').replace(/\s*\[\d*$/, ''));

  // ------------------------------------------------------------------ the search

  let goal = '';
  let terms = []; // lowercase words and phrases; the app adds synonyms when the model has them

  function hits(text) {
    if (!terms.length || !text) return 0;
    const lt = text.toLowerCase();
    let n = 0;
    for (const t of terms) if (lt.includes(t)) n++;
    return n;
  }
  const matchOf = (f) => hits(f.text) * 2 + (f.kind === 'doi' || f.kind === 'isbn' || f.kind === 'id' ? hits(f.context) : 0);

  // ------------------------------------------------------------------ reading

  // Furniture, not content. References stay: they hold the DOIs and titles.
  const SKIP =
    'nav, header, footer, aside, form, button, select, textarea, input, label, script, style, noscript, template, svg, canvas, ' +
    '[role=navigation], [role=banner], [role=contentinfo], [role=complementary], [role=search], [role=dialog], [aria-hidden=true], ' +
    '.navbox, .vertical-navbox, .sidebar, .mw-editsection, .toc, #toc, .mw-jump-link, .hatnote, .metadata, .noprint, .catlinks, ' +
    '[class*=cookie], [id*=cookie], [class*=consent], [class*=newsletter], [class*=advert], [class*=sponsor], [class*=promo], ' +
    '[id^=ad-], [class^=ad-], .ad, .ads, .share, [class*=social], .spiderpet-badge, #spiderpet-host';
  let skipMemo = new WeakMap();
  function skipped(el) {
    if (!el || el.nodeType !== 1) return false;
    let v = skipMemo.get(el);
    if (v === undefined) {
      v = !!el.closest(SKIP);
      skipMemo.set(el, v);
    }
    return v;
  }

  // Native and cheap where the browser has it; screen-reader-only text is
  // squeezed into a 1-pixel box, so tiny boxes do not count.
  function shown(el) {
    if (!el) return false;
    if (el.checkVisibility && !el.checkVisibility({ checkOpacity: true, checkVisibilityCSS: true })) return false;
    const r = el.getBoundingClientRect();
    return r.width > 2 && r.height > 2;
  }

  function mainRoot() {
    const cands = [...document.querySelectorAll(
      '#mw-content-text, main, article, [role=main], #content, .post-content, .entry-content, .article-body, .article__body, #main'
    )].filter(shown);
    let best = null;
    let most = 0;
    for (const c of cands) {
      const n = (c.textContent || '').length;
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
    return squash(box?.textContent || '').slice(0, 700);
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
    const seen = new Map();
    let order = 0;
    const put = (f) => {
      f.text = squash(f.text);
      if (!f.text) return null;
      f.id = 'f' + fnv(`${f.kind}|${f.label || ''}|${f.text}|${url}`);
      const first = seen.get(f.id);
      if (first) {
        // Cited twice: restyle both, harvest once.
        if (f.el || f.range) (first.more ||= []).push(f);
        return null;
      }
      f.order = order++;
      seen.set(f.id, f);
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
      const href = a.href;
      if (!/doi\.org|ncbi\.nlm\.nih\.gov|arxiv\.org|BookSources|openlibrary\.org\/isbn/i.test(href)) continue;
      if (skipped(a) || !shown(a)) continue;
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
      if (!pe || skipped(pe) || (pe.closest('a') && used.has(pe.closest('a')))) continue;
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
      const text = (h.textContent || '').replace(/\[\s*edit[^\]]*\]/gi, '').trim();
      if (text.length >= 3 && text.length <= 140) put({ kind: 'heading', text, el: h });
    }

    // Key sentences: the one or two that carry each paragraph, and with a
    // search, every sentence that matches it.
    const titleWords = new Set(document.title.toLowerCase().split(/[^\p{L}\p{N}]+/u).filter((w) => w.length > 3));
    const seg = typeof Intl.Segmenter === 'function' ? new Intl.Segmenter(document.documentElement.lang || 'en', { granularity: 'sentence' }) : null;
    const sentences = [];
    let pIndex = 0;
    for (const p of root.querySelectorAll(terms.length ? 'p, li, dd, td, blockquote' : 'p')) {
      if (skipped(p) || p.closest('cite, .references, .reflist') || (!terms.length && p.closest('table, blockquote'))) continue;
      if (terms.length && p.tagName !== 'P' && p.querySelector('p, li')) continue; // its children are read instead
      const rawAll = p.textContent || '';
      if (rawAll.trim().length < (terms.length ? 30 : 100) || !shown(p)) continue;
      const nodes = [];
      let raw = '';
      const tw = document.createTreeWalker(p, NodeFilter.SHOW_TEXT);
      for (let n = tw.nextNode(); n; n = tw.nextNode()) {
        if (skipped(n.parentElement)) continue;
        nodes.push({ n, at: raw.length });
        raw += n.nodeValue;
      }
      const parts = seg ? [...seg.segment(raw)].map((s) => ({ at: s.index, s: s.segment })) : splitSentences(raw);
      const cands = [];
      parts.forEach((part, i) => {
        const text = stripRefs(part.s);
        const lt = text.toLowerCase();
        const match = hits(text);
        if (text.length < (match ? 20 : 40) || text.length > 420) return;
        if (!match && !/[.!?]["”')\]]*$/.test(text)) return;
        let s = text.length >= 60 && text.length <= 260 ? 1 : 0.35;
        if (i === 0) s += 0.7;
        let th = 0;
        for (const w of titleWords) if (lt.includes(w)) th++;
        s += Math.min(1.5, 0.5 * th);
        if (/ (is|are|was|were) (a|an|the) /.test(lt)) s += 0.35;
        if (/\d/.test(text) && !/^\d/.test(text)) s += 0.3;
        if (/^(it|this|these|they|he|she|that|such)\b/.test(lt)) s -= 0.3;
        if (/(cookie|subscribe|sign in|log in|newsletter|javascript)/.test(lt)) s = -9;
        if (match) s += 3 + match;
        const startAt = part.at + part.s.match(/^\s*(?:(?:\d+|[a-z])\]\s*)?/i)[0].length; // after a stray "13]"
        const endAt = part.at + part.s.trimEnd().length;
        cands.push({ text, s, startAt, endAt, match });
      });
      cands.sort((a, b) => b.s - a.s);
      const keep = raw.length > 900 ? 3 : raw.length > 450 ? 2 : 1;
      cands.forEach((c, i) => {
        if (!(c.match || (i < keep && c.s >= 0.9 && p.tagName === 'P'))) return;
        const range = rangeFor(nodes, c.startAt, c.endAt);
        if (range) sentences.push({ kind: 'sentence', text: c.text, range, el: p, key: c.s >= 2.2, score0: c.s + (pIndex < 3 ? 0.4 : 0) });
      });
      pIndex++;
    }
    // Enough to read, not the whole book.
    sentences.sort((a, b) => b.score0 - a.score0);
    for (const s of sentences.slice(0, terms.length ? 80 : 30)) put(s);

    // Links people would follow: real words, real addresses, in the text.
    let links = 0;
    for (const a of root.querySelectorAll('p a[href], li a[href], dd a[href], h2 a[href], h3 a[href]')) {
      if (links >= (terms.length ? 60 : 30)) break;
      if (used.has(a) || skipped(a) || a.closest('cite, .references, .reflist')) continue;
      const text = (a.textContent || '').trim();
      if (!/^https?:/.test(a.href) || a.getAttribute('href').startsWith('#')) continue;
      if (text.split(/\s+/).length < 2 && text.length < 10 && !hits(text)) continue;
      if (!shown(a)) continue;
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

  // A DOM range for characters [a, b) of a block built from text nodes.
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
    const el = f.node || f.el;
    const r = el?.isConnected && f.kind !== 'sentence' ? el.getBoundingClientRect() : f.range?.getBoundingClientRect();
    if (!r || r.width < 1 || r.height < 1) return null;
    return { x: r.left + scrollX, y: r.top + scrollY, w: r.width, h: r.height };
  }
  function linesOf(f) {
    const rs = f.kind === 'sentence' || !(f.node || f.el)?.isConnected ? f.range?.getClientRects() : (f.node || f.el).getClientRects();
    return [...(rs || [])].filter((r) => r.width > 1).map((r) => ({ x: r.left + scrollX, y: r.top + scrollY, w: r.width, h: r.height }));
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
      this.bob = V(0, 0);
      this.heading = Math.PI / 2;
      this.goal = null;
      this.maxSpeed = 0;
      this.arrive = 1;
      this.airborne = false;
      this.time = 0;
      this.still = 0;
      this.burstT = 0;
      this.bursting = true;
      this.face = null;
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
    forward() { return fromAngle(this.heading); }
    drawPos() { return add(this.pos, this.bob); }
    head() { return add(this.drawPos(), mul(this.forward(), this.bodyLen * 0.32)); }
    rear() { return sub(this.drawPos(), mul(this.forward(), this.bodyLen * 0.5)); }
    hip(l) {
      const f = this.forward();
      return add(add(this.drawPos(), mul(f, l.along)), mul(perp(f), l.side * this.bodyWid * 0.5));
    }
    restPoint(l, curl = 1) { return add(this.pos, mul(fromAngle(this.heading + l.side * l.angle), l.rest * curl)); }
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
      this.bob = mul(this.bob, Math.exp(-14 * dt));
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
        const base = vlerp(l.from, l.to, k);
        l.lift = Math.sin(Math.PI * Math.min(1, l.t));
        l.foot = add(base, mul(norm(sub(base, this.pos)), l.lift * 7 * s));
        if (l.t >= 1) {
          l.foot = l.to;
          l.stepping = false;
          l.lift = 0;
          stepping--;
          // Each planted foot gives the body a small push: the bob.
          this.bob = add(this.bob, mul(norm(sub(l.to, this.pos)), 0.9 * s));
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
        j[1] = vlerp(h, pole, 0.7);
        j[2] = vlerp(pole, target, 0.5);
        j[3] = target;
        l.init = true;
      }
      j[1] = vlerp(j[1], pole, 0.3);
      j[2] = vlerp(j[2], vlerp(pole, target, 0.6), 0.15);
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

  const ADJ = ['cardboard', 'soggy', 'quantum', 'velvet', 'feral', 'haunted', 'tiny', 'rusty', 'cosmic', 'sleepy', 'crunchy', 'polite', 'sneaky', 'lunar',
    'plastic', 'gloomy', 'frantic', 'bashful', 'wobbly', 'neon', 'dusty', 'grumpy', 'paper', 'midnight', 'spicy', 'hollow', 'mossy', 'electric'];
  const NOUN = ['priest', 'harmonica', 'pickle', 'librarian', 'toaster', 'comet', 'accountant', 'teapot', 'wizard', 'noodle', 'archivist', 'pigeon', 'cactus',
    'goblin', 'lantern', 'bishop', 'raccoon', 'waffle', 'oracle', 'gremlin', 'cassette', 'monk', 'crouton', 'kettle', 'walrus', 'clerk', 'sock', 'banana'];
  const name = (() => {
    try {
      const kept = sessionStorage.getItem('spiderpet-name');
      if (kept) return kept;
    } catch {}
    const n = `${pick(ADJ)} ${pick(NOUN)}`;
    try { sessionStorage.setItem('spiderpet-name', n); } catch {}
    return n;
  })();

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
    hunt: () => `hunting: ${short(goal.toLowerCase(), 40)}`,
    tracking: () => pick([`tracking “${short(goal.toLowerCase(), 28)}”…`, 'following the scent…', 'nothing yet. deeper.']),
    huntDone: (n) => (n ? `hunt over: ${n} found.` : `no trace of “${short(goal.toLowerCase(), 28)}” here.`),
    eat(f) {
      const t = short(f.text, 34);
      if (terms.length && matchOf(f) > 0) return pick([`found: ${t}`, `prey: ${t}`, `got one: ${t}`]);
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
      .grab { position: fixed; width: 44px; height: 44px; margin: -22px 0 0 -22px; border-radius: 50%; pointer-events: auto; cursor: grab; }
      .grab:active { cursor: grabbing; }
    </style>
    <canvas></canvas><div class="grab" title="SpiderPet: drag me"></div>`;
  const canvas = shadow.querySelector('canvas');
  const ctx = canvas.getContext('2d');
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
    // The desktop SpiderPet palette, exactly.
    pal = dark
      ? { leg: '#EE7A68', joint: '#8BF5A6', body: '#6173F2', fill: 'rgba(10,12,26,.92)', head: '#E33CD2', tether: '#DA3CEC', name: '#F4F6FF',
          nameStroke: 'rgba(0,0,0,.72)', silk: 'rgba(232,238,255,.17)', silkStrong: 'rgba(232,238,255,.31)', cloud: 'rgba(12,14,28,.93)',
          magenta: '#E64CF2', green: '#93F5AE', blue: '#7D8BFF', salmon: '#F2836B', cyan: '#7FD6FF' }
      : { leg: '#E0604C', joint: '#22C463', body: '#4152E0', fill: 'rgba(16,18,42,.9)', head: '#D02CC0', tether: '#C42AD8', name: '#14151C',
          nameStroke: 'rgba(255,255,255,.85)', silk: 'rgba(20,24,40,.16)', silkStrong: 'rgba(20,24,40,.29)', cloud: 'rgba(255,255,255,.96)',
          magenta: '#B414C8', green: '#0E9A4C', blue: '#3346E0', salmon: '#F2836B', cyan: '#0B7FB8' };
  }

  let dpr = 1;
  function resize() {
    dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(innerWidth * dpr);
    canvas.height = Math.round(innerHeight * dpr);
  }

  function curve(a, c, b, upto = 1) {
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    const steps = Math.max(1, Math.round(14 * clamp(upto, 0, 1)));
    for (let i = 1; i <= steps; i++) {
      const t = i / 14;
      const u = 1 - t;
      ctx.lineTo(a.x * u * u + c.x * 2 * u * t + b.x * t * t, a.y * u * u + c.y * 2 * u * t + b.y * t * t);
    }
    ctx.stroke();
  }

  function dot(p, r, color) {
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
    ctx.fill();
  }

  // Where the thought cloud and the name sit (page coordinates), easing along.
  const label = { name: null, bubble: null, goal: null };
  let thought = '';
  let thoughtAt = -100;
  function think(text) {
    if (!text || text === thought) return;
    thought = text;
    thoughtAt = now();
  }

  function wrapText(text, maxW) {
    const words = text.split(' ');
    const lines = [];
    let cur = '';
    for (const w of words) {
      const next = cur ? cur + ' ' + w : w;
      if (ctx.measureText(next).width > maxW && cur) {
        lines.push(cur);
        cur = w;
      } else cur = next;
    }
    if (cur) lines.push(cur);
    return lines;
  }

  // A comic thought cloud: a rounded box with bumps on every side. Stroking
  // every part thick, then filling every part, leaves only the outer outline.
  function cloudParts(w, h, k) {
    const parts = [];
    const r = Math.min(h * 0.5, 14 * k);
    parts.push({ rr: [0, 0, w, h, r] });
    const rb = clamp(h * 0.34, 7 * k, 13 * k);
    const n = Math.max(2, Math.floor((w - rb) / (rb * 1.5)));
    for (let i = 0; i < n; i++) {
      const x = rb * 0.9 + ((w - rb * 1.8) * i) / (n - 1);
      parts.push({ c: [x, rb * 0.3, rb * (i % 2 ? 0.9 : 1)] });
      parts.push({ c: [Math.min(w - rb * 0.6, x + rb * 0.4), h - rb * 0.3, rb * (i % 2 ? 1 : 0.85)] });
    }
    parts.push({ c: [rb * 0.25, h * 0.5, h * 0.42] });
    parts.push({ c: [w - rb * 0.25, h * 0.5, h * 0.42] });
    return parts;
  }
  function tracePart(p) {
    ctx.beginPath();
    if (p.rr) ctx.roundRect(...p.rr);
    else ctx.arc(p.c[0], p.c[1], p.c[2], 0, Math.PI * 2);
  }

  function drawLabels(t, dt) {
    const s = spider;
    const k = s.scale;
    const vx = scrollX, vy = scrollY, vw = innerWidth, vh = innerHeight;
    const head = s.head();
    const age = t - thoughtAt;

    // The name floats over the spider when it has nothing to say.
    ctx.font = `bold ${15 * k}px "Segoe UI", system-ui, sans-serif`;
    const nw = ctx.measureText(name).width;
    const body = s.drawPos();
    let want = V(clamp(body.x, vx + nw / 2 + 6, vx + vw - nw / 2 - 6), clamp(body.y - s.reach * 0.95, vy + 20 * k, vy + vh - 4));
    if (!label.name || dist(label.name, want) > 600) label.name = want;
    label.name = vlerp(label.name, want, damp(5, dt));
    if (!thought) {
      ctx.textAlign = 'center';
      ctx.lineJoin = 'round';
      ctx.lineWidth = 3.2;
      ctx.strokeStyle = pal.nameStroke;
      ctx.strokeText(name, label.name.x, label.name.y);
      ctx.fillStyle = pal.name;
      ctx.fillText(name, label.name.x, label.name.y);
      ctx.textAlign = 'left';
      return;
    }

    // The thought cloud: sized for the whole thought, typed out quickly.
    const padx = 13 * k, pady = 9 * k, edge = 12 * k;
    ctx.font = `${15.5 * k}px "Segoe UI", system-ui, sans-serif`;
    const lines = wrapText(thought, 260 * k);
    const lh = 15.5 * k * 1.3;
    let tw = 0;
    for (const l of lines) tw = Math.max(tw, ctx.measureText(l).width);
    ctx.font = `bold ${12 * k}px "Segoe UI", system-ui, sans-serif`;
    tw = Math.max(tw, ctx.measureText(name).width);
    const nh = 12 * k * 1.3;
    const w = tw + padx * 2;
    const h = nh + k + lines.length * lh + pady * 2;
    const fit = (q) => V(clamp(q.x, vx + w / 2 + edge, vx + vw - w / 2 - edge), clamp(q.y, vy + h + edge, vy + vh - edge));
    // Well clear of the legs and never over the words the spider is working
    // on: up-right first, then up-left, down-right, down-left. It holds still
    // while the spider works nearby and only drifts over when it moved far.
    const tb = tether ? boxOf(tether.f) : null;
    const covers = (q) => tb && q.x - w / 2 < tb.x + tb.w && q.x + w / 2 > tb.x && q.y - h < tb.y + tb.h && q.y > tb.y;
    const dx = s.reach * 1.1, up = head.y - s.reach * 1.35, down = head.y + s.reach * 1.35 + h;
    const cands = [V(head.x + dx, up), V(head.x - dx, up), V(head.x + dx, down), V(head.x - dx, down)].map(fit);
    const spot = cands.find((q) => !covers(q)) || cands[0];
    // After a jump down the page it reappears next to the spider, not trailing in from off screen.
    const lost = !label.bubble || label.bubble.y < vy + h * 0.5 || label.bubble.y - h > vy + vh - h * 0.5;
    if (lost || dist(spot, label.bubble) > 1600) label.bubble = label.goal = spot;
    if (dist(spot, label.goal) > s.reach * 1.6 || covers(label.goal)) label.goal = spot;
    label.bubble = vlerp(label.bubble, fit(label.goal), damp(2.2, dt));
    const tl = V(label.bubble.x - w / 2, label.bubble.y - h);

    // Thought trail: three shrinking bubbles leading off toward the spider.
    const below = label.bubble.y >= head.y;
    const from = V(clamp(head.x, tl.x + 18 * k, tl.x + w - 18 * k), below ? tl.y - 5 * k : tl.y + h + 5 * k);
    const to = add(head, V(0, below ? 10 * k : -10 * k));
    const dir = norm(sub(to, from));
    const reach = Math.min(dist(from, to), 70 * k);
    [[4.8, 0.15], [3.4, 0.5], [2.2, 0.85]].forEach(([r, at]) => {
      const c = add(from, mul(dir, reach * at));
      dot(c, (r + 1.4) * k, pal.body);
      dot(c, r * k, pal.cloud);
    });

    // A small pop when a new thought arrives.
    const pop = 0.82 + 0.18 * easeOutBack(Math.min(1, age / 0.22));
    ctx.save();
    ctx.translate(tl.x + w / 2, tl.y + h);
    ctx.scale(pop, pop);
    ctx.translate(-w / 2, -h);
    const parts = cloudParts(w, h, k);
    ctx.strokeStyle = pal.body;
    ctx.lineWidth = 3.2 * k;
    for (const p of parts) {
      tracePart(p);
      ctx.stroke();
    }
    ctx.fillStyle = pal.cloud;
    for (const p of parts) {
      tracePart(p);
      ctx.fill();
    }
    ctx.textBaseline = 'top';
    ctx.font = `bold ${12 * k}px "Segoe UI", system-ui, sans-serif`;
    ctx.fillStyle = dark ? pal.joint : pal.body;
    ctx.fillText(name, padx, pady);
    ctx.font = `${15.5 * k}px "Segoe UI", system-ui, sans-serif`;
    ctx.fillStyle = pal.name;
    let left = Math.floor(age * 55) + 1;
    lines.forEach((l, i) => {
      if (left <= 0) return;
      ctx.fillText(l.slice(0, left), padx, pady + nh + k + i * lh);
      left -= l.length + 1;
    });
    ctx.textBaseline = 'alphabetic';
    ctx.restore();
  }

  function draw(t, dt) {
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, innerWidth, innerHeight);
    ctx.translate(-scrollX, -scrollY);
    const s = spider;
    const k = s.scale;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';

    // Silk between finds, and the dragline back to the last one.
    ctx.lineWidth = 0.9;
    ctx.strokeStyle = pal.silk;
    for (const w of silk) {
      const a = anchorOf(w.a), b = anchorOf(w.b);
      if (!a || !b) continue;
      if ((a.y < scrollY - 400 && b.y < scrollY - 400) || (a.y > scrollY + innerHeight + 400 && b.y > scrollY + innerHeight + 400)) continue;
      const d = dist(a, b);
      curve(a, add(vlerp(a, b, 0.5), V(0, d * 0.08)), b, (t - w.at) / 0.35);
    }
    const last = lastEaten && anchorOf(lastEaten);
    if (last && mode !== 'held' && mode !== 'drop') {
      const rear = s.rear();
      const d = dist(rear, last);
      ctx.strokeStyle = pal.silkStrong;
      curve(rear, add(vlerp(rear, last, 0.5), V(0, d * 0.1)), last);
    }

    // The thread it rappels on, or hangs from your hand by.
    if (mode === 'drop' || mode === 'held') {
      ctx.strokeStyle = pal.name;
      ctx.globalAlpha = 0.6;
      ctx.lineWidth = 1.1;
      ctx.beginPath();
      ctx.moveTo(s.rear().x, mode === 'drop' ? scrollY - 4 : s.rear().y - 300 * k);
      ctx.lineTo(s.rear().x, s.rear().y);
      ctx.stroke();
      ctx.globalAlpha = 1;
    }

    // Sentences read so far, when the page cannot highlight them itself.
    if (!HIGHLIGHTS && readingRange) {
      ctx.fillStyle = dark ? 'rgba(230,76,242,.18)' : 'rgba(180,20,200,.14)';
      for (const r of readingRange.getClientRects()) ctx.fillRect(r.left + scrollX - 1, r.top + scrollY - 1, r.width + 2, r.height + 2);
    }

    // Lock-on: the box pulses around the target and the line is out from the
    // head. A grab shoots it; a read runs its tip along the sentence.
    if (tether) {
      const head = s.head();
      if (tether.mode !== 'read') {
        const pulse = 0.75 + 0.25 * Math.sin(t * 9);
        ctx.strokeStyle = pal.tether;
        ctx.globalAlpha = pulse;
        ctx.lineWidth = 1.6;
        for (const r of linesOf(tether.f).slice(0, 4)) ctx.strokeRect(r.x - 2.5, r.y - 2.5, r.w + 5, r.h + 5);
        ctx.globalAlpha = 1;
      }
      let end = tether.tip;
      if (!end) {
        const b = boxOf(tether.f);
        if (b) end = V(clamp(head.x, b.x - 2.5, b.x + b.w + 2.5), clamp(head.y, b.y - 2.5, b.y + b.h + 2.5));
      }
      if (end) {
        const tip = vlerp(head, end, tether.amount);
        ctx.strokeStyle = pal.tether;
        ctx.lineWidth = Math.max(1.2, 1.7 * k);
        ctx.beginPath();
        ctx.moveTo(head.x, head.y);
        ctx.lineTo(tip.x, tip.y);
        ctx.stroke();
        dot(tip, Math.max(1.6, 2.4 * k), pal.tether);
      }
    }

    // Legs, joints, body, head: the node-and-line look of the reference clip.
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
    ctx.beginPath();
    for (const l of s.legs)
      for (let i = 1; i < 4; i++) {
        const rr = i === 3 ? r * (1 + 0.35 * l.lift) : r;
        ctx.moveTo(l.j[i].x + rr, l.j[i].y);
        ctx.arc(l.j[i].x, l.j[i].y, rr, 0, Math.PI * 2);
      }
    ctx.fill();
    const c = s.drawPos();
    ctx.save();
    ctx.translate(c.x, c.y);
    ctx.rotate(s.heading);
    ctx.fillStyle = pal.fill;
    ctx.strokeStyle = pal.body;
    ctx.lineWidth = Math.max(1.2, 1.7 * k);
    ctx.fillRect(-s.bodyLen / 2, -s.bodyWid / 2, s.bodyLen, s.bodyWid);
    ctx.strokeRect(-s.bodyLen / 2, -s.bodyWid / 2, s.bodyLen, s.bodyWid);
    ctx.restore();
    dot(s.head(), Math.max(2.2, 3.7 * k), pal.head);

    drawLabels(t, dt);

    // Arrival glitch: torn color bars down the left edge for a moment.
    const intro = t - summonedAt;
    if (intro >= 0 && intro < 0.7) {
      const a = 1 - intro / 0.7;
      const bars = [pal.magenta, pal.green, pal.blue, pal.salmon, pal.cyan];
      ctx.globalAlpha = 0.5 * a;
      for (let i = 0; i < 18; i++) {
        ctx.fillStyle = pick(bars);
        ctx.fillRect(scrollX + rand(0, 30), scrollY + rand(0, innerHeight), rand(10, 160), rand(2, 6));
      }
      ctx.globalAlpha = 1;
    }

    grab.style.left = s.pos.x - scrollX + 'px';
    grab.style.top = s.pos.y - scrollY + 'px';
  }

  // ------------------------------------------------------------------ the page's look

  const HIGHLIGHTS = typeof CSS !== 'undefined' && CSS.highlights && typeof Highlight === 'function';
  const hl = {};
  if (HIGHLIGHTS)
    for (const n of ['eaten', 'eaten-key', 'reading', 'reading-key', 'goal']) {
      hl[n] = new Highlight();
      CSS.highlights.set('spiderpet-' + n, hl[n]);
    }
  let readingRange = null;

  // Every place the search words appear, lit up at once.
  function lightGoal() {
    if (!HIGHLIGHTS) return;
    hl.goal.clear();
    if (!terms.length) return;
    const root = mainRoot();
    const tw = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    let count = 0;
    for (let n = tw.nextNode(); n && count < 600; n = tw.nextNode()) {
      const t = n.nodeValue;
      if (t.length < 2 || skipped(n.parentElement)) continue;
      const lt = t.toLowerCase();
      for (const term of terms) {
        let at = lt.indexOf(term);
        while (at >= 0 && count < 600) {
          const r = document.createRange();
          r.setStart(n, at);
          r.setEnd(n, at + term.length);
          hl.goal.add(r);
          count++;
          at = lt.indexOf(term, at + term.length);
        }
      }
    }
  }

  function glitch(node) {
    if (!node) return;
    node.classList.remove('spiderpet-glitch');
    void node.offsetWidth;
    node.classList.add('spiderpet-glitch');
    setTimeout(() => node.classList.remove('spiderpet-glitch'), 520);
  }

  // Restyle the real text, the way the clip does: no overlay, the page itself changes.
  function restyle(f) {
    for (const x of [f, ...(f.more || [])]) {
      if (x.styled) {
        glitch(x.node);
        continue;
      }
      x.styled = true;
      if (x.kind === 'sentence') {
        if (HIGHLIGHTS) hl[x.key ? 'eaten-key' : 'eaten'].add(x.range);
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
      node.classList.add('spiderpet-on', 'spiderpet-' + x.kind, 'spiderpet-v' + (parseInt(x.id.slice(1), 36) % 3));
      x.node = node;
      glitch(node);
    }
    ownChanges();
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
    ownChanges();
  }

  // ------------------------------------------------------------------ behaviour

  let finds = [];
  const byId = new Map();
  let app = { connected: false, settings: {} };
  let spider = null;
  let mode = 'drop'; // drop, walk, lasso, eat, rest, held
  let target = null;
  let tether = null;
  let modeAt = 0;
  let userScrolledAt = -100;
  let ourScrollAt = -100;
  let lastScanAt = 0;
  let chooseAt = 0;
  let domDirty = true;
  let running = false;
  let raf = 0;
  let lastT = now();
  let revealId = null;
  let lastUrl = pageUrl();
  let restingSaid = false;
  let summonedAt = -100;
  let silk = []; // {a, b, at}: threads between finds eaten one after another
  let lastEaten = null;
  let found = 0; // matches eaten in this hunt

  // The page changed (lazy loading, infinite feeds): read it again later.
  // Reading a big page can take half a second, so the slower the last read,
  // the longer the wait before the next one.
  let scanGap = 3;
  let scanMs = 0;
  const observer = new MutationObserver(() => {
    domDirty = true;
  });
  // The spider's own changes (restyled words, badges) are not news.
  const ownChanges = () => observer.takeRecords();

  // Where a silk thread ends. Measuring forces the browser to lay the page
  // out, so each spot is measured twice a second, not every frame.
  const anchors = new Map(); // id -> { p, at }
  function anchorOf(id) {
    const t = now();
    const c = anchors.get(id);
    if (c && t - c.at < 0.5) return c.p;
    const f = byId.get(id);
    const ls = f ? linesOf(f) : [];
    const p = ls.length ? V(ls[0].x, ls[0].y + ls[0].h / 2) : null;
    anchors.set(id, { p, at: t + Math.random() * 0.2 }); // spread the re-measuring over frames
    return p;
  }

  function setMode(m) {
    mode = m;
    modeAt = now();
  }

  function rescan(sendAll) {
    const t0 = now();
    domDirty = false;
    skipMemo = new WeakMap();
    const got = scan();
    lastScanAt = now();
    scanMs = (lastScanAt - t0) * 1000;
    scanGap = clamp((lastScanAt - t0) * 40, 3, 30);
    const fresh = [];
    for (const f of got) {
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
        finds: list.map(({ id, kind, text, label, href, context, page, key, order }) => ({ id, kind, text, label: label || '', href: href || '', context: context || '', page: !!page, key: !!key, order, match: hits(text) })),
      }).catch(() => {});
    if (terms.length) lightGoal();
    return fresh.length;
  }

  function sendPage() {
    const root = mainRoot();
    const text = squash((root.innerText || '').replace(/\[\d+\]/g, '')).slice(0, 8000);
    api.runtime.sendMessage({ type: 'page', url: pageUrl(), title: document.title, lang: document.documentElement.lang || '', text }).catch(() => {});
  }

  // What next: the most valuable find near the spider, in view, in reading
  // order. On a hunt, only what matches the search (or what the AI rated high).
  function chooseTarget() {
    const vy = scrollY, vh = innerHeight, vx = scrollX, vw = innerWidth;
    let best = null;
    let bestScore = -1e9;
    for (const f of finds) {
      if (f.eaten || f.page) continue;
      const m = terms.length ? matchOf(f) : 0;
      if (terms.length && m === 0 && !(f.score >= 7)) continue;
      const b = boxOf(f);
      if (!b) continue;
      if (b.y + b.h < vy + 4 || b.y > vy + vh - 20 || b.x > vx + vw || b.x + b.w < vx) continue;
      let s = KIND_PRIO[f.kind] ?? 1;
      if (f.key) s += 0.8;
      if (f.score >= 0) s += f.score * 0.35 - 1.2; // the AI knows what you are after
      s += m * 2.5;
      s -= dist(spider.pos, V(b.x, b.y + b.h / 2)) / (900 * spider.scale);
      s -= (b.y - vy) / 2400;
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

  // The next match the spider has not eaten: below the view first, in reading order.
  function nextMatch() {
    const below = scrollY + innerHeight - 20;
    let down = null, up = null;
    for (const f of finds) {
      if (f.eaten || f.page || !(matchOf(f) > 0 || f.score >= 7)) continue;
      const b = boxOf(f);
      if (!b) continue;
      if (b.y > below) {
        if (!down || b.y < down.b.y) down = { f, b };
      } else if (b.y + b.h < scrollY + 4) {
        if (!up || b.y < up.b.y) up = { f, b };
      }
    }
    return down || up;
  }

  function atBottom() {
    return scrollY + innerHeight >= document.documentElement.scrollHeight - 8;
  }

  function crawlOn(t, k) {
    ourScrollAt = t;
    window.scrollBy({ top: innerHeight * (terms.length ? 0.8 : 0.7), behavior: 'smooth' });
    spider.setGoal(V(spider.pos.x, scrollY + innerHeight * 0.75), 260 * k, 80 * k);
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
        // It lands a third of the way down the view, wherever the view is now
        // (the page may still be scrolling).
        spider.airborne = true;
        const landY = scrollY + clamp(innerHeight * 0.3, 160, 360);
        spider.pos = V(spider.pos.x, Math.min(landY, Math.max(spider.pos.y, scrollY - 40) + 900 * k * dt));
        if (spider.pos.y >= landY - 1) {
          spider.airborne = false;
          spider.place(spider.pos, Math.PI / 2);
          think(!app.connected ? say.offline() : terms.length ? say.hunt() : say.land());
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
          target = null;
          if (t - chooseAt < 0.25) break; // looking costs layout: four times a second is plenty
          chooseAt = t;
          target = chooseTarget();
          if (!target) {
            // Out of food in view: look again if the page changed (lazy pages), then crawl down.
            if (domDirty && t - lastScanAt > scanGap && rescan(false)) break;
            // On a hunt it jumps straight to the next match and drops in on a thread.
            if (terms.length && t - ourScrollAt > 0.9) {
              const next = nextMatch();
              if (next) {
                ourScrollAt = t;
                think(say.tracking());
                window.scrollTo({ top: Math.max(0, next.b.y - innerHeight * 0.35), behavior: 'smooth' });
                break;
              }
            }
            const crawl = app.settings?.crawl !== false || terms.length;
            if (crawl && !atBottom() && t - userScrolledAt > (terms.length ? 2 : 4) && t - ourScrollAt > (terms.length ? 0.9 : 1.2)) {
              think(terms.length ? say.tracking() : say.crawl());
              crawlOn(t, k);
            } else if (t - ourScrollAt > 1.5 && !restingSaid) {
              restingSaid = true;
              think(terms.length && atBottom() ? say.huntDone(found) : atBottom() ? say.drained() : say.more());
              setMode('rest');
            }
            break;
          }
          restingSaid = false;
          tether = { f: target, amount: 1, mode: 'travel' }; // locked on while it walks over, like the clip
          if (!terms.length && Math.random() < 0.25) think(say.sense());
        }
        const goalPt = approachPoint(target);
        if (!goalPt) {
          target.eaten = true; // it vanished from the page
          target = null;
          tether = null;
          break;
        }
        spider.setGoal(goalPt, (terms.length ? 680 : 560) * k, 70 * k);
        const b = boxOf(target);
        const near = V(clamp(spider.pos.x, b.x, b.x + b.w), clamp(spider.pos.y, b.y, b.y + b.h));
        if (dist(spider.pos, near) < spider.reach * 1.05 || (dist(spider.pos, goalPt) < 12 * k && spider.speed() < 40 * k)) {
          spider.face = Math.atan2(near.y - spider.pos.y, near.x - spider.pos.x);
          tether = { f: target, amount: 0, mode: 'grab' };
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
          if (terms.length && matchOf(target) > 0) found++;
          think(say.eat(target));
          api.runtime.sendMessage({ type: 'eaten', id: target.id }).catch(() => {});
          badge(target);
          if (lastEaten && lastEaten !== target.id) silk.push({ a: lastEaten, b: target.id, at: t });
          if (silk.length > 160) silk.shift();
          lastEaten = target.id;
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
          if (HIGHLIGHTS) {
            hl.reading.clear();
            hl['reading-key'].clear();
          }
          tether = null;
          spider.face = null;
          target = null;
          setMode('walk');
        }
        break;
      }
      case 'rest': {
        spider.setGoal(spider.pos, 0, 1);
        if (t - chooseAt < 0.5) break;
        chooseAt = t;
        // New things in view (you scrolled, the page grew): back to work.
        if (t - modeAt > 1.5 && (chooseTarget() || (domDirty && t - lastScanAt > scanGap && rescan(false)))) {
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

  // The sentence sweep: highlight and underline grow as the spider reads,
  // and the lasso's tip runs along with it.
  function startReading(f) {
    readingRange = f.range.cloneRange();
    readingRange.collapse(true);
    tether = { f, amount: 1, mode: 'read', tip: null };
  }
  function readProgress(f, p) {
    p = clamp(p, 0, 1);
    const full = f.range;
    const r = document.createRange();
    try {
      r.setStart(full.startContainer, full.startOffset);
      const total = full.toString().length;
      let want = Math.round(total * p);
      const rootNode = full.commonAncestorContainer.nodeType === 1 ? full.commonAncestorContainer : full.commonAncestorContainer.parentNode;
      const tw = document.createTreeWalker(rootNode, NodeFilter.SHOW_TEXT);
      let n = (tw.currentNode = full.startContainer);
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
    const rects = r.getClientRects();
    const lr = rects[rects.length - 1];
    if (lr && tether) tether.tip = V(lr.right + scrollX, lr.bottom + scrollY + 1);
    if (HIGHLIGHTS) {
      const h = hl[f.key ? 'reading-key' : 'reading'];
      h.clear();
      h.add(r);
    }
  }

  function frame() {
    if (!running) return;
    raf = requestAnimationFrame(frame);
    const t = now();
    const resting = mode === 'rest' && spider.speed() < 1 && t - thoughtAt > 2;
    if (t - lastT < (resting ? 1 / 31 : 1 / 61)) return;
    const dt = Math.min(0.05, t - lastT);
    lastT = t;
    try {
      step(t, dt);
      draw(t, dt);
    } catch (e) {
      console.warn('SpiderPet:', e);
    }
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

  function setGoal(g, list) {
    const changed = g !== goal;
    goal = g || '';
    terms = goal ? [...new Set((list || []).map((x) => String(x).toLowerCase().trim()).filter((x) => x.length >= 2))] : [];
    if (changed) {
      found = 0;
      restingSaid = false;
      if (mode === 'walk' || mode === 'rest') {
        target = null;
        tether = null;
        setMode('walk');
      }
      think(goal ? say.hunt() : say.sense());
    }
    rescan(false); // sentences that match the search join the hunt
    lightGoal();
  }

  function onMessage(m) {
    switch (m.type) {
      case 'app':
        app = m.app || app;
        if (app.goal !== undefined && (app.goal !== goal || (app.terms || []).length !== terms.length)) setGoal(app.goal, app.terms);
        break;
      case 'goal':
        setGoal(m.goal, m.terms);
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
        if (m.gist?.summary && !terms.length) think('habitat: ' + short(m.gist.summary.toLowerCase().replace(/\.$/, ''), 44));
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
    anchors.clear();
    target = null;
    tether = null;
    readingRange = null;
    silk = [];
    lastEaten = null;
    if (HIGHLIGHTS) for (const h of Object.values(hl)) h.clear();
    hello();
  }

  function hello() {
    readTheme();
    api.runtime.sendMessage({ type: 'spider-hello', url: pageUrl(), title: document.title }).then((r) => {
      if (r?.app) {
        app = r.app;
        goal = app.goal || '';
        terms = goal ? (app.terms || []).map((x) => String(x).toLowerCase()) : [];
      }
      if (r?.reveal) revealId = r.reveal;
      sendPage();
      rescan(true);
      if (terms.length) think(say.hunt());
    }).catch(() => {
      rescan(false);
    });
  }

  // The old desktop spider's size: the body is about two lines of text long.
  function scaleForPage() {
    const root = mainRoot();
    const p = root.querySelector('p') || root;
    const cs = getComputedStyle(p);
    let lh = parseFloat(cs.lineHeight);
    if (!lh) lh = (parseFloat(cs.fontSize) || 16) * 1.4;
    return clamp(lh / 22, 0.7, 1.7);
  }

  function start() {
    if (running) return;
    running = true;
    document.documentElement.appendChild(host);
    resize();
    readTheme();
    spider = spider || new Spider(scaleForPage());
    const x = scrollX + clamp(innerWidth * 0.22, 120, 420);
    spider.airborne = true;
    spider.place(V(x, scrollY - 40), Math.PI / 2);
    spider.goal = V(x, scrollY + clamp(innerHeight * 0.3, 160, 360));
    summonedAt = now();
    setMode('drop');
    addEventListener('resize', resize);
    addEventListener('wheel', markUser, { passive: true });
    addEventListener('touchmove', markUser, { passive: true });
    addEventListener('keydown', onKey);
    api.runtime.onMessage.addListener(onMessage);
    observer.observe(document.body, { childList: true, subtree: true });
    hello();
    lastT = now();
    raf = requestAnimationFrame(frame);
  }

  function stop() {
    if (!running) return;
    running = false;
    cancelAnimationFrame(raf);
    host.remove();
    observer.disconnect();
    removeEventListener('resize', resize);
    removeEventListener('wheel', markUser);
    removeEventListener('touchmove', markUser);
    removeEventListener('keydown', onKey);
    api.runtime.onMessage.removeListener(onMessage);
    if (HIGHLIGHTS) {
      hl.reading.clear();
      hl['reading-key'].clear();
      hl.goal.clear();
    }
    api.runtime.sendMessage({ type: 'stopped' }).catch(() => {});
    // Restyled text and checks stay, so you can still read them.
  }

  window.__spiderpet = {
    start() {
      if (!running) start();
    },
    running: () => running,
    // For tests and bug reports: how hard the spider is working.
    stats: () => ({ mode, finds: finds.length, eaten: finds.filter((f) => f.eaten).length, scanMs: Math.round(scanMs), scanGap, goal, terms }),
  };
  start();
})();
