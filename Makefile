All:
	gcc -o main_client main_client.c -lpthread
	gcc -o main_client_ex main_client_ex.c -lpthread -lssh
	gcc -o main_server main_server.c -lpthread

clean:
	rm main_client main_server main_client_ex