let enableAccounts = false;
let newsCarousel = window.__newsCarouselAtStart === true;
let showAccountNotification = false;
let pages = [];
const pageControls = document.querySelector('.page-controls');
let currentPageIndex = 0;

const CAROUSEL_SPACING = 118;
const CAROUSEL_DELAY = 4000;
const carousel = { cards: [], index: 0, timer: 0, paused: false, lastWheel: 0, loaded: false };
const carouselRoot = document.getElementById('news-carousel');

function carouselOffset(i) {
  const n = carousel.cards.length;
  let d = ((i - carousel.index) % n + n) % n;
  if (d > n / 2) d -= n;
  return d;
}

function layoutCarousel() {
  carousel.cards.forEach((card, i) => {
    const rel = Math.max(-2, Math.min(2, carouselOffset(i)));
    const depth = Math.abs(rel);
    card.style.transform = `translate(-50%, -50%) translateY(${rel * CAROUSEL_SPACING}px) scale(${Math.pow(0.8, depth)})`;
    card.style.opacity = depth <= 1 ? '1' : '0';
    card.style.zIndex = String(10 - depth);
    card.style.pointerEvents = depth <= 1 ? 'auto' : 'none';
    card.classList.toggle('front', rel === 0);
  });
}

function carouselActive() {
  return newsCarousel && (!carousel.loaded || carousel.cards.length > 0) && pages[currentPageIndex]?.id === 'news';
}

function restartCarouselTimer() {
  clearInterval(carousel.timer);
  carousel.timer = 0;
  if (carouselActive() && !carousel.paused && carousel.cards.length > 1)
    carousel.timer = setInterval(() => stepCarousel(1), CAROUSEL_DELAY);
}

function stepCarousel(delta) {
  const n = carousel.cards.length;
  if (n < 2) return;
  carousel.index = ((carousel.index + delta) % n + n) % n;
  layoutCarousel();
  restartCarouselTimer();
}

function updateCarouselMode() {
  document.querySelector('section').classList.toggle('carousel', carouselActive());
  restartCarouselTimer();
}

function buildCarousel(slides) {
  carouselRoot.replaceChildren();
  carouselRoot.classList.remove('ready');
  carousel.index = 0;
  carousel.loaded = true;
  carousel.cards = slides.map((slide, i) => {
    const card = document.createElement('a');
    card.className = 'carousel-card';
    card.href = slide.href;
    card.target = '_blank';
    const img = document.createElement('img');
    img.src = slide.src;
    card.appendChild(img);
    const title = document.createElement('span');
    title.className = 'carousel-title';
    title.textContent = slide.title;
    card.appendChild(title);
    card.addEventListener('click', (e) => {
      const rel = carouselOffset(i);
      if (rel === 0) return;
      e.preventDefault();
      stepCarousel(rel);
    });
    carouselRoot.appendChild(card);
    return card;
  });
  layoutCarousel();
  updateCarouselMode();

  const visible = carousel.cards.slice(0, 2).concat(carousel.cards.slice(-1))
    .map((card) => card.querySelector('img'))
    .map((img) => img.complete ? Promise.resolve() : new Promise((resolve) => {
      img.addEventListener('load', resolve, { once: true });
      img.addEventListener('error', resolve, { once: true });
    }));
  const timeout = new Promise((resolve) => setTimeout(resolve, 3000));
  Promise.race([Promise.all(visible), timeout]).then(() => carouselRoot.classList.add('ready'));
}

carouselRoot.addEventListener('mouseenter', () => { carousel.paused = true; restartCarouselTimer(); });
carouselRoot.addEventListener('mouseleave', () => { carousel.paused = false; restartCarouselTimer(); });
carouselRoot.addEventListener('wheel', (e) => {
  e.preventDefault();
  const now = Date.now();
  if (e.deltaY === 0 || now - carousel.lastWheel < 350) return;
  carousel.lastWheel = now;
  stepCarousel(e.deltaY > 0 ? 1 : -1);
}, { passive: false });

function rebuildPages() {
  const currentTitle = pages[currentPageIndex]?.dataset.title;
  const allPages = Array.from(document.querySelectorAll('.page'));
  pages = allPages.filter((page) => {
    if (page.id === 'accounts') return enableAccounts;
    if (page.id === 'notification') return showAccountNotification;
    return true;
  });
  if (currentTitle) {
    const idx = pages.findIndex((page) => page.dataset.title === currentTitle);
    if (idx !== -1) {
      currentPageIndex = idx;
    } else {
      const newsIdx = pages.findIndex((page) => page.id === 'news');
      currentPageIndex = newsIdx === -1 ? 0 : newsIdx;
    }
  }
  __showPageAtIndex(currentPageIndex);
}

function __showPageAtIndex(index) {
  if (pages.length === 0) return;
  currentPageIndex = ((index % pages.length) + pages.length) % pages.length;
  pages.forEach((page, i) => {
    const selected = i === currentPageIndex;
    page.setAttribute('aria-selected', selected ? 'true' : 'false');
    page.hidden = !selected;
  });
  document.getElementById('page-title').textContent = pages[currentPageIndex].dataset.title;
  updateCarouselMode();
}

function __renderPage(title) {
  const index = pages.findIndex((page) => page.dataset.title === title);
  if (index === -1) return;
  __showPageAtIndex(index);
}

function __stepPage(delta) {
  __showPageAtIndex(currentPageIndex + delta);
}

pageControls.firstElementChild.addEventListener('click', () => __stepPage(-1));
pageControls.lastElementChild.addEventListener('click', () => __stepPage(1));

rebuildPages();

function __renderNews(items) {
  const origin = 'https://www.play.wizard101.com';
  const abs = (url) => {
    if (!url) return origin + '/news';
    if (url.startsWith('/')) return origin + url;
    return url;
  };

  function newsTypeSrc(title) {
    const rules = [
      [/\bpets?\b/i, 'pet'],
      [/\bmembership\b/i, 'member'],
      [/\bcrowns?\b/i, 'crowns'],
      [/\bmounts?\b/i, 'mount'],
      [/\b(?:items|bundles?)\b/i, 'bundle']
    ];
    for (const [pattern, name] of rules) {
      if (!pattern.test(title)) continue;
      if (name === 'crowns' && /\bevents?\b/i.test(title)) return 'assets/news-types/other.png';
      return 'assets/news-types/' + name + '.png';
    }
    return 'assets/news-types/other.png';
  }

  const cutoff = new Date();
  cutoff.setMonth(cutoff.getMonth() - 1);
  cutoff.setHours(0, 0, 0, 0);

  const news = document.getElementById('news');
  news.replaceChildren();
  news.classList.remove('ready');
  const slides = [];

  for (const item of items) {
    const tagNames = new Set((item.tags || []).map((tag) => String(tag).trim().toUpperCase()).filter(Boolean));
    if (tagNames.has('EU') && !tagNames.has('NA')) continue;
    if (tagNames.size === 1 && tagNames.has('STEAM')) continue;

    const match = /^(\d{4})-(\d{2})-(\d{2})/.exec(item.date || '');
    if (!match) continue;
    const posted = new Date(Number(match[1]), Number(match[2]) - 1, Number(match[3]));
    if (posted < cutoff) continue;

    const href = abs(item.href);
    const article = document.createElement('article');
    if (item.date) article.dataset.date = item.date;
    if (item.img_src) {
      article.dataset.image = item.img_src;
      slides.push({ src: abs(item.img_src), href, title: item.title || '' });
    }

    const figure = document.createElement('figure');
    const img_link = document.createElement('a');
    img_link.href = href;
    img_link.target = '_blank';
    const img = document.createElement('img');
    img.src = newsTypeSrc(item.title);
    img_link.appendChild(img);
    figure.appendChild(img_link);

    const p = document.createElement('p');
    const title_link = document.createElement('a');
    title_link.href = href;
    title_link.target = '_blank';
    const title = document.createElement('b');
    title.textContent = item.title;
    title_link.appendChild(title);
    p.appendChild(title_link);
    p.appendChild(document.createTextNode(' ' + (item.text)));

    article.appendChild(figure);
    article.appendChild(p);
    news.appendChild(article);
  }

  void news.offsetWidth;
  news.classList.add('ready');

  buildCarousel(slides);
}

// ── C++ → JS (called via ExecuteScript) ──────────────────────────────────

function __showPatchProgress() {
  document.getElementById('login-fields').style.display = 'none';
  document.getElementById('play-fields').style.display  = 'flex';
}

function __setFileProgress(name, percent) {
  const short = name.split('/').pop();
  document.getElementById('file-name').textContent = short;
  document.getElementById('file-progress').value   = percent;
}

function __setOverallProgress(percent) {
  document.getElementById('total-progress').value = percent;
  if (percent >= 100) {
    document.getElementById('file-progress').value = 100;
    document.getElementById('file-name').textContent = 'Download Complete!';
    const btn = document.getElementById('play');
    btn.disabled = false;
    btn.removeAttribute('disabled');
  }
}

function __showLoginError() {
  const btn = document.getElementById('login');
  btn.disabled    = false;
  btn.textContent = 'Login';
}

function __showAccountNotification(reason) {
  document.getElementById('notice-locked').hidden = reason !== 'AuthQuarantined';
  document.getElementById('notice-banned').hidden = reason !== 'AuthBanned';
  showAccountNotification = true;
  rebuildPages();
  __renderPage('Account Notification');
}

function __resetToLogin() {
  document.getElementById('play-fields').style.display  = 'none';
  document.getElementById('login-fields').style.display = 'flex';
  document.getElementById('file-progress').value  = 0;
  document.getElementById('total-progress').value = 0;
  document.getElementById('file-name').textContent = '';
  const play = document.getElementById('play');
  play.disabled = true;
  play.setAttribute('disabled', '');
  const login = document.getElementById('login');
  login.disabled    = false;
  login.textContent = 'Login';
  document.getElementById('password').value = '';
}

// ── JS → C++ (postMessage bridge) ────────────────────────────────────────

document.getElementById('login').addEventListener('click', () => {
  const username = document.getElementById('username').value.trim();
  const password = document.getElementById('password').value;
  if (!username || !password) return;

  const remember = document.getElementById('remember-username').checked;
  window.chrome.webview.postMessage(
    JSON.stringify({ action: 'login', username, password, remember })
  );
});

function __setRememberedUsername(name) {
  if (!name) return;
  document.getElementById('username').value = name;
  document.getElementById('remember-username').checked = true;
}

function __setRememberedPassword(password) {
  if (!password) return;
  document.getElementById('password').value = password;
}

function __fillLoginFields(username, password) {
  document.getElementById('username').value = username || '';
  document.getElementById('password').value = password || '';
}

function __initAccounts(selectedUsername, usernames) {
  const select = document.getElementById('account-select');
  select.replaceChildren();
  for (const name of usernames) {
    const opt = document.createElement('option');
    opt.value = name;
    opt.textContent = name;
    select.appendChild(opt);
  }
  if (select.options.length === 0) {
    select.selectedIndex = -1;
    return;
  }
  if (selectedUsername) {
    for (let i = 0; i < select.options.length; i++) {
      if (select.options[i].value === selectedUsername) {
        select.selectedIndex = i;
        return;
      }
    }
  }
  // Single option or no saved selection: select the first entry.
  select.selectedIndex = 0;
}

function resolutionLabel(value) {
  if (value === 'off')        return 'Off';
  if (value === 'borderless') return 'Borderless Fullscreen';
  if (value === 'fullscreen') return 'Fullscreen';
  return value;
}

function populateResolutionSelect(options, selected) {
  const select = document.getElementById('game-resolution');
  select.replaceChildren();
  for (const value of options) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = resolutionLabel(value);
    select.appendChild(option);
  }
  for (let i = 0; i < select.options.length; i++) {
    if (select.options[i].value === selected) {
      select.selectedIndex = i;
      return;
    }
  }
  select.selectedIndex = 0;
}

function __initResolutionSettings(options, selected) {
  populateResolutionSelect(options, selected);
}

// Called once on load by the C++ backend to populate settings fields and
// navigate to the user's chosen starting page.
function __initSettings(installDir, fixedWindowSize, quickLaunch, useOrigAuth, keepOpen, steamTracking, rememberPassword, enableAccountsSetting, newsCarouselSetting, startingPage) {
  document.getElementById('install-dir').value = installDir;
  document.getElementById('fixed-window-size').checked = fixedWindowSize;
  document.getElementById('quick-launch').checked = quickLaunch;
  document.getElementById('use-orig-auth').checked = useOrigAuth;
  document.getElementById('keep-open').checked = keepOpen;
  document.getElementById('steam-tracking').checked = steamTracking;
  document.getElementById('remember-password').checked = rememberPassword;
  document.getElementById('enable-accounts').checked = enableAccountsSetting;
  enableAccounts = enableAccountsSetting;
  document.getElementById('news-carousel-setting').checked = newsCarouselSetting;
  newsCarousel = newsCarouselSetting;

  const accountsOption = document.getElementById('starting-page-accounts');
  accountsOption.hidden = !enableAccounts;

  const select = document.getElementById('starting-page');
  for (let i = 0; i < select.options.length; i++) {
    if (select.options[i].value === startingPage) {
      select.selectedIndex = i;
      break;
    }
  }

  rebuildPages();

  if (startingPage && (startingPage !== 'Accounts' || enableAccounts))
    __renderPage(startingPage);
}

document.getElementById('save-settings').addEventListener('click', () => {
  const installDir       = document.getElementById('install-dir').value;
  const fixedWindowSize  = document.getElementById('fixed-window-size').checked;
  const quickLaunch      = document.getElementById('quick-launch').checked;
  const useOrigAuth      = document.getElementById('use-orig-auth').checked;
  const keepOpen         = document.getElementById('keep-open').checked;
  const steamTracking    = document.getElementById('steam-tracking').checked;
  const rememberPassword = document.getElementById('remember-password').checked;
  const enableAccountsSetting = document.getElementById('enable-accounts').checked;
  const newsCarouselSetting = document.getElementById('news-carousel-setting').checked;
  newsCarousel = newsCarouselSetting;
  let startingPage     = document.getElementById('starting-page').value;
  const gameResolution = document.getElementById('game-resolution').value;
  enableAccounts = enableAccountsSetting;
  document.getElementById('starting-page-accounts').hidden = !enableAccounts;
  if (!enableAccountsSetting && startingPage === 'Accounts') {
    document.getElementById('starting-page').selectedIndex = 0;
    startingPage = document.getElementById('starting-page').value;
  }
  rebuildPages();
  window.chrome.webview.postMessage(
    JSON.stringify({ action: 'save_settings', install_dir: installDir,
                     fixed_window_size: fixedWindowSize,
                     quick_launch: quickLaunch,
                     use_orig_auth: useOrigAuth,
                     keep_open: keepOpen,
                     steam_tracking: steamTracking,
                     remember_password: rememberPassword,
                     enable_accounts: enableAccountsSetting,
                     news_carousel: newsCarouselSetting,
                     starting_page: startingPage,
                     game_resolution: gameResolution })
  );
});

function postSelectAccount() {
  const username = document.getElementById('account-select').value;
  if (!username) return;
  window.chrome.webview.postMessage(
    JSON.stringify({ action: 'select_account', username })
  );
}

document.getElementById('account-select').addEventListener('change', postSelectAccount);

document.getElementById('fill-login').addEventListener('click', postSelectAccount);

document.getElementById('add-account').addEventListener('click', () => {
  const username = document.getElementById('account-username').value.trim();
  const password = document.getElementById('account-password').value;
  if (!username || !password) return;
  window.chrome.webview.postMessage(
    JSON.stringify({ action: 'add_account', username, password })
  );
  document.getElementById('account-username').value = '';
  document.getElementById('account-password').value = '';
});

document.getElementById('remove-account').addEventListener('click', () => {
  const username = document.getElementById('account-select').value;
  if (!username) return;
  window.chrome.webview.postMessage(
    JSON.stringify({ action: 'remove_account', username })
  );
});

(function () {
  const n = Math.floor(Math.random() * 15) + 1;
  const style = document.createElement('style');
  style.textContent = `main::before { background-image: url('assets/backgrounds/${n}.webp'); }`;
  document.head.appendChild(style);
})();

document.getElementById('play').addEventListener('click', () => {
  window.chrome.webview.postMessage(JSON.stringify({ action: 'play' }));
});

document.querySelector('.header-title').addEventListener('mousedown', (e) => {
  if (e.button === 0)
    window.chrome.webview.postMessage(JSON.stringify({ action: 'drag' }));
});

document.getElementById('minimize').addEventListener('click', () => {
  window.chrome.webview.postMessage(JSON.stringify({ action: 'minimize' }));
});

document.getElementById('close').addEventListener('click', () => {
  window.chrome.webview.postMessage(JSON.stringify({ action: 'close' }));
});