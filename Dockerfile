# Serveur C pour Render (ou tout hebergeur de conteneurs).
# Etape 1 : compilation ; etape 2 : image finale minimale, sans compilateur, sans root.

FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile server.h ./
COPY *.c ./
RUN make server

FROM debian:bookworm-slim
RUN useradd --system --no-create-home --uid 10001 app
WORKDIR /app
COPY --from=build /src/server ./server
COPY public ./public
COPY data ./data
USER app

# Render fournit PORT ; cette valeur ne sert que pour un "docker run" local.
ENV PORT=10000
EXPOSE 10000

# Forme "exec" : le serveur est le processus 1 et recoit SIGTERM (arret propre, memoire liberee).
CMD ["./server"]
