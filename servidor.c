#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

//Variáveis globais
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t log_cond = PTHREAD_COND_INITIALIZER;

//vetor com os clientes
client_info *clients = NULL;
int num_clients = 0;

//buffer do log
log_entry *log_buffer = NULL;
int log_count = 0;
int log_capacity = 0;

aggregate_sum global_sum = {0};

//Função para gerar timestamp
void timestamp_to_str(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S ", tm);
}

//Adicionar entrada de log no buffer 
void add_log_entry(const char *message) {
    pthread_mutex_lock(&log_mutex);
    
    //aumentando o tamalho do log conforme necessidade
    if (log_count >= log_capacity) {
        log_capacity = log_capacity ? log_capacity * 2 : 1;
        log_buffer = realloc(log_buffer, log_capacity * sizeof(log_entry));
    }
    //Copiando a mensagem para o buffer
    strncpy(log_buffer[log_count].message, message, sizeof(log_buffer[0].message));
    log_count++;
    //Dando o sinal que chegou uma nova mensagem
    pthread_cond_signal(&log_cond);
    
    pthread_mutex_unlock(&log_mutex);
}

//Subserviço de descoberta para o cliente que enviou o seu DESC -> DESC_ACK
void handle_discovery(int sockfd, struct sockaddr_in *client_addr) {
    packet pkt;
    pkt.type = DESC_ACK;
    pkt.seqn = 0;

    //Envia o pacote DESC_ACK para o cliente
    sendto(sockfd, &pkt, sizeof(pkt), 0, 
          (struct sockaddr*)client_addr, sizeof(*client_addr));

    //mutex lock para acessar a lista de clientes
    pthread_mutex_lock(&client_mutex);
    //Verificando se o IP do cliente ja esya na lista de clientes
    int found = 0;
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].address.s_addr == client_addr->sin_addr.s_addr) {
            found = 1;
            break;
        }
    }
    //Adicionando o cliente e inicializando-o
    if (!found) {
        clients = realloc(clients, (num_clients + 1) * sizeof(client_info));
        clients[num_clients].address = client_addr->sin_addr;
        clients[num_clients].last_req = 0;
        clients[num_clients].last_sum = 0;
        num_clients++;
    }

    pthread_mutex_unlock(&client_mutex);
}

//Subserviço de processamento que recebe os dados da requisição como um ponteiro para request_dat
void* process_request(void *arg) {
    request_data *data = (request_data*)arg;

    //inicializando informações do cliente
    uint32_t client_seq = data->pkt.seqn;
    uint32_t value = data->pkt.payload.req.value;
    struct in_addr client_ip = data->client_addr.sin_addr;

    //Busca cliente e se não estiver na tabela ignora a requisição
    pthread_mutex_lock(&client_mutex);
    client_info *client = NULL;
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].address.s_addr == client_ip.s_addr) {
            client = &clients[i];
            break;
        }
    }
    pthread_mutex_unlock(&client_mutex);

    if (!client) {
        free(data);
        return NULL;
    }

    //Verifica duplicata
    int is_duplicate = (client_seq <= client->last_req);

    //Atualiza os dados do cliente se não for duplicata
    if (!is_duplicate) {
        pthread_mutex_lock(&client_mutex);
        global_sum.num_reqs++;
        global_sum.total_sum += value;
        client->last_req = client_seq;
        client->last_sum = global_sum.total_sum;
        pthread_mutex_unlock(&client_mutex);
    }

    //Prepara e confirma a requisição 
    packet ack_pkt;
    ack_pkt.type = REQ_ACK;
    ack_pkt.seqn = client_seq;
    ack_pkt.payload.ack.seqn = client_seq;
    ack_pkt.payload.ack.num_reqs = global_sum.num_reqs;
    ack_pkt.payload.ack.total_sum = global_sum.total_sum;
    
    //Envia o SEND_ACK para o cliente
    sendto(data->sockfd, &ack_pkt, sizeof(ack_pkt), 0,
          (struct sockaddr*)&data->client_addr, sizeof(data->client_addr));

    // Prepara mensagem de log e envia para o buffer
    char timestamp[64];
    timestamp_to_str(timestamp, sizeof(timestamp));

    char log_msg[256];
    if (is_duplicate) {
        snprintf(log_msg, sizeof(log_msg), "%sclient %s DUP!! id_req %u value %u num_reqs %u total_sum %lu\n",
                timestamp, inet_ntoa(client_ip), client_seq, value,
                global_sum.num_reqs, global_sum.total_sum);
    } else {
        snprintf(log_msg, sizeof(log_msg), "%sclient %s id_req %u value %u num_reqs %u total_sum %lu\n",
                timestamp, inet_ntoa(client_ip), client_seq, value,
                global_sum.num_reqs, global_sum.total_sum);
    }

    add_log_entry(log_msg);
    free(data);
    return NULL;
}

// Subserviço de interface que fica aguardando os logs até ser sinalizada
void* interface_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&log_mutex);
        
        //Esperando uma mensagem para o log
        while (log_count == 0) {
            pthread_cond_wait(&log_cond, &log_mutex);
        }

        // Processa todas as mensagens
        for (int i = 0; i < log_count; i++) {
            printf("%s", log_buffer[i].message);
        }
        //Zera o contador de logs
        log_count = 0;
        pthread_mutex_unlock(&log_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    //Verifica se foi passado a porta de entrada
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    //Convertendo a porta para um int e criando o socket UDP via Ipv4
    int port = atoi(argv[1]);
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    //Configura o endereço para aceitar as conexões em qualquer IP da maquina
    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    //Associando o socket a porta IP local
    bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    //Habilita broadcast
    int broadcast = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    //Inicia thread de interface
    pthread_t iface_thread;
    pthread_create(&iface_thread, NULL, interface_thread, NULL);

    //Mensagem inicial
    char timestamp[64];
    timestamp_to_str(timestamp, sizeof(timestamp));
    printf("%snum_reqs 0 total_sum 0\n", timestamp);

    //Loop principal
    while (1) {
        //Armazenando o dado enviado e o endereço do cliente que enviou o pacote
        packet pkt;
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        //Recebendo o pacote UDP
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                            (struct sockaddr*)&client_addr, &addr_len);

        if (n < 0) {
            perror("recvfrom");
            continue;
        }

        //Identifica o tipo de pacote e destina-o
        switch (pkt.type) {
            case DESC:
                handle_discovery(sockfd, &client_addr);
                break;
                
            case REQ: {
                pthread_t thread;
                request_data *data = malloc(sizeof(request_data));

                data->sockfd = sockfd;
                data->pkt = pkt;
                data->client_addr = client_addr;
                
                pthread_create(&thread, NULL, process_request, data);
                pthread_detach(thread);
                break;
            }
                
            default:
                fprintf(stderr, "Pacote desconhecido: %d\n", pkt.type);
        }
    }

    close(sockfd);
    free(clients);
    free(log_buffer);
    return 0;
}