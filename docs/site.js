(function () {
  // 1. GitHub repo and pages link interpolation
  const host = location.hostname;
  const m = host.match(/^([^.]+)\.github\.io$/);
  let repo = "vindeckyy/CastMirror";
  if (m) {
    const user = m[1];
    const parts = location.pathname.split("/").filter(Boolean);
    const name = parts[0] || "CastMirror";
    repo = user + "/" + name;
  }
  document.querySelectorAll("[data-repo-href]").forEach(function (el) {
    const path = el.getAttribute("data-repo-href") || "";
    el.setAttribute("href", "https://github.com/" + repo + path);
  });
  document.querySelectorAll("[data-pages-href]").forEach(function (el) {
    el.setAttribute("href", "https://" + repo.split("/")[0] + ".github.io/" + repo.split("/")[1] + "/");
  });

  // 2. Clipboard copy button
  document.querySelectorAll(".copy").forEach(function (btn) {
    btn.addEventListener("click", function () {
      const pre = btn.parentElement;
      const text = pre.innerText.replace(/^Copy\n?/, "");
      navigator.clipboard.writeText(text.trim()).then(function () {
        btn.textContent = "Copied";
        setTimeout(function () { btn.textContent = "Copy"; }, 1400);
      });
    });
  });

  // 3. Hero App Showcase Tab Switcher
  const showcaseTabs = document.querySelectorAll(".showcase-tab-btn");
  const showcaseImg = document.getElementById("showcase-img");
  const showcaseCaption = document.getElementById("showcase-caption");

  const showcaseData = {
    cast: {
      src: "assets/screenshot-cast.png",
      alt: "CastMirror — Cast Tab",
      caption: "Cast Tab: Discover receivers, select screen or window with app icons, and tune bitrate."
    },
    live: {
      src: "assets/screenshot-live.png",
      alt: "CastMirror — Live Session Tab",
      caption: "Live Session Tab: Real-time vector sparklines (FPS, Bitrate, RTT, Loss) and studio freeze/mute controls."
    },
    settings: {
      src: "assets/screenshot-settings.png",
      alt: "CastMirror — Settings Tab",
      caption: "Settings Tab: Quality presets, audio capture, playout buffer delay, theme switcher, and diagnostics."
    },
    logs: {
      src: "assets/screenshot-logs.png",
      alt: "CastMirror — Logs Tab",
      caption: "Logs Tab: Searchable event stream with severity filters and instant folder access."
    }
  };

  showcaseTabs.forEach(function (tab) {
    tab.addEventListener("click", function () {
      const target = tab.getAttribute("data-tab");
      if (!showcaseData[target] || !showcaseImg) return;

      showcaseTabs.forEach(function (t) { t.classList.remove("is-active"); });
      tab.classList.add("is-active");

      showcaseImg.style.opacity = "0.4";
      setTimeout(function () {
        showcaseImg.src = showcaseData[target].src;
        showcaseImg.alt = showcaseData[target].alt;
        if (showcaseCaption) showcaseCaption.textContent = showcaseData[target].caption;
        showcaseImg.style.opacity = "1";
      }, 100);
    });
  });

  // 4. Documentation Hub Tab / Panel Switcher
  const docNavLinks = document.querySelectorAll(".docs-nav-link");
  const docPanels = document.querySelectorAll(".doc-panel");

  function switchDoc(docId, scrollIntoView) {
    if (!docId) docId = "doc-building";
    const targetLink = document.querySelector('.docs-nav-link[data-doc="' + docId + '"]');
    const targetPanel = document.getElementById(docId);

    if (!targetPanel || !targetLink) return;

    docNavLinks.forEach(function (link) { link.classList.remove("is-active"); });
    docPanels.forEach(function (panel) { panel.classList.remove("is-active"); });

    targetLink.classList.add("is-active");
    targetPanel.classList.add("is-active");

    if (scrollIntoView) {
      const docsSection = document.getElementById("documentation");
      if (docsSection) {
        docsSection.scrollIntoView({ behavior: "smooth" });
      }
    }
  }

  docNavLinks.forEach(function (link) {
    link.addEventListener("click", function (e) {
      e.preventDefault();
      const docId = link.getAttribute("data-doc");
      location.hash = docId;
      switchDoc(docId, false);
    });
  });

  // Handle hash changes or initial hash
  function checkHash() {
    const hash = location.hash.replace(/^#/, "");
    if (hash && hash.startsWith("doc-")) {
      switchDoc(hash, true);
    }
  }

  window.addEventListener("hashchange", checkHash);
  checkHash();
})();
