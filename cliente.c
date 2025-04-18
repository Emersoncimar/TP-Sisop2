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
#include <errno.h>
#include <sys/time.h>

#define TIMEOUT_MS 10

//Variáveis globais
struct sockaddr_in server_addr;
int server_found = 0;
int sockfd;
uint32_t current_seq = 1;

void print_timestamp() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S ", tm_info);
    printf("%s", buffer);
}


//SUBSERVIÇO DE DESCOBERTA
void discover_server(int port) {
    struct sockaddr_in broadcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_BROADCAST
    };
    
    packet pkt;
    pkt.type = DESC;
    pkt.seqn = 0;
    
    // Envia pacote de descoberta
    sendto(sockfd, &pkt, sizeof(pkt), 0,
           (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    
    // Configura timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Espera resposta
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    
    while (!server_found) {
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                            (struct sockaddr*)&from, &from_len);
        
        if (n > 0 && pkt.type == DESC_ACK) {
            server_addr = from;
            server_found = 1;
            
            // Exibe mensagem de descoberta
            print_timestamp();
            printf("server_addr %s\n", inet_ntoa(from.sin_addr));
        }
    }
    
    // Remove timeout
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

//SUBSERVIÇO DE PROCESSAMENTO
void send_request(uint32_t value) {
    packet pkt;
    pkt.type = REQ;
    pkt.seqn = current_seq;
    pkt.payload.req.value = value;
    
    // Envia requisição
    sendto(sockfd, &pkt, sizeof(pkt), 0,
           (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    // Configura timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Espera resposta
    packet ack_pkt;
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    
    while (1) {
        ssize_t n = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0,
                            (struct sockaddr*)&from, &from_len);
        
        if (n > 0 && ack_pkt.type == REQ_ACK && 
            ack_pkt.payload.ack.seqn == current_seq) {
            // Resposta recebida
            print_timestamp();
            printf("server %s id_req %u value %u num_reqs %u total_sum %lu\n",
                   inet_ntoa(server_addr.sin_addr),
                   current_seq, value,
                   ack_pkt.payload.ack.num_reqs,
                   ack_pkt.payload.ack.total_sum);
            
            current_seq++;
            break;
        } else if (n < 0 && errno == EAGAIN) {
            // Timeout - reenvia
            sendto(sockfd, &pkt, sizeof(pkt), 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));
        }
    }
    
    // Remove timeout
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

//SUBSERVIÇO DE INTERFACE
void* input_thread(void *arg) {
    char buffer[256];
    
    while (fgets(buffer, sizeof(buffer), stdin)) {
        if (!server_found) {
            fprintf(stderr, "Servidor não encontrado ainda\n");
            continue;
        }
        
        uint32_t value;
        if (sscanf(buffer, "%u", &value) == 1) {
            send_request(value);
        }
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int port = atoi(argv[1]);
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    
    // Habilita broadcast
    int broadcast = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // Descobre servidor
    discover_server(port);
    
    // Inicia thread de entrada
    pthread_t thread;
    pthread_create(&thread, NULL, input_thread, NULL);
    
    // Loop para receber mensagens (se necessário)
    while (1) {
        // O cliente principal pode ficar ocioso ou tratar outras mensagens
        sleep(1);
    }
    
    close(sockfd);
    return 0;
}