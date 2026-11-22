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

void error(const char *msg)
{
	perror(msg);
	exit(0);
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
    if (argc < 2)
        error("Please specify hostname");

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

    if (pthread_create(&receiver, NULL, receive_messages, &sockfd) != 0){}
        error("ERROR creating receiver thread");

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