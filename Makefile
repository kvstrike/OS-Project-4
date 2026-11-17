All:
	gcc -o main_client main_client.c -lpthread
	gcc -o main_server main_server.c -lpthread
	gcc -o main_client-ryan main_client-ryan.c -lpthread
	gcc -o main_server-ryan main_server-ryan.c -lpthread

clean:
	rm main_client main_server main_client-ryan main_server-ryan