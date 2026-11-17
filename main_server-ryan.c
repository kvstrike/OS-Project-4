#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define PORT_NUM 8083

#define RESETRGB "\033[0m"

void error(const char *msg)
{
	perror(msg);
	exit(1);
}

typedef struct _ThreadArgs {
	int clisockfd;
	struct in_addr ip;
} ThreadArgs;

#define MAX_CLIENTS 100

typedef struct {
    int socket;
    char name[256];
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void broadcast(char *message, int sender_socket) {
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket != sender_socket) {
            send(clients[i].socket, message, strlen(message), 0);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(int socket) {
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket == socket) {

            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }

            client_count--;
            break;
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

void* thread_main(void* args) {
    pthread_detach(pthread_self());

    int clisockfd = ((ThreadArgs*) args)->clisockfd;
	struct in_addr ip = ((ThreadArgs*) args)->ip;
    free(args);

    char buffer[256];
    char name[256];

    int nrcv = recv(clisockfd, name, sizeof(name) - 1, 0);

    if (nrcv <= 0) {
        close(clisockfd);
        return NULL;
    }

    name[nrcv] = '\0';

    pthread_mutex_lock(&clients_mutex);

    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);
        close(clisockfd);
        return NULL;
    }

    clients[client_count].socket = clisockfd;
    strcpy(clients[client_count].name, name);
    client_count++;

    pthread_mutex_unlock(&clients_mutex);

	char color[32];
    snprintf(color, sizeof(color), "\033[38;2;%d;%d;%dm",
        rand() % 256, rand() % 256, rand() % 256);

	{
	char message[516];

    snprintf(message, sizeof(message), "%s%s(%s) has joined the chat.%s", color, name, inet_ntoa(ip), RESETRGB);

	broadcast(message, clisockfd);
	}

    while (1) {
        nrcv = recv(clisockfd, buffer, sizeof(buffer) - 1, 0);

        if (nrcv <= 0)
            break;

        buffer[nrcv] = '\0';

        char message[600];

        snprintf(message, sizeof(message), "%s[%s(%s)]: %s%s", color, name, inet_ntoa(ip), buffer, RESETRGB);

        broadcast(message, clisockfd);
    }

	{
	char message[516];

    snprintf(message, sizeof(message), "%s%s(%s) has left the chat.%s", color, name, inet_ntoa(ip), RESETRGB);

	broadcast(message, clisockfd);
	}

    remove_client(clisockfd);

    close(clisockfd);

    return NULL;
}

int main(int argc, char *argv[])
{
	srand(time(NULL));

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) error("ERROR opening socket");

	struct sockaddr_in serv_addr;
	socklen_t slen = sizeof(serv_addr);
	memset((char*) &serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(PORT_NUM);

	int status = bind(sockfd,
			(struct sockaddr*) &serv_addr, slen);
	if (status < 0) error("ERROR on binding");

	listen(sockfd, 5);

	while(1) {
		struct sockaddr_in cli_addr;
		socklen_t clen = sizeof(cli_addr);
		int newsockfd = accept(sockfd,
			(struct sockaddr *) &cli_addr, &clen);
		if (newsockfd < 0) error("ERROR on accept");

		// prepare ThreadArgs structure to pass client socket
		ThreadArgs* args = (ThreadArgs*) malloc(sizeof(ThreadArgs));
		if (args == NULL) error("ERROR creating thread argument");

		args->clisockfd = newsockfd;
		args->ip = cli_addr.sin_addr;

		pthread_t tid;
		if (pthread_create(&tid, NULL, thread_main, (void*) args) != 0) error("ERROR creating a new thread");
	}

	return 0;
}

