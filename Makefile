CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread -I.
# Build de verification : AddressSanitizer (fuites, debordements, use-after-free) + UBSan.
SANFLAGS = -Wall -Wextra -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -pthread -I.
TARGET = server

SRCS = main.c dynbuf.c util.c http.c json.c articles.c routes.c mem.c dict.c scrabble.c scrabble_api.c
OBJS = $(SRCS:.c=.o)
DEPS = server.h

# Tout sauf main.c : utilise pour lier les tests.
LIB_SRCS = $(filter-out main.c,$(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

# Serveur instrumente : a lancer a la place de ./server pour traquer les erreurs memoire.
asan: server_asan

server_asan: $(SRCS) $(DEPS)
	$(CC) $(SANFLAGS) -o server_asan $(SRCS)

# Tests unitaires du moteur de jeu, compiles et executes sous ASan/UBSan.
test: tests/test_scrabble
	./tests/test_scrabble

tests/test_scrabble: tests/test_scrabble.c $(LIB_SRCS) $(DEPS)
	$(CC) $(SANFLAGS) -o tests/test_scrabble tests/test_scrabble.c $(LIB_SRCS)

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(TARGET) server_asan tests/test_scrabble

re: fclean all
	rm -f $(OBJS)

.PHONY: all run asan test clean fclean re
