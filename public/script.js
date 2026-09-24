document.addEventListener("DOMContentLoaded", () => {
  const yearEl = document.getElementById("year");
  if (yearEl) yearEl.textContent = new Date().getFullYear();

  /* Reveille l'API des l'arrivee sur une page : l'hebergement gratuit du serveur s'endort apres
     15 min d'inactivite et met jusqu'a une minute a repartir (on ignore le resultat). */
  fetch("/api/hello", { cache: "no-store" }).catch(() => {});

  /* Page courante mise en evidence dans la navigation. */
  const here = location.pathname === "/" ? "/index.html" : location.pathname;
  document.querySelectorAll(".links a").forEach((a) => {
    if (a.pathname === here) a.setAttribute("aria-current", "page");
  });

  /* Blocs repliables ouverts sur ordinateur, replies sur telephone (et qui suivent le redimensionnement). */
  const desktop = window.matchMedia("(min-width: 641px)");
  const syncDetails = () => {
    document.querySelectorAll("details[data-desktop-open]").forEach((d) => { d.open = desktop.matches; });
  };
  syncDetails();
  if (desktop.addEventListener) desktop.addEventListener("change", syncDetails);
  else desktop.addListener(syncDetails);

  const output = document.getElementById("output");
  if (!output) return;

  async function callApi(path) {
    output.textContent = "Chargement...";
    try {
      const res = await fetch(path);
      const data = await res.json();
      output.textContent = JSON.stringify(data, null, 2);
    } catch (err) {
      output.textContent = "Erreur: " + err;
    }
  }

  document.getElementById("btn-hello")?.addEventListener("click", () => callApi("/api/hello"));
  document.getElementById("btn-time")?.addEventListener("click", () => callApi("/api/time"));
  document.getElementById("btn-visits")?.addEventListener("click", () => callApi("/api/visits"));
});
