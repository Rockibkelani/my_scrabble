/*
 * Scrabble : client leger. Toute la logique (regles, score, dictionnaire, IA,
 * suggestions) est cote serveur, en C (/api/scrabble, voir scrabble.c).
 * Ce fichier ne fait qu'afficher l'etat recu et envoyer les actions du joueur.
 *
 * Etat local : uniquement les lettres posees "en attente" ce tour-ci (turnTiles),
 * envoyees au serveur d'un seul coup quand on clique sur "Valider le mot".
 */

(function () {
  const BOARD_SIZE = 15;
  const AI_DELAY_MS = 700;
  const PREMIUM_CLASS = { l: "dl", L: "tl", w: "dw", W: "tw" };
  const PREMIUM_LABEL = { l: "LD", L: "LT", w: "MD", W: "MT" };

  const $ = (id) => document.getElementById(id);
  const modeScreenEl = $("scrabble-mode-screen");
  const gameScreenEl = $("scrabble-game-screen");
  const boardEl = $("scrabble-board");
  const rackEl = $("scrabble-rack");
  const bagEl = $("scrabble-bag-count");
  const messageEl = $("scrabble-message");
  const turnIndicatorEl = $("scrabble-turn-indicator");
  const suggestionsEl = $("scrabble-suggestions");
  const statusEl = $("scrabble-status");
  const playerEls = [
    { box: $("scrabble-p1-box"), name: $("scrabble-p1-name"), score: $("scrabble-score-p1") },
    { box: $("scrabble-p2-box"), name: $("scrabble-p2-name"), score: $("scrabble-score-p2") },
  ];

  if (!boardEl) return; /* on n'est pas sur la page scrabble.html */

  let state = null;         /* dernier etat envoye par le serveur */
  let turnTiles = [];       /* lettres posees en attente : {id, row, col, letter, points, blank} */
  let selectedOrder = [];   /* ids des lettres de la main selectionnees */
  let busy = false;         /* une requete est en cours ou l'IA joue : on ignore les clics */
  let aiTimer = null;

  /* ---------- Reseau ---------- */

  async function api(method, path, body) {
    try {
      const res = await fetch("/api/scrabble" + path, {
        method,
        headers: body ? { "Content-Type": "application/json" } : undefined,
        body: body ? JSON.stringify(body) : undefined,
      });
      let data = null;
      try { data = await res.json(); } catch (e) { /* corps vide ou invalide */ }
      return { ok: res.ok, status: res.status, data };
    } catch (e) {
      return { ok: false, status: 0, data: null };
    }
  }

  /* Reponse du relais (Netlify) ou reseau : le serveur dort ou redemarre, ce n'est pas une erreur de jeu. */
  function isServerAsleep(res) {
    return res.status === 0 || res.status === 502 || res.status === 504 || (res.status === 503 && !(res.data && res.data.error));
  }

  function errorText(res) {
    if (res.data && res.data.error) return res.data.error;
    if (res.status === 404) return "Cette partie n'existe plus (le serveur a redemarre). Changez de mode pour en lancer une nouvelle.";
    if (isServerAsleep(res)) return "Le serveur ne repond pas. Reessayez dans quelques secondes.";
    return "Erreur du serveur.";
  }

  function setMessage(msg) {
    messageEl.textContent = msg;
  }

  function setBusy(value) {
    busy = value;
    document.body.classList.toggle("ai-turn", value);
  }

  /* Applique un etat recu du serveur, puis declenche le tour de l'IA si necessaire. */
  function applyState(newState) {
    state = newState;
    turnTiles = [];
    selectedOrder = [];
    hideSuggestions();
    setMessage(state.message);
    render();
    clearTimeout(aiTimer);
    if (state.ai_pending) {
      setBusy(true);
      aiTimer = setTimeout(playAi, AI_DELAY_MS);
    } else {
      setBusy(false);
    }
  }

  let aiFailures = 0;

  async function playAi() {
    if (!state) return;
    const res = await api("POST", `/${state.id}/ai`);
    if (res.ok) {
      aiFailures = 0;
      applyState(res.data);
      return;
    }
    if (isServerAsleep(res) && ++aiFailures <= 5) {
      /* Le serveur dort ou redemarre : on relit l'etat (l'IA a peut-etre deja joue) puis on retente. */
      setMessage("Le serveur se reveille, nouvelle tentative...");
      aiTimer = setTimeout(async () => {
        if (!state) return;
        const current = await api("GET", `/${state.id}`);
        if (current.ok) applyState(current.data);
        else playAi();
      }, 3000);
      return;
    }
    aiFailures = 0;
    setBusy(false);
    setMessage(errorText(res));
  }

  /* Action envoyee au serveur : en cas de refus, on garde les lettres posees et on affiche l'erreur. */
  async function act(path, body) {
    if (busy || !state) return;
    setBusy(true);
    const res = await api("POST", `/${state.id}/${path}`, body);
    if (res.ok) {
      applyState(res.data);
    } else {
      setBusy(false);
      setMessage(errorText(res));
    }
  }

  /* ---------- Ecran de mode ---------- */

  /* L'hebergement gratuit met le serveur en veille : le premier appel peut echouer (502/504 du relais)
     le temps qu'il se reveille, donc on reessaie quelques fois en le disant a l'utilisateur. */
  async function startGame(mode) {
    if (busy) return;
    setBusy(true);
    statusEl.textContent = "";
    let res;
    for (let attempt = 0; attempt < 8; attempt++) {
      res = await api("POST", "/new", { mode });
      if (res.ok || !isServerAsleep(res)) break;
      statusEl.textContent = "Le serveur se reveille (hebergement gratuit), cela peut prendre une minute...";
      await new Promise((resolve) => setTimeout(resolve, 4000));
    }
    if (!res.ok) {
      setBusy(false);
      statusEl.textContent = errorText(res);
      return;
    }
    statusEl.textContent = "";
    modeScreenEl.classList.add("hidden");
    gameScreenEl.classList.remove("hidden");
    applyState(res.data);
  }

  function leaveGame(keepalive) {
    clearTimeout(aiTimer);
    if (state) {
      /* Libere la partie cote serveur (sinon elle expire toute seule apres inactivite). */
      fetch(`/api/scrabble/${state.id}`, { method: "DELETE", keepalive: !!keepalive }).catch(() => {});
      state = null;
    }
    turnTiles = [];
    selectedOrder = [];
    setBusy(false);
  }

  function backToModeScreen() {
    leaveGame(false);
    gameScreenEl.classList.add("hidden");
    modeScreenEl.classList.remove("hidden");
  }

  /* ---------- Suggestions ---------- */

  function hideSuggestions() {
    suggestionsEl.classList.add("hidden");
    suggestionsEl.innerHTML = "";
  }

  async function showSuggestions() {
    if (busy || !state || state.game_over) return;
    setBusy(true);
    const res = await api("POST", `/${state.id}/suggest`);
    setBusy(false);
    if (!res.ok) {
      setMessage(errorText(res));
      return;
    }
    const list = res.data.suggestions;
    suggestionsEl.classList.remove("hidden");
    if (list.length === 0) {
      suggestionsEl.textContent = "Aucun mot jouable trouve avec vos lettres actuelles.";
      return;
    }
    suggestionsEl.innerHTML = "<p>Suggestions (cliquez pour poser sur le plateau) :</p>";
    const box = document.createElement("div");
    box.className = "suggestion-list";
    list.forEach((s) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "suggestion-btn";
      btn.textContent = `${s.word} (+${s.score})`;
      btn.addEventListener("click", () => applySuggestion(s));
      box.appendChild(btn);
    });
    suggestionsEl.appendChild(box);
  }

  /* Pose les lettres d'une suggestion en attente ; le joueur valide (ou reprend) ensuite. */
  function applySuggestion(s) {
    if (busy || !state) return;
    turnTiles = [];
    selectedOrder = [];
    for (const t of s.tiles) {
      const tile = state.rack.find((r) => r.id === t.id);
      if (!tile) continue;
      turnTiles.push({ id: t.id, row: t.row, col: t.col, letter: t.letter, points: tile.points, blank: tile.blank });
    }
    hideSuggestions();
    setMessage(`"${s.word}" (+${s.score} points) est pose : validez le mot ou reprenez vos lettres.`);
    render();
  }

  /* ---------- Actions ---------- */

  function validateTurn() {
    if (turnTiles.length === 0) {
      setMessage("Placez au moins une lettre sur le plateau.");
      return;
    }
    act("play", { tiles: turnTiles.map((t) => ({ id: t.id, row: t.row, col: t.col, letter: t.blank ? t.letter : "" })) });
  }

  function recallTiles() {
    if (busy) return;
    turnTiles = [];
    selectedOrder = [];
    hideSuggestions();
    setMessage("Lettres reprises en main.");
    render();
  }

  function exchangeSelection() {
    if (selectedOrder.length === 0) {
      setMessage("Selectionnez au moins une lettre a echanger.");
      return;
    }
    act("exchange", { tile_ids: selectedOrder });
  }

  function passTurn() {
    act("pass");
  }

  function endGame() {
    act("end");
  }

  /* ---------- Interaction ---------- */

  function onBoardClick(e) {
    if (busy || !state || state.game_over) return;
    const cell = e.target.closest(".scrabble-cell");
    if (!cell) return;
    const r = Number(cell.dataset.row);
    const c = Number(cell.dataset.col);
    if (state.tiles.some((t) => t.r === r && t.c === c)) return;

    const turnIdx = turnTiles.findIndex((t) => t.row === r && t.col === c);
    if (turnIdx >= 0) {
      turnTiles.splice(turnIdx, 1);
      hideSuggestions();
      render();
      return;
    }

    if (selectedOrder.length === 0) {
      setMessage("Selectionnez une lettre dans votre main, puis cliquez sur une case.");
      return;
    }
    const id = selectedOrder[0];
    const tile = state.rack.find((t) => t.id === id);
    if (!tile) {
      selectedOrder.shift();
      render();
      return;
    }

    let letter = tile.letter;
    if (tile.blank) {
      const chosen = prompt("Quelle lettre ce jeton blanc doit-il representer ?");
      if (!chosen) return;
      letter = chosen.trim().toUpperCase();
      if (!/^[A-Z]$/.test(letter)) {
        setMessage("Veuillez entrer une seule lettre (A-Z).");
        return;
      }
    }
    selectedOrder.shift();
    turnTiles.push({ id: tile.id, row: r, col: c, letter, points: tile.points, blank: tile.blank });
    hideSuggestions();
    render();
  }

  function onRackClick(e) {
    if (busy || !state || state.game_over) return;
    const tileEl = e.target.closest(".rack-tile");
    if (!tileEl) return;
    const id = Number(tileEl.dataset.id);
    const pos = selectedOrder.indexOf(id);
    if (pos >= 0) selectedOrder.splice(pos, 1);
    else selectedOrder.push(id);
    render();
  }

  /* ---------- Rendu ---------- */

  function render() {
    if (!state) return;
    renderBoard();
    renderRack();
    state.players.forEach((p, i) => {
      playerEls[i].name.textContent = p.name;
      playerEls[i].score.textContent = String(p.score);
      playerEls[i].box.classList.toggle("active-player", state.current === i && !state.game_over);
    });
    bagEl.textContent = String(state.bag);
    turnIndicatorEl.textContent = state.game_over ? "Partie terminee" : `Au tour de ${state.players[state.current].name}`;
  }

  function renderBoard() {
    const placed = new Map();
    state.tiles.forEach((t) => placed.set(`${t.r},${t.c}`, { letter: t.l, points: t.p, blank: t.b, pending: false }));
    turnTiles.forEach((t) => placed.set(`${t.row},${t.col}`, { letter: t.letter, points: t.points, blank: t.blank, pending: true }));

    let html = "";
    for (let r = 0; r < BOARD_SIZE; r++) {
      for (let c = 0; c < BOARD_SIZE; c++) {
        const tile = placed.get(`${r},${c}`);
        const premium = state.premium[r][c];
        const classes = ["scrabble-cell"];
        if (PREMIUM_CLASS[premium]) classes.push(PREMIUM_CLASS[premium]);
        if (r === 7 && c === 7) classes.push("center");
        if (tile) classes.push("filled");
        if (tile && tile.pending) classes.push("tentative");
        if (tile && tile.blank) classes.push("blank-tile");

        let content = "";
        if (tile) {
          content = `<span class="cell-letter">${tile.letter}</span><span class="cell-pts">${tile.blank ? "" : tile.points}</span>`;
        } else if (r === 7 && c === 7) {
          content = `<span class="cell-star">&#9733;</span>`;
        } else if (PREMIUM_LABEL[premium]) {
          content = `<span class="cell-premium">${PREMIUM_LABEL[premium]}</span>`;
        }
        html += `<div class="${classes.join(" ")}" data-row="${r}" data-col="${c}">${content}</div>`;
      }
    }
    boardEl.innerHTML = html;
  }

  function renderRack() {
    let html = "";
    for (const tile of state.rack) {
      if (turnTiles.some((t) => t.id === tile.id)) continue;
      const classes = ["rack-tile"];
      if (selectedOrder.includes(tile.id)) classes.push("selected");
      if (tile.blank) classes.push("blank-tile");
      const letter = tile.blank ? "&#9734;" : tile.letter;
      const pts = tile.blank ? "" : tile.points;
      html += `<div class="${classes.join(" ")}" data-id="${tile.id}"><span class="cell-letter">${letter}</span><span class="pts">${pts}</span></div>`;
    }
    rackEl.innerHTML = html;
  }

  boardEl.addEventListener("click", onBoardClick);
  rackEl.addEventListener("click", onRackClick);
  $("scrabble-validate").addEventListener("click", validateTurn);
  $("scrabble-recall").addEventListener("click", recallTiles);
  $("scrabble-hint").addEventListener("click", showSuggestions);
  $("scrabble-exchange").addEventListener("click", exchangeSelection);
  $("scrabble-pass").addEventListener("click", passTurn);
  $("scrabble-end").addEventListener("click", endGame);
  $("scrabble-new").addEventListener("click", backToModeScreen);
  $("scrabble-mode-pvp").addEventListener("click", () => startGame("pvp"));
  $("scrabble-mode-ai").addEventListener("click", () => startGame("ai"));
  window.addEventListener("pagehide", () => leaveGame(true));

  /* Lien direct depuis l'accueil (scrabble.html?mode=ai ou ?mode=pvp) : la partie demarre tout de suite.
     On retire le parametre de l'adresse pour que "Changer de mode" puis un rechargement ne relance rien. */
  const params = new URLSearchParams(location.search);
  const initialMode = params.get("mode");
  if (initialMode === "ai" || initialMode === "pvp") {
    history.replaceState(null, "", location.pathname);
    startGame(initialMode);
  }
})();
