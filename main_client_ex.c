#include <openssl/core.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <string.h>
#include <libssh/libssh.h>

#define PORT_NUM 8083
#define SSH_PORT 22
#define MAX_ROOMS 100

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

int sending = 0;

static char name[64];

void error(const char *msg)
{
	perror(msg);
	exit(0);
}

void ssh_error_exit(ssh_session session, const char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, ssh_get_error(session));
	ssh_disconnect(session);
	ssh_free(session);
	exit(1);
}

// Check the server's host key against ~/.ssh/known_hosts
int verify_knownhost(ssh_session session)
{
	ssh_key srv_pubkey = NULL;
	unsigned char *hash = NULL;
	size_t hlen;

	if (ssh_get_server_publickey(session, &srv_pubkey) < 0) return -1;
	int rc = ssh_get_publickey_hash(srv_pubkey,
					SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
	ssh_key_free(srv_pubkey);
	if (rc < 0) return -1;

	enum ssh_known_hosts_e state = ssh_session_is_known_server(session);
	switch (state) {
	case SSH_KNOWN_HOSTS_OK:
			break;
	case SSH_KNOWN_HOSTS_CHANGED:
	case SSH_KNOWN_HOSTS_OTHER:
			fprintf(stderr, "WARNING: host key for server changed!\n");
			ssh_print_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
			ssh_clean_pubkey_hash(&hash);
			return -1;
	case SSH_KNOWN_HOSTS_NOT_FOUND:
	case SSH_KNOWN_HOSTS_UNKNOWN: {
			fprintf(stderr, "The server is unknown. Fingerprint:\n");
			ssh_print_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
			fprintf(stderr, "Trust this host? (yes/no): ");
			char ans[16];
			if (fgets(ans, sizeof(ans), stdin) == NULL ||
							strncmp(ans, "yes", 3) != 0) {
					ssh_clean_pubkey_hash(&hash);
					return -1;
			}
			if (ssh_session_update_known_hosts(session) < 0) {
					fprintf(stderr, "Could not update known_hosts: %s\n",
									ssh_get_error(session));
			}
			break;
	}
	case SSH_KNOWN_HOSTS_ERROR:
			fprintf(stderr, "Error: %s\n", ssh_get_error(session));
			ssh_clean_pubkey_hash(&hash);
			return -1;
	}
	ssh_clean_pubkey_hash(&hash);
	return 0;
}

int authenticate(ssh_session session)
{
	int rc = ssh_userauth_publickey_auto(session, NULL, NULL);
	if (rc == SSH_AUTH_SUCCESS) return 0;

	char prompt[] = "Password: ";
	char password[128];
	if (ssh_getpass(prompt, password, sizeof(password), 0, 0) < 0)
			return -1;
	rc = ssh_userauth_password(session, NULL, password);
	memset(password, 0, sizeof(password));
	return rc == SSH_AUTH_SUCCESS ? 0 : -1;
}


int send_all(ssh_channel channel, const char *message)
{
    size_t total_sent = 0;
    size_t message_length = strlen(message);

    while (total_sent < message_length) {
        int n = ssh_channel_write(
            channel,
            message + total_sent,
            message_length - total_sent
        );

        if (n <= 0)
            return -1;

        total_sent += n;
    }

    return 0;
}

// Function to receive a line of text from the socket
int recv_line(ssh_channel channel, char *buffer, int max_length)
{
    int position = 0;
    char character;

    while (position < max_length - 1) {
        int n = ssh_channel_read(channel, &character, 1, 0);

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

// Function to create a new room on the server
int choose_room(ssh_channel channel)
{
    char buffer[256];

    int room_numbers[MAX_ROOMS];
    int room_counts[MAX_ROOMS];
    int number_of_rooms = 0;

    if (send_all(channel, "LIST\n") < 0)
        error("ERROR requesting room list");

    int n = recv_line(channel, buffer, sizeof(buffer));

    if (n < 0)
        error("ERROR receiving room list");

    int server_room_count;

    if (sscanf(buffer, "ROOMS %d", &server_room_count) != 1) {
        fprintf(stderr, "Invalid room list from server: %s\n", buffer);
        return -1;
    }

    printf("\nAvailable rooms:\n");

    while (1) {
        n = recv_line(channel, buffer, sizeof(buffer));

        if (n < 0)
            error("ERROR receiving room information");

        if (n == 0)
            break;

        int room_number;
        int client_count;

        if (sscanf(buffer, "%d %d", &room_number, &client_count) == 2) {
            printf("Room %d - %d client(s)\n", room_number, client_count);

            if (number_of_rooms < MAX_ROOMS) {
                room_numbers[number_of_rooms] = room_number;
                room_counts[number_of_rooms] = client_count;
                number_of_rooms++;
            }
        }
    }

    if (number_of_rooms == 0) {
        fprintf(stderr, "No rooms are currently available.\n");
        return -1;
    }

    while (1) {
        printf("\nEnter the room number or type new to create a room: ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            return -1;

        buffer[strcspn(buffer, "\n")] = '\0';

        if (strcmp(buffer, "new") == 0)
            return -1;

        char *end_pointer;
        long selected_room = strtol(buffer, &end_pointer, 10);

        while (*end_pointer == ' ' || *end_pointer == '\t')
            end_pointer++;

        if (end_pointer == buffer || *end_pointer != '\0') {
            printf("Please enter a valid room number.\n");
            continue;
        }

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

int request_room(ssh_channel channel, int argc, char **argv)
{
    char request[256];
    char response[256];

    int selected_room = -1;

    if (argc == 2) {
        selected_room = choose_room(channel);

        if (selected_room == -1) {
            strcpy(request, "NEW\n");
            printf("Requesting a new room...\n");
        }
        else {
            snprintf(request, sizeof(request), "JOIN %d\n", selected_room);
        }
    }
    else if (strcmp(argv[2], "new") == 0) {
        strcpy(request, "NEW\n");
        printf("Requesting a new room...\n");
    }
    else {
        char *end_pointer;
        long room_number = strtol(argv[2], &end_pointer, 10);

        if (end_pointer == argv[2] || *end_pointer != '\0' ||
            room_number < 0 || room_number >= MAX_ROOMS) {

            fprintf(stderr, "Room argument must be 'new' or a room number.\n");
            exit(EXIT_FAILURE);
        }

        selected_room = (int)room_number;

        snprintf(request, sizeof(request), "JOIN %d\n", selected_room);
    }

    if (send_all(channel, request) < 0)
        error("ERROR sending room request");

    int n = recv_line(channel, response, sizeof(response));

    if (n < 0)
        error("ERROR receiving room confirmation");

    int confirmed_room;

    if (sscanf(response, "OK %d", &confirmed_room) != 1) {
        fprintf(stderr, "Server rejected room request: %s\n", response);
        exit(EXIT_FAILURE);
    }

    printf("Joined room %d.\n\n", confirmed_room);

    return confirmed_room;
}

int stream_file(ssh_channel channel, const char *filename, const char *header)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
        return -1;

    // Get file size
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    size_t header_len = header ? strlen(header) : 0;
    size_t total_size = (size_t)file_size + header_len;

    // Send total size followed by a space
    char size_buffer[32];
    int size_len = snprintf(size_buffer, sizeof(size_buffer), "%zu ", total_size);

    if (size_len < 0 || (size_t)size_len >= sizeof(size_buffer)) {
        fclose(file);
        return -1;
    }

    size_t sent = 0;

    while (sent < (size_t)size_len) {
        int n = ssh_channel_write(channel, size_buffer + sent,
                                  (uint32_t)((size_t)size_len - sent));
        if (n <= 0) {
            fclose(file);
            return -1;
        }
        sent += (size_t)n;
    }

    // Send header
    sent = 0;
    while (sent < header_len) {
        int n = ssh_channel_write(channel, header + sent,
                                  (uint32_t)(header_len - sent));
        if (n <= 0) {
            fclose(file);
            return -1;
        }
        sent += (size_t)n;
    }

    // Stream file contents
    char buffer[8192];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sent = 0;

        while (sent < bytes_read) {
            int n = ssh_channel_write(channel, buffer + sent,
                                      (uint32_t)(bytes_read - sent));
            if (n <= 0) {
                fclose(file);
                return -1;
            }
            sent += (size_t)n;
        }
    }

    int failed = ferror(file);
    fclose(file);

    return failed ? -1 : 0;
}

int save_file(const char *filename, const char *data, size_t file_size)
{
    FILE *file = fopen(filename, "wb");

    if (!file)
        return -1;

    size_t written = fwrite(data, 1, file_size, file);

    if (written != file_size || fclose(file) != 0)
        return -1;

    return 0;
}

char *receive_message(ssh_channel channel, size_t *message_size)
{
    char size_buffer[32];
    size_t i = 0;
    char c;

    // Read size prefix
    while (i < sizeof(size_buffer) - 1) {
        int n = ssh_channel_read(channel, &c, 1, 0);

        if (n <= 0)
            return NULL;

        if (c == ' ')
            break;

        if (c < '0' || c > '9')
            return NULL;

        size_buffer[i++] = c;
    }

    if (i == 0 || i == sizeof(size_buffer) - 1)
        return NULL;

    size_buffer[i] = '\0';

    char *end;
    unsigned long long parsed_size = strtoull(size_buffer, &end, 10);

    if (*end != '\0' || parsed_size > 100 * 1024 * 1024)
        return NULL;

    size_t size = (size_t)parsed_size;

    char *buffer = malloc(size + 1);
    if (!buffer)
        return NULL;

    size_t received = 0;

    while (received < size) {
        int n = ssh_channel_read(
            channel,
            buffer + received,
            (uint32_t)(size - received),
            0
        );

        if (n <= 0) {
            free(buffer);
            return NULL;
        }

        received += (size_t)n;
    }

    buffer[size] = '\0';
    *message_size = size;

    return buffer;
}

void* receive_messages(void* arg)
{
    ssh_channel channel = *(ssh_channel*)arg;

    while (1) {
		size_t message_size;
		char *buffer = receive_message(channel, &message_size);

		if (!buffer)
    		break;

		if (strstr(buffer, "] has DM'd you: [FILE] ") != NULL) {
			char *marker = strstr(buffer, "] has DM'd you: [FILE] ");
			if (!marker)
				continue;

			char *sender_start = strchr(strchr(buffer, '[') + 1, '[');

			if (!sender_start || sender_start >= marker)
				continue;

			size_t sender_len = (size_t)(marker - sender_start) + 1;
			char *sender = strndup(sender_start, sender_len);

			if (!sender)
				continue;

			char *filename_start = marker + strlen("] has DM'd you: [FILE] ");
			size_t remaining = message_size - (size_t)(filename_start - buffer);

			char *filename_end = memchr(filename_start, ' ', remaining);

			if (!filename_end) {
				free(sender);
				continue;
			}

			char *filename = strndup(filename_start,
									(size_t)(filename_end - filename_start));

			if (!filename) {
				free(sender);
				continue;
			}

			char *data_start = filename_end + 1;
			size_t header_size = (size_t)(data_start - buffer);

			if (header_size > message_size) {
				free(sender);
				free(filename);
				continue;
			}

			size_t file_size = message_size - header_size - 8;
			char *data = malloc(file_size ? file_size : 1);

			if (!data) {
				free(sender);
				free(filename);
				continue;
			}

			memcpy(data, data_start, file_size);

			printf("\nThis file transfer request is for you!\033[K\n");
			printf("Filename: %s\n", filename);
			printf("Sender: %s\n", sender);
			printf("Would you like to download it to the current path? (y/N): ");
			fflush(stdout);

			pthread_mutex_lock(&mutex);

			sending = 1;

			while (sending == 1)
				pthread_cond_wait(&cond, &mutex);

			if (sending == 2)
				save_file(filename, data, file_size);
			// else if (sending == 3)
			// 	pass;
			sending = 0;

			pthread_mutex_unlock(&mutex);

			free(sender);
			free(filename);
			free(data);
			continue;
		}
		else {
			printf("\r%s\033[K\n", buffer);
		}

		printf("Please enter the message: ");
		fflush(stdout);
	}

    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <server_address> [room_number|new]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Create SSH session
    ssh_session session = ssh_new();

    if (session == NULL)
        error("ERROR creating SSH session");

    // Configure SSH
    ssh_options_set(session, SSH_OPTIONS_HOST, argv[1]);

    int ssh_port = SSH_PORT;
    ssh_options_set(session, SSH_OPTIONS_PORT, &ssh_port);
    ssh_options_parse_config(session, NULL);

    // Connect, verify and authenticate
    if (ssh_connect(session) != SSH_OK)
        ssh_error_exit(session, "ERROR connecting");

    if (verify_knownhost(session) < 0)
        ssh_error_exit(session, "ERROR verifying host");

    if (authenticate(session) < 0)
        ssh_error_exit(session, "ERROR authenticating");

    // Create forwarded channel
    ssh_channel channel = ssh_channel_new(session);

    if (channel == NULL)
        ssh_error_exit(session, "ERROR creating channel");

    if (ssh_channel_open_forward(
            channel,
            "localhost",
            PORT_NUM,
            "localhost",
            0
        ) != SSH_OK) {
        ssh_channel_free(channel);
        ssh_error_exit(session, "ERROR opening forward channel");
    }

    // Request room
    request_room(channel, argc, argv);

    // Get username
    while (1) {
        printf("Please enter your name: ");
        fflush(stdout);

        memset(name, 0, sizeof(name));

        if (fgets(name, sizeof(name), stdin) == NULL)
            break;

        if (name[0] == '\n')
            continue;

        name[strcspn(name, "\n")] = '\0';

		for (int i = 0; name[i] != '\0'; i++) {
			if (name[i] == ' ')
				name[i] = '_';
		}

        if (send_all(channel, name) < 0)
            error("ERROR writing username");

        break;
    }

    // Start receiver thread
    pthread_t receiver;

    int thread_status = pthread_create(
        &receiver,
        NULL,
        receive_messages,
        &channel
    );

    if (thread_status != 0) {
        fprintf(stderr, "ERROR creating receiver thread: %s\n",
                strerror(thread_status));

        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);

        exit(EXIT_FAILURE);
    }

	char buffer[256];

    // Send messages
    while (1) {
        printf("Please enter the message: ");
        fflush(stdout);

        memset(buffer, 0, sizeof(buffer));

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
			if (sending) {
				sending = 3;
				continue;
			}
			break;
		}

        if (buffer[0] == '\n') {
			if (sending) {
				sending = 3;
				continue;
			}
            break;
		}
		buffer[strcspn(buffer, "\n")] = '\0';

		pthread_mutex_lock(&mutex);

		if (sending == 1) {
			if (strcmp(buffer, "y") == 0 || strcmp(buffer, "Y") == 0)
				sending = 2;
			else
				sending = 3;

			pthread_cond_signal(&cond);
    		pthread_mutex_unlock(&mutex);
			continue;
		}

		pthread_mutex_unlock(&mutex);

		if (strncmp(buffer, "[SEND] ", 7) == 0) {
			char recipient[64];
			char filename[64];

			strcpy(recipient, strtok(buffer + 7, " "));
			strcpy(filename, strtok(NULL, " "));

			printf("Sending %s to %s\n", filename, recipient);

			snprintf(buffer, sizeof(buffer),
					"[DM] %s [FILE] %s ",
					recipient, filename);


			stream_file(channel, filename, buffer);

			printf("File sent!\n");
			continue;
		}

		char finalSend[260];

		snprintf(finalSend, sizeof(finalSend), "%zu %s", strlen(buffer), buffer);

		if (send_all(channel, finalSend) < 0) {
			printf("ERROR sending message\n");
		}
    }

    // Disconnect
    ssh_channel_send_eof(channel);

    pthread_join(receiver, NULL);

    ssh_channel_close(channel);
    ssh_channel_free(channel);

    ssh_disconnect(session);
    ssh_free(session);

    return 0;
}