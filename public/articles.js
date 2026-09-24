document.addEventListener("DOMContentLoaded", () => {
  const listEl = document.getElementById("articles-list");
  const form = document.getElementById("new-article-form");
  if (!listEl) return;

  function escapeHtml(str) {
    const div = document.createElement("div");
    div.textContent = str ?? "";
    return div.innerHTML;
  }

  function escapeAttr(str) {
    return escapeHtml(str).replace(/"/g, "&quot;");
  }

  async function loadArticles() {
    listEl.textContent = "Chargement...";
    try {
      const res = await fetch("/api/articles");
      const items = await res.json();
      renderArticles(items);
    } catch (err) {
      listEl.textContent = "Erreur de chargement: " + err;
    }
  }

  function renderArticles(items) {
    listEl.innerHTML = "";
    if (items.length === 0) {
      listEl.innerHTML = "<p>Aucun article pour le moment.</p>";
      return;
    }
    items.forEach((a) => listEl.appendChild(renderArticle(a)));
  }

  function renderArticle(a) {
    const el = document.createElement("article");
    el.className = "article-item";
    renderView(el, a);
    return el;
  }

  function renderView(el, a) {
    el.innerHTML = `
      <h3>${escapeHtml(a.title)}</h3>
      <p>${escapeHtml(a.content)}</p>
      <small>${escapeHtml(a.created_at)} &middot; #${a.id}</small>
      <div class="article-actions">
        <button class="edit-btn">Modifier</button>
        <button class="delete-btn">Supprimer</button>
      </div>
    `;

    el.querySelector(".delete-btn").addEventListener("click", async () => {
      await fetch(`/api/articles/${a.id}`, { method: "DELETE" });
      loadArticles();
    });

    el.querySelector(".edit-btn").addEventListener("click", () => renderEdit(el, a));
  }

  function renderEdit(el, a) {
    el.innerHTML = `
      <input type="text" class="edit-title" value="${escapeAttr(a.title)}">
      <textarea class="edit-content" rows="3">${escapeHtml(a.content)}</textarea>
      <div class="article-actions">
        <button class="save-btn">Enregistrer</button>
        <button class="cancel-btn">Annuler</button>
      </div>
    `;

    el.querySelector(".save-btn").addEventListener("click", async () => {
      const title = el.querySelector(".edit-title").value;
      const content = el.querySelector(".edit-content").value;
      const res = await fetch(`/api/articles/${a.id}`, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ title, content }),
      });
      const updated = await res.json();
      renderView(el, updated);
    });

    el.querySelector(".cancel-btn").addEventListener("click", () => renderView(el, a));
  }

  form?.addEventListener("submit", async (e) => {
    e.preventDefault();
    const title = document.getElementById("new-title").value;
    const content = document.getElementById("new-content").value;
    await fetch("/api/articles", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ title, content }),
    });
    form.reset();
    loadArticles();
  });

  loadArticles();
});
