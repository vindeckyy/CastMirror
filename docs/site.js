(function () {
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
})();
