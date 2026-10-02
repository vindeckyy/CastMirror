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
  // Same derivation for links to a file on the default branch. The branch is
  // read from the same place the repo is: the hostname/path, defaulting to
  // "main" (ci.yml and pages.yml both trigger on master and main).
  const branch = (location.pathname.split("/").filter(Boolean)[1] || "main");
  document.querySelectorAll("[data-repo-file]").forEach(function (el) {
    const path = el.getAttribute("data-repo-file") || "";
    el.setAttribute("href", "https://github.com/" + repo + "/blob/" + branch + "/" + path);
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
    winmain: {
      src: "assets/screenshot-windows-main.png",
      alt: "CastMirror on Windows: a navigation rail, two Cast devices as cards, and a casting panel with Start casting and the casting options",
      caption: "Windows: pick a TV, choose the entire screen or one window, and press Start casting."
    },
    winsettings: {
      src: "assets/screenshot-windows-settings.png",
      alt: "CastMirror settings on Windows: expandable groups for picture, audio, latency, discovery, desktop integration, appearance and diagnostics, with Audio open",
      caption: "Windows settings: audio source, reconnect window, shortcuts, start at sign-in and discovery."
    },
    linuxcast: {
      src: "assets/screenshot-cast.png",
      alt: "CastMirror on Linux, Cast tab",
      caption: "Linux (GTK 4): discover receivers, pick a screen or window, and tune the bitrate."
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

  // 4. Documentation Hub Navigation & Scrollspy
  const docNavLinks = document.querySelectorAll(".docs-nav-link");
  const docPanels = document.querySelectorAll(".doc-panel");

  function setActiveNavLink(targetId) {
    docNavLinks.forEach(function (link) {
      if (link.getAttribute("data-doc") === targetId) {
        link.classList.add("is-active");
      } else {
        link.classList.remove("is-active");
      }
    });
  }

  // Smooth scroll click handler
  docNavLinks.forEach(function (link) {
    link.addEventListener("click", function (e) {
      const targetId = link.getAttribute("data-doc");
      if (targetId === "all") {
        e.preventDefault();
        const docsSection = document.getElementById("documentation");
        if (docsSection) docsSection.scrollIntoView({ behavior: "smooth" });
        setActiveNavLink("all");
        history.replaceState(null, "", "#documentation");
      } else {
        e.preventDefault();
        const targetPanel = document.getElementById(targetId);
        if (targetPanel) {
          const yOffset = -80;
          const y = targetPanel.getBoundingClientRect().top + window.pageYOffset + yOffset;
          window.scrollTo({ top: y, behavior: "smooth" });
          setActiveNavLink(targetId);
          history.replaceState(null, "", "#" + targetId);
        }
      }
    });
  });

  // Scrollspy to highlight active chapter
  if ("IntersectionObserver" in window) {
    const observer = new IntersectionObserver(
      function (entries) {
        entries.forEach(function (entry) {
          if (entry.isIntersecting) {
            setActiveNavLink(entry.target.id);
          }
        });
      },
      {
        rootMargin: "-20% 0px -60% 0px",
        threshold: 0
      }
    );

    docPanels.forEach(function (panel) {
      observer.observe(panel);
    });
  }

  // Deep-link check on load
  function checkHash() {
    const hash = location.hash.replace(/^#/, "");
    if (hash === "documentation") {
      setActiveNavLink("all");
    } else if (hash && hash.startsWith("doc-")) {
      const targetPanel = document.getElementById(hash);
      if (targetPanel) {
        setTimeout(function () {
          const yOffset = -80;
          const y = targetPanel.getBoundingClientRect().top + window.pageYOffset + yOffset;
          window.scrollTo({ top: y, behavior: "smooth" });
          setActiveNavLink(hash);
        }, 100);
      }
    }
  }

  window.addEventListener("hashchange", checkHash);
  if (location.hash) {
    checkHash();
  }
})();
