#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>

#define PORT_NUM 8083
#define MAX_ROOMS 100

void error(const char *msg)
{
	perror(msg);
	exit(0);
}

int send_all(int socket, const char *message){
    size_t total_sent = 0;
    size_t message_length = strlen(message);

    while(total_sent < message_length){
        int n = send(socket, message + total_sent, message_length - total_sent, 0);
        if(n <= 0){
            return -1; // Error occurred
        }
        total_sent += n;
    }
    return 0;
}
int recv_line(int sockfd, char *buffer, int max_length)
{
    int position = 0;
    char character;

    while (position < max_length - 1) {
        int n = recv(sockfd, &character, 1, 0);

        if (n <= 0) {
            if (position == 0)
                return -1;

            break;
        }

        if (character == '\n')
            break;

        if (character != '\r')
            buffer[position++] = character;
    }

    buffer[position] = '\0';

    return position;
}
int choose_room(int sockfd)
{
    char buffer[256];

    int room_numbers[MAX_ROOMS];
    int room_counts[MAX_ROOMS];
    int number_of_rooms = 0;

    // Ask the server for its room list.
    if (send_all(sockfd, "LIST\n") < 0)
        error("ERROR requesting room list");

    // Read the first line: ROOMS <number>
    int n = recv_line(sockfd, buffer, sizeof(buffer));

    if (n < 0)
        error("ERROR receiving room list");

    int server_room_count;

    if (sscanf(buffer, "ROOMS %d", &server_room_count) != 1) {
        fprintf(stderr, "Invalid room list from server: %s\n", buffer);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("\nAvailable rooms:\n");

    /*
        The server sends each room as:
        room_number client_count

        The list ends with an empty line.
    */
    while (1) {
        n = recv_line(sockfd, buffer, sizeof(buffer));

        if (n < 0)
            error("ERROR receiving room information");

        // Empty line means the room list is finished.
        if (n == 0)
            break;

        int room_number;
        int client_count;

        if (sscanf(buffer, "%d %d", &room_number, &client_count) == 2) {
            printf("Room %d - %d client(s)\n",
                   room_number,
                   client_count);

            if (number_of_rooms < MAX_ROOMS) {
                room_numbers[number_of_rooms] = room_number;
                room_counts[number_of_rooms] = client_count;
                number_of_rooms++;
            }
        }
    }

    if (number_of_rooms == 0) {
        fprintf(stderr, "No rooms are currently available.\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Ask the user which room they want to join.
    while (1) {
        printf("\nEnter the room number you want to join: ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        char *end_pointer;
        long selected_room = strtol(buffer, &end_pointer, 10);

        // Skip any whitespace after the number.
        while (*end_pointer == ' ' || *end_pointer == '\t' ||
               *end_pointer == '\n') {
            end_pointer++;
        }

        if (end_pointer == buffer || *end_pointer != '\0') {
            printf("Please enter a valid room number.\n");
            continue;
        }

        // Check that the room appeared in the server's list.
        int room_exists = 0;

        for (int i = 0; i < number_of_rooms; i++) {
            if (room_numbers[i] == selected_room) {
                room_exists = 1;
                break;
            }
        }

        if (!room_exists) {
            printf("That room is not in the room list.\n");
            continue;
        }

        return (int)selected_room;
    }
}
int request_room(int sockfd, int argc, char **argv)
{
    char request[256];
    char response[256];

    int selected_room = -1;

    if (argc == 2) {
        // No room argument: show available rooms.
        selected_room = choose_room(sockfd);

        snprintf(
            request,
            sizeof(request),
            "JOIN %d\n",
            selected_room
        );
    }
    else if (strcmp(argv[2], "new") == 0) {
        // Create a new room.
        strcpy(request, "NEW\n");

        printf("Requesting a new room...\n");
    }
    else {
        // The argument should be a room number.
        char *end_pointer;
        long room_number = strtol(argv[2], &end_pointer, 10);

        if (end_pointer == argv[2] || *end_pointer != '\0' || room_number < 0 || room_number >= MAX_ROOMS) {

            fprintf(stderr, "Room argument must be 'new' or a room number.\n");

            close(sockfd);
            exit(EXIT_FAILURE);
        }

        selected_room = (int)room_number;

        snprintf(
            request,
            sizeof(request),
            "JOIN %d\n",
            selected_room
        );
    }

    // Send NEW or JOIN request to the server.
    if (send_all(sockfd, request) < 0)
        error("ERROR sending room request");

    // Wait for the server's confirmation.
    int n = recv_line(sockfd, response, sizeof(response));

    if (n < 0)
        error("ERROR receiving room confirmation");

    int confirmed_room;

    if (sscanf(response, "OK %d", &confirmed_room) != 1) {
        fprintf(stderr, "Server rejected room request: %s\n", response);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Joined room %d.\n\n", confirmed_room);

    return confirmed_room;
}

void* receive_messages(void* arg)
{
    int sockfd = *(int*)arg;
    char buffer[256];

    while (1) {
        memset(buffer, 0, sizeof(buffer));

        int n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);

        if (n <= 0) {
            printf("\nDisconnected from server.\n");
            break;
        }

        buffer[n] = '\0';

        printf("\r%s\033[K\n", buffer);
        printf("Please enter the message: ");
        fflush(stdout);
    }

    return NULL;
}

int main(int argc, char **argv)
{
    if(argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <server_address> [room_number|new]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    struct hostent* server = gethostbyname(argv[1]);
    if (server == NULL)
        error("ERROR, no such host");

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;

    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    serv_addr.sin_port = htons(PORT_NUM);

    int status = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    if (status < 0)
        error("ERROR connecting");

    request_room(sockfd, argc, argv);
    char buffer[256];

    // Get username
    while (1) {
        printf("Please enter your name: ");
        fflush(stdout);

        memset(buffer, 0, sizeof(buffer));

        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            break;

        if (buffer[0] == '\n')
            continue;

		buffer[strcspn(buffer, "\n")] = '\0';

        int n = send(sockfd, buffer, strlen(buffer), 0);

        if (n <= 0)
            error("ERROR writing username");

        break;
    }

    // Start receiver thread
    pthread_t receiver;

    int thread_status = pthread_create(
        &receiver,
        NULL,
        receive_messages,
        &sockfd
    );

    if (thread_status != 0) {
        fprintf(stderr, "ERROR creating receiver thread: %s\n", strerror(thread_status));

        close(sockfd);
        exit(EXIT_FAILURE);
    }
    // Send messages
    while (1) {
        printf("Please enter the message: ");
        fflush(stdout);

        memset(buffer, 0, sizeof(buffer));

        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            break;

        // Empty message means disconnect
        if (buffer[0] == '\n')
            break;

		buffer[strcspn(buffer, "\n")] = '\0';

        int n = send(sockfd, buffer, strlen(buffer), 0);

        if (n <= 0) {
            printf("ERROR sending message\n");
            break;
        }
    }

    // Shut down connection
    shutdown(sockfd, SHUT_RDWR);

    // Wait for receiver thread
    pthread_join(receiver, NULL);

    close(sockfd);

    return 0;
}