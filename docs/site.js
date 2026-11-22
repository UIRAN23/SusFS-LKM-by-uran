/* SusFS LKM by uran - site script (language switch, tabs, demo, copy buttons) */
(function () {
  "use strict";

  // GitHub repo for links. Auto-detected from <owner>.github.io/<repo>/.
  // If the site runs on a custom domain, put "owner/repo" here.
  var REPO_OVERRIDE = "";

  var TITLES = {
    ru: "SusFS LKM by uran · SUSFS как модуль ядра Android",
    en: "SusFS LKM by uran · SUSFS as an Android kernel module"
  };
  var DESCS = {
    ru: "SUSFS в виде загружаемого модуля ядра для arm64 GKI 5.10–6.18: без пересборки ядра, с KernelSU-модулем и .ko под каждый KMI.",
    en: "SUSFS as a loadable kernel module for arm64 GKI 5.10-6.18: no kernel rebuild, with a KernelSU module and a .ko for every KMI."
  };

  function safe(fn) { try { return fn(); } catch (e) { return null; } }

  /* ---- repo links ---- */
  var repo = REPO_OVERRIDE;
  if (!repo) {
    var m = location.hostname.match(/^([^.]+)\.github\.io$/);
    var seg = location.pathname.split("/")[1];
    if (m && seg) repo = m[1] + "/" + seg;
  }
  if (repo) {
    document.querySelectorAll("[data-gh]").forEach(function (a) {
      a.href = "https://github.com/" + repo + a.getAttribute("data-gh");
    });
  }

  /* ---- language ---- */
  var html = document.documentElement;
  function pickLang() {
    var q = safe(function () { return new URLSearchParams(location.search).get("lang"); });
    if (q === "ru" || q === "en") return q;
    var saved = safe(function () { return localStorage.getItem("lang"); });
    if (saved === "ru" || saved === "en") return saved;
    var l = (navigator.languages && navigator.languages[0]) || navigator.language || "en";
    return /^(ru|uk|be|kk)/i.test(l) ? "ru" : "en";
  }
  function setLang(l, save) {
    html.lang = l;
    document.title = TITLES[l];
    var d = document.querySelector('meta[name="description"]');
    if (d) d.content = DESCS[l];
    document.querySelectorAll(".lang button").forEach(function (b) {
      b.setAttribute("aria-pressed", String(b.dataset.l === l));
    });
    if (save) safe(function () { localStorage.setItem("lang", l); });
  }
  setLang(pickLang(), false);
  document.querySelectorAll(".lang button").forEach(function (b) {
    b.addEventListener("click", function () { setLang(b.dataset.l, true); });
  });

  /* ---- hero demo: root view vs app view ---- */
  var demo = document.getElementById("demo");
  if (demo) {
    demo.querySelectorAll(".view button").forEach(function (b) {
      b.addEventListener("click", function () {
        demo.dataset.view = b.dataset.v;
        demo.querySelectorAll(".view button").forEach(function (x) {
          x.setAttribute("aria-pressed", String(x === b));
        });
      });
    });
  }

  /* ---- tabs ---- */
  document.querySelectorAll(".tabs").forEach(function (box) {
    var tabs = Array.prototype.slice.call(box.querySelectorAll('[role="tab"]'));
    function show(t) {
      tabs.forEach(function (x) {
        var on = x === t;
        x.setAttribute("aria-selected", String(on));
        x.tabIndex = on ? 0 : -1;
        document.getElementById(x.getAttribute("aria-controls")).hidden = !on;
      });
    }
    tabs.forEach(function (t, i) {
      t.addEventListener("click", function () { show(t); });
      t.addEventListener("keydown", function (e) {
        var n = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : null;
        if (n === null) return;
        n = (n + tabs.length) % tabs.length;
        show(tabs[n]); tabs[n].focus();
      });
    });
  });

  /* ---- copy buttons ---- */
  document.querySelectorAll(".copy").forEach(function (btn) {
    btn.addEventListener("click", function () {
      var pre = btn.parentElement.querySelector("pre");
      var text = pre ? pre.innerText.replace(/\n$/, "") : "";
      var done = function () {
        btn.classList.add("done");
        setTimeout(function () { btn.classList.remove("done"); }, 1400);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, function () {});
      }
    });
  });
})();
