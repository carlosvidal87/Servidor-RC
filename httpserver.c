#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>

#define PORT 8888
#define BUFFER_SIZE 2048
#define MAX_PATH 1024

// Função auxiliar para obter o tipo MIME com base na extensão do arquivo
const char* get_mime_type(const char* filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    return "application/octet-stream";
}

// Função executada por cada thread para tratar um cliente
void* handle_client(void* socket_desc) {
    int client_sock = *(int*)socket_desc;
    free(socket_desc); // Libera a memória alocada para o descritor

    char buffer[BUFFER_SIZE];
    ssize_t read_size;

    // Loop para tratar múltiplas requisições (HTTP 1.1 Keep-Alive)
    while ((read_size = recv(client_sock, buffer, BUFFER_SIZE, 0)) > 0) {
        buffer[read_size] = '\0';
        printf("--- Requisição Recebida ---\n%s---------------------------\n", buffer);

        char method[16], path[MAX_PATH];
        sscanf(buffer, "%s %s", method, path);

        // O navegador pode pedir a raiz "/", então servimos o index.html por padrão
        char filepath[MAX_PATH];
        if (strcmp(path, "/") == 0) {
            strcpy(filepath, "index.html");
        } else {
            // Remove a barra inicial do caminho
            strcpy(filepath, path + 1);
        }

        int file_fd = open(filepath, O_RDONLY);
        if (file_fd == -1) {
            // Arquivo não encontrado, envia resposta 404
            char response_404[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html><body><h1>404 Not Found</h1></body></html>";
            send(client_sock, response_404, strlen(response_404), 0);
            printf("Resposta: 404 Not Found para %s\n\n", filepath);
            break; // Fecha a conexão em caso de erro
        }

        // Obtém o tamanho do arquivo para o cabeçalho Content-Length
        struct stat file_stat;
        fstat(file_fd, &file_stat);
        off_t file_size = file_stat.st_size;

        // Monta o cabeçalho de resposta 200 OK
        char response_header[BUFFER_SIZE];
        sprintf(response_header,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Connection: keep-alive\r\n\r\n",
                get_mime_type(filepath), file_size);

        // Envia o cabeçalho
        send(client_sock, response_header, strlen(response_header), 0);
        printf("Resposta: 200 OK para %s (Tamanho: %ld bytes)\n\n", filepath, file_size);

        // Envia o conteúdo do arquivo em blocos
        char file_buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(file_fd, file_buffer, BUFFER_SIZE)) > 0) {
            send(client_sock, file_buffer, bytes_read, 0);
        }

        close(file_fd);
    }

    if (read_size == 0) {
        printf("Cliente desconectado (socket %d).\n\n", client_sock);
    } else if (read_size == -1) {
        perror("recv failed");
    }

    close(client_sock);
    return NULL;
}

int main() {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("Não foi possível criar o socket");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind falhou");
        return 1;
    }

    listen(server_sock, 10); // Fila de até 10 conexões pendentes

    printf("Servidor HTTP iniciado na porta %d. Aguardando conexões...\n", PORT);

    while ((client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len))) {
        printf("Conexão aceita. Criando thread para o cliente.\n");

        pthread_t client_thread;
        int* new_sock = malloc(sizeof(int));
        *new_sock = client_sock;

        if (pthread_create(&client_thread, NULL, handle_client, (void*)new_sock) < 0) {
            perror("Não foi possível criar a thread");
            free(new_sock); // Libera memória em caso de falha
            continue;
        }

        // Desacopla a thread para que seus recursos sejam liberados automaticamente ao terminar
        pthread_detach(client_thread);
    }

    if (client_sock < 0) {
        perror("Accept falhou");
        return 1;
    }

    close(server_sock);
    return 0;
}
