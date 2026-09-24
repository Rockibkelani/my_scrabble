# MyScrabble

Un Scrabble en français jouable dans le navigateur, contre une IA ou à deux sur le même écran.
Toute la logique du jeu tourne sur un serveur HTTP écrit en C, sans framework ni bibliothèque externe.

**Jouer : https://myscrabblee.netlify.app**

## Fonctionnalités

- Partie contre l'IA, qui cherche à chaque tour le coup qui rapporte le plus.
- Partie à deux sur le même appareil.
- Dictionnaire français de plus de 310 000 mots (pluriels et conjugaisons compris, sans accents).
- Suggestions des meilleurs mots jouables avec ses lettres.
- Règles complètes : cases bonus, jokers, bonus de 50 points pour 7 lettres posées, échange, passe.
- Site adapté au téléphone.

## Lancer en local

Il faut Linux (ou WSL), `gcc` et `make`.

```sh
make            # compile ./server
./server        # http://localhost:8080
./server 3000   # sur un autre port (ou variable d'environnement PORT)
```

`Ctrl+C` arrête le serveur proprement : il libère toute sa mémoire et le vérifie avant de quitter.

## Tests

```sh
make test       # tests du moteur de jeu, compilés avec AddressSanitizer et UBSan
make asan       # serveur instrumenté (./server_asan) pour traquer les erreurs mémoire
```

## Organisation

| Fichier | Rôle |
| --- | --- |
| `main.c` | Réseau (accept + pool de 16 threads), aiguillage des requêtes, arrêt propre |
| `scrabble.c` | Règles, calcul des points, IA et suggestions |
| `scrabble_api.c` | Parties en mémoire et routes `/api/scrabble` |
| `dict.c` | Chargement et recherche dans le dictionnaire (`data/words.txt`) |
| `routes.c` | Route `/api/hello` et fichiers statiques de `public/` |
| `http.c` | Lecture des requêtes et envoi des réponses HTTP |
| `json.c` | Lecture et écriture JSON écrites à la main |
| `mem.c` | Allocateur qui compte chaque allocation et libération |
| `dynbuf.c`, `util.c` | Tampon extensible et petites fonctions utilitaires |
| `server.h` | Configuration et déclarations communes |
| `public/` | Pages du site (`index.html`, `scrabble.html`, `about.html`), CSS et JS |
| `tests/` | Tests unitaires |

## API

Toutes les réponses sont en JSON. Une partie est identifiée par l'`id` renvoyé à sa création.

| Méthode | Route | Rôle |
| --- | --- | --- |
| `POST` | `/api/scrabble/new` | Nouvelle partie, corps `{"mode": "ai"}` ou `{"mode": "pvp"}` |
| `GET` | `/api/scrabble/:id` | État de la partie |
| `POST` | `/api/scrabble/:id/play` | Poser un mot, corps `{"tiles": [{"id", "row", "col", "letter"?}]}` (`letter` pour un joker) |
| `POST` | `/api/scrabble/:id/exchange` | Échanger des lettres, corps `{"tile_ids": [...]}` |
| `POST` | `/api/scrabble/:id/pass` | Passer son tour |
| `POST` | `/api/scrabble/:id/ai` | Faire jouer l'IA |
| `POST` | `/api/scrabble/:id/suggest` | Meilleurs coups possibles |
| `POST` | `/api/scrabble/:id/end` | Terminer la partie |
| `DELETE` | `/api/scrabble/:id` | Supprimer la partie |
| `GET` | `/api/hello` | Réponse minimale, sert à réveiller le serveur |

Une partie inactive depuis plus d'une heure est supprimée ; 64 parties au plus peuvent tourner en même temps.

## Déploiement

- **Serveur C sur Render** : `render.yaml` crée le service à partir du `Dockerfile` (compilation, puis image
  minimale sans droits root). Render fournit le port dans la variable `PORT`.
- **Site sur Netlify** : `netlify.toml` publie `public/` et relaie `/api/*` vers Render. Le navigateur ne
  parle qu'à Netlify, donc pas de CORS.

Chaque `git push` sur `main` redéploie les deux.

### Limites de l'offre gratuite

- Le serveur Render s'endort après 15 minutes sans requête et met 30 à 60 secondes à se réveiller.
  Le site affiche « Le serveur se réveille… » et réessaie tout seul.
- Les parties vivent en mémoire : elles sont perdues à chaque mise en veille ou redéploiement.
