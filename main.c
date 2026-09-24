/*
 * Petit serveur HTTP en C.
 * - Sert les fichiers statiques du dossier ./public
 * - Expose des routes dynamiques sous /api
 * - API REST complete pour /api/articles (GET/POST/PUT/DELETE, JSON)
 * - Logique complete du Scrabble cote serveur (/api/scrabble, voir scrabble.c)
 *
 * Compilation : make            (make asan / make test pour les verifications memoire)
 * Lancement   : ./server [port]   (sinon variable PORT, sinon 8080) ; Ctrl+C / SIGTERM pour arreter proprement
 *
 * Organisation du code (declarations dans server.h) :
 *   mem.c            allocateur trace : chaque allocation/liberation est comptee
 *   dynbuf.c         buffer de caracteres extensible
 *   util.c           helpers generiques (recherche, decodage URL, dates...)
 *   http.c           envoi de reponses, lecture complete d'une requete
 *   json.c           mini-JSON maison (lecture/ecriture)
 *   articles.c       modele Article + API REST /api/articles
 *   routes.c         routes /api/hello|time|visits|contact + fichiers statiques
 *   dict.c           dictionnaire francais du Scrabble (chargement, index)
 *   scrabble.c       regles, score, IA et suggestions du Scrabble
 *   scrabble_api.c   parties en memoire et routes HTTP du Scrabble
 *   main.c           reseau (accept + pool de workers), dispatch, arret propre
 *
 * Memoire : un pool fixe de NUM_WORKERS threads (joints a l'arret) traite les connexions.
 * A l'arret, tout est libere (parties, dictionnaire, articles) puis mem_report() verifie
 * que chaque allocation a ete liberee ; le code de sortie est 3 en cas de fuite.
 */

#include "server.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---------- File des connexions acceptees, consommee par les workers ---------- */

static struct {
    int fds[CONN_QUEUE_SIZE];
    int head, count;
    int closing;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} g_queue = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static int queue_push(int fd) {
    pthread_mutex_lock(&g_queue.lock);
    if (g_queue.count == CONN_QUEUE_SIZE) {
        pthread_mutex_unlock(&g_queue.lock);
        return -1;
    }
    g_queue.fds[(g_queue.head + g_queue.count) % CONN_QUEUE_SIZE] = fd;
    g_queue.count++;
    pthread_cond_signal(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
    return 0;
}

/* Bloque jusqu'a une connexion. Retourne -1 quand la file est fermee et vide. */
static int queue_pop(int *fd) {
    pthread_mutex_lock(&g_queue.lock);
    while (g_queue.count == 0 && !g_queue.closing) pthread_cond_wait(&g_queue.cond, &g_queue.lock);
    if (g_queue.count == 0) {
        pthread_mutex_unlock(&g_queue.lock);
        return -1;
    }
    *fd = g_queue.fds[g_queue.head];
    g_queue.head = (g_queue.head + 1) % CONN_QUEUE_SIZE;
    g_queue.count--;
    pthread_mutex_unlock(&g_queue.lock);
    return 0;
}

static void queue_close(void) {
    pthread_mutex_lock(&g_queue.lock);
    g_queue.closing = 1;
    pthread_cond_broadcast(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
}

/* ---------- Traitement d'une connexion ---------- */

static void handle_client(int fd) {
    char *raw = NULL;
    size_t raw_len = 0;
    if (read_http_request(fd, &raw, &raw_len) != 0 || raw_len == 0) {
        xfree(raw);
        close(fd);
        return;
    }

    char method[8] = "", path[512] = "", version[16] = "";
    sscanf(raw, "%7s %511s %15s", method, path, version);

    char clean_path[512];
    snprintf(clean_path, sizeof(clean_path), "%s", path);
    char *qmark = strchr(clean_path, '?');
    if (qmark) *qmark = '\0';

    const char *header_end = mem_find(raw, raw_len, "\r\n\r\n");
    const char *body = "";
    if (header_end) body = header_end + 4;

    printf("%s %s\n", method, clean_path);

    if (strcmp(clean_path, "/api/scrabble") == 0 || starts_with(clean_path, "/api/scrabble/")) {
        scrabble_handle(fd, method, clean_path, body);
    } else if (strcmp(method, "GET") == 0) {
        if (strcmp(clean_path, "/api/hello") == 0) {
            handle_api_hello(fd);
        } else if (strcmp(clean_path, "/api/time") == 0) {
            handle_api_time(fd);
        } else if (strcmp(clean_path, "/api/visits") == 0) {
            handle_api_visits(fd);
        } else if (strcmp(clean_path, "/api/articles") == 0) {
            handle_articles_list(fd);
        } else if (starts_with(clean_path, "/api/articles/")) {
            int id = parse_id_after_prefix(clean_path, "/api/articles/");
            if (id < 0) send_json_error(fd, "400 Bad Request", "identifiant invalide");
            else handle_articles_get(fd, id);
        } else {
            serve_static(fd, clean_path);
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(clean_path, "/api/contact") == 0) {
            handle_api_contact(fd, body);
        } else if (strcmp(clean_path, "/api/articles") == 0) {
            handle_articles_create(fd, body);
        } else {
            send_json_error(fd, "404 Not Found", "route inconnue");
        }
    } else if (strcmp(method, "PUT") == 0) {
        if (starts_with(clean_path, "/api/articles/")) {
            int id = parse_id_after_prefix(clean_path, "/api/articles/");
            if (id < 0) send_json_error(fd, "400 Bad Request", "identifiant invalide");
            else handle_articles_update(fd, id, body);
        } else {
            send_json_error(fd, "404 Not Found", "route inconnue");
        }
    } else if (strcmp(method, "DELETE") == 0) {
        if (starts_with(clean_path, "/api/articles/")) {
            int id = parse_id_after_prefix(clean_path, "/api/articles/");
            if (id < 0) send_json_error(fd, "400 Bad Request", "identifiant invalide");
            else handle_articles_delete(fd, id);
        } else {
            send_json_error(fd, "404 Not Found", "route inconnue");
        }
    } else {
        send_response(fd, "405 Method Not Allowed", "text/plain", "Methode non autorisee", 21);
    }

    xfree(raw);
    close(fd);
}

static void *worker_main(void *arg) {
    (void)arg;
    int fd;
    while (queue_pop(&fd) == 0) handle_client(fd);
    return NULL;
}

/* ---------- Arret propre ---------- */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int listen_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }
    return server_fd;
}

/* Port : argument de la ligne de commande, sinon variable PORT (imposee par Render), sinon 8080. */
static int parse_port(int argc, char *argv[]) {
    const char *text = argc > 1 ? argv[1] : getenv("PORT");
    if (!text || !*text) return PORT_DEFAULT;
    char *end;
    long v = strtol(text, &end, 10);
    if (*end != '\0' || v <= 0 || v > 65535) {
        fprintf(stderr, "Port invalide : %s\n", text);
        return -1;
    }
    return (int)v;
}

int main(int argc, char *argv[]) {
    int port = parse_port(argc, argv);
    if (port < 0) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    seed_articles();
    if (scrabble_init(SCRABBLE_WORDS_FILE) != 0) {
        fprintf(stderr, "Avertissement : dictionnaire '%s' introuvable ou invalide, Scrabble desactive.\n",
                SCRABBLE_WORDS_FILE);
    }

    int exit_code = 0;
    int server_fd = listen_socket(port);
    if (server_fd < 0) {
        exit_code = 1;
        goto cleanup;
    }

    /* Les workers ne doivent pas recevoir les signaux d'arret : seul le thread principal les traite. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    pthread_t workers[NUM_WORKERS];
    int nworkers = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (pthread_create(&workers[nworkers], NULL, worker_main, NULL) == 0) nworkers++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (nworkers == 0) {
        fprintf(stderr, "Impossible de creer les threads de traitement.\n");
        exit_code = 1;
        close(server_fd);
        goto cleanup;
    }

    printf("Serveur demarre sur http://localhost:%d (%d workers, Ctrl+C pour arreter)\n", port, nworkers);

    struct pollfd pfd = { .fd = server_fd, .events = POLLIN, .revents = 0 };
    while (!g_stop) {
        int pr = poll(&pfd, 1, 200); /* delai court : on reverifie g_stop regulierement */
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pr == 0) continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) perror("accept");
            if (errno == EMFILE || errno == ENFILE) usleep(50000); /* plus de descripteurs : ne pas tourner a vide */
            continue;
        }

        /* Un client qui n'envoie ou ne lit rien ne doit pas bloquer un worker (ni l'arret). */
        struct timeval tv = { .tv_sec = IO_TIMEOUT_SECONDS, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        if (queue_push(client_fd) != 0) {
            send_response(client_fd, "503 Service Unavailable", "text/plain", "Serveur occupe", 14);
            close(client_fd);
        }
    }

    printf("Arret en cours...\n");
    close(server_fd);
    queue_close(); /* les workers finissent les connexions deja acceptees puis se terminent */
    for (int i = 0; i < nworkers; i++) pthread_join(workers[i], NULL);

cleanup:
    scrabble_cleanup();
    articles_free();
    if (mem_report(stdout) != 0 && exit_code == 0) exit_code = 3;
    return exit_code;
}
