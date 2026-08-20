/* ============================================================
   fq-compressor — FQC v2 文档站脚本
   功能：侧栏滚动高亮（scroll-spy）+ 移动端抽屉导航
   零依赖，纯原生 JS
   ============================================================ */
(function () {
  'use strict';

  var sidebar = document.getElementById('sidebar');
  var navToggle = document.getElementById('navToggle');
  var backdrop = document.getElementById('sidebarBackdrop');

  /* ---------- 移动端抽屉 ---------- */

  function closeMobileNav() {
    if (sidebar) sidebar.classList.remove('open');
    if (navToggle) {
      navToggle.classList.remove('open');
      navToggle.setAttribute('aria-expanded', 'false');
    }
    if (backdrop) backdrop.classList.remove('show');
  }

  function openMobileNav() {
    if (sidebar) sidebar.classList.add('open');
    if (navToggle) {
      navToggle.classList.add('open');
      navToggle.setAttribute('aria-expanded', 'true');
    }
    if (backdrop) backdrop.classList.add('show');
  }

  if (navToggle) {
    navToggle.addEventListener('click', function () {
      if (sidebar && sidebar.classList.contains('open')) {
        closeMobileNav();
      } else {
        openMobileNav();
      }
    });
  }
  if (backdrop) backdrop.addEventListener('click', closeMobileNav);

  // 点击任意侧栏锚点链接后关闭移动端抽屉
  if (sidebar) {
    var navLinks = sidebar.querySelectorAll('a[href^="#"]');
    Array.prototype.forEach.call(navLinks, function (a) {
      a.addEventListener('click', closeMobileNav);
    });
  }

  /* ---------- 滚动高亮（scroll-spy） ---------- */

  var topLevelIds = ['overview', 'format', 'algorithm', 'architecture', 'resources'];
  var sections = [];
  var byId = {};

  // 按侧栏导航顺序收集目标 section（去重、保持文档顺序）
  var seen = {};
  var navAnchors = document.querySelectorAll('.nav a[href^="#"]');
  Array.prototype.forEach.call(navAnchors, function (a) {
    var id = a.getAttribute('href').slice(1);
    if (!seen[id]) {
      seen[id] = true;
      var el = document.getElementById(id);
      if (el) {
        sections.push({ id: id, cat: el.getAttribute('data-cat'), top: 0 });
        byId[id] = el;
      }
    }
  });

  function measure() {
    for (var i = 0; i < sections.length; i++) {
      var el = byId[sections[i].id];
      sections[i].top = el.getBoundingClientRect().top + window.scrollY;
    }
  }

  // 找出「最靠上且仍在高亮区上方」的 section
  function findActive() {
    var probe = window.scrollY + 140;
    var current = null;
    for (var i = 0; i < sections.length; i++) {
      if (sections[i].top <= probe) {
        current = sections[i];
      } else {
        break;
      }
    }
    return current;
  }

  function setActive(id) {
    var link = document.querySelector('.nav a[href="#' + id + '"]');
    if (link) link.classList.add('active');
  }

  function applyActive(s) {
    // 清除全部高亮
    var actives = document.querySelectorAll('.nav a.active');
    Array.prototype.forEach.call(actives, function (a) {
      a.classList.remove('active');
    });
    if (!s) return;

    if (topLevelIds.indexOf(s.id) !== -1) {
      // 顶级分组：只高亮自身
      setActive(s.id);
    } else {
      // 子节：高亮自身 + 所属分组的顶级链接
      setActive(s.id);
      if (s.cat && topLevelIds.indexOf(s.cat) !== -1) setActive(s.cat);
    }
  }

  var ticking = false;
  function onScroll() {
    if (ticking) return;
    ticking = true;
    window.requestAnimationFrame(function () {
      applyActive(findActive());
      ticking = false;
    });
  }

  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', function () { measure(); onScroll(); });

  // 初始定位
  measure();
  applyActive(findActive());
})();
