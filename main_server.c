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

// Information passed to each client thread
typedef struct _ThreadArgs {
    int clisockfd;
    struct in_addr ip;
    int room; // Room assigned to this client
} ThreadArgs;

#define MAX_CLIENTS 100
#define MAX_ROOMS 100

typedef struct {
    int socket;
    char name[256];
    char ip[32];

    // Stores the color of the client in RGB format
    int red;
    int green;
    int blue;
} Client;

Client clients[MAX_ROOMS][MAX_CLIENTS];
int client_count[MAX_ROOMS];

// current number of rooms created
int numRooms = 0;

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

int createRoom(){
    pthread_mutex_lock(&clients_mutex);

    if(numRooms >= MAX_ROOMS){
        pthread_mutex_unlock(&clients_mutex);
        return -1; // Cannot create more rooms
    }

    int room = numRooms;
    client_count[room] = 0;
    numRooms++;
    pthread_mutex_unlock(&clients_mutex);
    return room;
}

void broadcast(char *message, int sender_socket, int room)
{
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < client_count[room]; i++) {
        if (clients[room][i].socket != sender_socket) {
            send(clients[room][i].socket, message, strlen(message), 0);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

void printCLList()
{
    pthread_mutex_lock(&clients_mutex);

    printf("\n===== Connected Clients =====\n");

    for (int room = 0; room < numRooms; room++) {

        printf("Room: %d\n", room);

        if (client_count[room] == 0) {
            printf("No connected clients.\n");
        }

        for (int cl = 0; cl < client_count[room]; cl++) {

            printf("[%s(%s)]\n",
                clients[room][cl].name,
                clients[room][cl].ip);
        }
    }

    printf("==============================\n");

    pthread_mutex_unlock(&clients_mutex);
}


void remove_client(int socket, int room)
{
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < client_count[room]; i++) {

        if (clients[room][i].socket == socket) {

            for (int j = i; j < client_count[room] - 1; j++) {
                clients[room][j] = clients[room][j + 1];
            }

            client_count[room]--;
            break;
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}


void* thread_main(void* args)
{
    pthread_detach(pthread_self());

    int clisockfd = ((ThreadArgs*) args)->clisockfd;
    struct in_addr ip = ((ThreadArgs*) args)->ip;

    // Get the room assigned to this client
    int room = ((ThreadArgs*) args)->room;

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

    if (client_count[room] >= MAX_CLIENTS) {
        pthread_mutex_unlock(&clients_mutex);
        close(clisockfd);
        return NULL;
    }

    // Position where the new client will be stored
    int client_index = client_count[room];

    clients[room][client_index].socket = clisockfd;

    strcpy(clients[room][client_index].name, name);

    inet_ntop(AF_INET, &ip,
        clients[room][client_index].ip,
        sizeof(clients[room][client_index].ip));

    // Generate a unique color
    int red;
    int green;
    int blue;

    int uniqueColor;

    do {

        red = rand() % 256;
        green = rand() % 256;
        blue = rand() % 256;

        uniqueColor = 1;

        // Check the colors of existing clients in this room
        for (int cl = 0; cl < client_count[room]; cl++) {

            if (clients[room][cl].red == red &&
                clients[room][cl].green == green &&
                clients[room][cl].blue == blue) {

                uniqueColor = 0;
                break;
            }
        }

    } while (uniqueColor == 0);

    // Store the assigned color
    clients[room][client_index].red = red;
    clients[room][client_index].green = green;
    clients[room][client_index].blue = blue;

    client_count[room]++;

    pthread_mutex_unlock(&clients_mutex);

    printCLList();

    char color[32];

    snprintf(color, sizeof(color),
        "\033[38;2;%d;%d;%dm",
        red, green, blue);

    char message[516];

    snprintf(message, sizeof(message),
        "%s%s(%s) has joined the chat.%s",
        color, name, inet_ntoa(ip), RESETRGB);

    broadcast(message, clisockfd, room);


    while (1) {

        nrcv = recv(clisockfd, buffer, sizeof(buffer) - 1, 0);

        if (nrcv <= 0)
            break;

        buffer[nrcv] = '\0';

        char message[600];

        snprintf(message, sizeof(message),
            "%s[%s(%s)]: %s%s",
            color, name, inet_ntoa(ip), buffer, RESETRGB);

        broadcast(message, clisockfd, room);
    }


    {
        char message[516];

        snprintf(message, sizeof(message),
            "%s%s(%s) has left the chat.%s",
            color, name, inet_ntoa(ip), RESETRGB);

        broadcast(message, clisockfd, room);
    }

    remove_client(clisockfd, room);

    printCLList();

    close(clisockfd);

    return NULL;
}


int main(int argc, char *argv[])
{
    srand(time(NULL));

    memset(client_count, 0, sizeof(int) * MAX_ROOMS);

    int firstRoom = createRoom();

    if (firstRoom == -1) {
        error("ERROR creating the first room");
    }

    printf("Created room %d\n", firstRoom);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
        error("ERROR opening socket");

    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);

    memset((char*) &serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT_NUM);

    int status = bind(sockfd,
        (struct sockaddr*) &serv_addr,
        slen);

    if (status < 0)
        error("ERROR on binding");

    listen(sockfd, 5);

    while (1) {

        struct sockaddr_in cli_addr;
        socklen_t clen = sizeof(cli_addr);

        int newsockfd = accept(sockfd,
            (struct sockaddr*) &cli_addr,
            &clen);

        if (newsockfd < 0)
            error("ERROR on accept");

        // Prepare ThreadArgs structure to pass client information
        ThreadArgs* args = (ThreadArgs*) malloc(sizeof(ThreadArgs));

        if (args == NULL)
            error("ERROR creating thread argument");

        args->clisockfd = newsockfd;
        args->ip = cli_addr.sin_addr;

        // Temporary: all clients still join room 0
        args->room = firstRoom;

        pthread_t tid;

        if (pthread_create(&tid, NULL, thread_main, (void*) args) != 0)
            error("ERROR creating a new thread");
    }

    return 0;
}
