#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>

#define PORT 8888
#define BUFFER_SIZE 4096 // Aumentado para throttling mais eficiente
#define MAX_PATH 1024
#define MAX_QOS_RULES 100
#define MAX_CLIENTS 100
#define DEFAULT_RATE_KBPS 1000

// --- Estruturas de Dados Globais ---

// Regra de QoS: mapeia um IP a uma taxa máxima em kbps
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int rate_kbps;
} QoS_Rule;

// Estado de um cliente para cálculo de RTT
typedef struct {
    char ip[INET_ADDRSTRLEN];
    struct timeval html_req_time;
    int active_connections; // Para gerenciar banda compartilhada
} Client_State;

// --- Variáveis Globais e Mutexes ---

QoS_Rule g_qos_rules[MAX_QOS_RULES];
int g_num_qos_rules = 0;

Client_State g_client_states[MAX_CLIENTS];
int g_num_client_states = 0;

long g_max_server_throughput_kbps = 0;
long g_current_allocated_kbps = 0;

pthread_mutex_t g_qos_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_throughput_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Funções Auxiliares ---

void load_qos_config(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        perror("Não foi possível abrir o arquivo de QoS. Usando apenas a taxa padrão.");
        return;
    }
    while (g_num_qos_rules < MAX_QOS_RULES &&
           fscanf(file, "%s %d", g_qos_rules[g_num_qos_rules].ip, &g_qos_rules[g_num_qos_rules].rate_kbps) == 2) {
        g_num_qos_rules++;
    }
    fclose(file);
    printf("Carregadas %d regras de QoS do arquivo %s.\n", g_num_qos_rules, filename);
}

int get_rate_for_ip(const char* ip) {
    for (int i = 0; i < g_num_qos_rules; i++) {
        if (strcmp(ip, g_qos_rules[i].ip) == 0) {
            return g_qos_rules[i].rate_kbps;
        }
    }
    return DEFAULT_RATE_KBPS; // Taxa padrão
}

const char* get_mime_type(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".bin") == 0) return "application/octet-stream";
    return "application/octet-stream";
}

// --- Função Principal da Thread do Cliente ---

void* handle_client(void* arg) {
    int client_sock = ((int*)arg)[0];
    char client_ip[INET_ADDRSTRLEN];
    strcpy(client_ip, (char*)(arg + sizeof(int)));
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t read_size;

    while ((read_size = recv(client_sock, buffer, BUFFER_SIZE, 0)) > 0) {
        buffer[read_size] = '\0';
        
        char method[16], path[MAX_PATH], version[16];
        sscanf(buffer, "%s %s %s", method, path, version);

        char filepath[MAX_PATH];
        if (strcmp(path, "/") == 0) {
            strcpy(filepath, "index.html");
        } else {
            strcpy(filepath, path + 1);
        }

        // --- Lógica de RTT e Display Visual ---
        pthread_mutex_lock(&g_qos_mutex);
        int client_idx = -1;
        for (int i = 0; i < g_num_client_states; i++) {
            if (strcmp(client_ip, g_client_states[i].ip) == 0) {
                client_idx = i;
                break;
            }
        }
        
        if (strstr(filepath, ".html")) {
            if (client_idx != -1) {
                gettimeofday(&g_client_states[client_idx].html_req_time, NULL);
            }
        } else { // É um objeto
            if (client_idx != -1 && g_client_states[client_idx].html_req_time.tv_sec != 0) {
                struct timeval now, diff;
                gettimeofday(&now, NULL);
                timersub(&now, &g_client_states[client_idx].html_req_time, &diff);
                double rtt_ms = (diff.tv_sec * 1000.0) + (diff.tv_usec / 1000.0);
                
                printf("\n--- STATUS CLIENTE ---\n");
                printf("IP: %s | RTT Estimado: %.2f ms\n", client_ip, rtt_ms);
                printf("------------------------\n\n");

                g_client_states[client_idx].html_req_time.tv_sec = 0;
            }
        }
        pthread_mutex_unlock(&g_qos_mutex);
        // --- Fim da Lógica de RTT ---

        int file_fd = open(filepath, O_RDONLY);
        if (file_fd == -1) {
            char response_404[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            send(client_sock, response_404, strlen(response_404), 0);
            break;
        }

        struct stat file_stat;
        fstat(file_fd, &file_stat);
        off_t file_size = file_stat.st_size;

        char response_header[BUFFER_SIZE];
        sprintf(response_header,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Connection: keep-alive\r\n\r\n",
                get_mime_type(filepath), file_size);
        send(client_sock, response_header, strlen(response_header), 0);

        // --- Lógica de Controle de Taxa (Throttling) ---
        if (!strstr(filepath, ".html")) {
            int rate_kbps = get_rate_for_ip(client_ip);
            long bytes_per_sec = (rate_kbps * 1000) / 8;
            long chunk_size = BUFFER_SIZE;
            
            long usec_per_chunk = (long)((double)chunk_size / bytes_per_sec * 1000000.0);
            
            char file_buffer[BUFFER_SIZE];
            ssize_t bytes_read;
            while ((bytes_read = read(file_fd, file_buffer, chunk_size)) > 0) {
                send(client_sock, file_buffer, bytes_read, 0);
                usleep(usec_per_chunk);
            }
        } else { // Envia HTML sem controle de taxa
            char file_buffer[BUFFER_SIZE];
            ssize_t bytes_read;
            while ((bytes_read = read(file_fd, file_buffer, BUFFER_SIZE)) > 0) {
                send(client_sock, file_buffer, bytes_read, 0);
            }
        }
        close(file_fd);
    }

    // --- Lógica de Desalocação de Banda ---
    pthread_mutex_lock(&g_throughput_mutex);
    pthread_mutex_lock(&g_qos_mutex);

    for (int i = 0; i < g_num_client_states; i++) {
        if (strcmp(client_ip, g_client_states[i].ip) == 0) {
            g_client_states[i].active_connections--;
            if (g_client_states[i].active_connections == 0) {
                int rate_kbps = get_rate_for_ip(client_ip);
                g_current_allocated_kbps -= rate_kbps;
                printf("Recurso liberado para IP %s. Vazão alocada atual: %ld/%ld kbps\n", 
                       client_ip, g_current_allocated_kbps, g_max_server_throughput_kbps);
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_qos_mutex);
    pthread_mutex_unlock(&g_throughput_mutex);
    
    close(client_sock);
    return NULL;
}

// --- Função Principal do Servidor (main) ---

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <vazao_maxima_servidor_kbps>\n", argv[0]);
        return 1;
    }
    g_max_server_throughput_kbps = atol(argv[1]);

    load_qos_config("qos.conf");

    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Criação do Socket
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("Não foi possível criar o socket");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Bind do socket à porta e endereço
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind falhou");
        close(server_sock);
        return 1;
    }

    // Coloca o socket em modo de escuta
    if (listen(server_sock, 10) < 0) {
        perror("Listen falhou");
        close(server_sock);
        return 1;
    }
    
    printf("Servidor HTTP com QoS iniciado na porta %d. Vazão máxima: %ld kbps\n", 
           PORT, g_max_server_throughput_kbps);

    // Loop principal para aceitar conexões
    while ((client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len))) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // --- Lógica de Controle de Admissão ---
        int client_rate_kbps = get_rate_for_ip(client_ip);
        int is_new_ip = 1;

        pthread_mutex_lock(&g_throughput_mutex);
        pthread_mutex_lock(&g_qos_mutex);

        int client_idx = -1;
        for (int i = 0; i < g_num_client_states; i++) {
            if (strcmp(client_ip, g_client_states[i].ip) == 0) {
                is_new_ip = 0;
                client_idx = i;
                break;
            }
        }
        
        int admit = 0;
        if (is_new_ip) {
            if (g_current_allocated_kbps + client_rate_kbps <= g_max_server_throughput_kbps) {
                g_current_allocated_kbps += client_rate_kbps;
                if (g_num_client_states < MAX_CLIENTS) {
                    strcpy(g_client_states[g_num_client_states].ip, client_ip);
                    g_client_states[g_num_client_states].active_connections = 1;
                    g_client_states[g_num_client_states].html_req_time.tv_sec = 0;
                    g_num_client_states++;
                }
                admit = 1;
            }
        } else {
            g_client_states[client_idx].active_connections++;
            admit = 1;
        }
        
        pthread_mutex_unlock(&g_qos_mutex);
        pthread_mutex_unlock(&g_throughput_mutex);

        if (admit) {
            printf("Cliente %s admitido. Vazão alocada atual: %ld/%ld kbps\n", 
                   client_ip, g_current_allocated_kbps, g_max_server_throughput_kbps);

            pthread_t client_thread;
            void* arg = malloc(sizeof(int) + INET_ADDRSTRLEN);
            memcpy(arg, &client_sock, sizeof(int));
            strcpy(arg + sizeof(int), client_ip);
            
            if (pthread_create(&client_thread, NULL, handle_client, arg) < 0) {
                perror("Não foi possível criar a thread");
                free(arg);
            } else {
                pthread_detach(client_thread);
            }
        } else {
            printf("Cliente %s recusado. Capacidade do servidor excedida.\n", client_ip);
            char response_503[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
            send(client_sock, response_503, strlen(response_503), 0);
            close(client_sock);
        }
    }

    // Tratamento de erro para a falha do accept()
    if (client_sock < 0) {
        perror("Accept falhou");
        close(server_sock);
        return 1;
    }

    close(server_sock);
    return 0;
}
