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

//struct para informações do servidor
struct sockaddr_in server_addr;
//Flag indicando se o servidor foi encontrado
int server_found = 0;
int sockfd;
uint32_t current_seq = 1;

//Função de timestamp novamente
void print_timestamp() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S ", tm_info);
    printf("%s", buffer);
}


//Subserviço de descoberta
void discover_server(int port) {
    //Configurando o Broadcast
    struct sockaddr_in broadcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_BROADCAST
    };
    
    //Inicia o pacote DESC
    packet pkt;
    pkt.type = DESC;
    pkt.seqn = 0;
    
    //Envia pacote DESC
    sendto(sockfd, &pkt, sizeof(pkt), 0,
           (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    
    //Configura timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    //Esperando alguma resposta do servidor
    while (!server_found) {
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                            (struct sockaddr*)&from, &from_len);

        //Verificando se o pkt é do tipo DESC_ACK e guardando o IP do servidor
        if (n > 0 && pkt.type == DESC_ACK) {
            server_addr = from;
            server_found = 1;
            
            // Exibe mensagem de descoberta
            print_timestamp();
            printf("server_addr %s\n", inet_ntoa(from.sin_addr));
        }
    }
    
    //Remove timeout
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

//Subserviço de processamento
void send_request(uint32_t value) {
    packet pkt;
    pkt.type = REQ;
    pkt.seqn = current_seq;
    pkt.payload.req.value = value;
    
    //Envia requisição para o servidor
    sendto(sockfd, &pkt, sizeof(pkt), 0,
           (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    //timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    //Espera resposta do servidor
    packet ack_pkt;
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    
    while (1) {
        //recebe o pacote do servidor
        ssize_t n = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0,
                            (struct sockaddr*)&from, &from_len);
        
        //Verificando se um pkt REQ_ACK e se a sua sequencia é valida 
        if (n > 0 && ack_pkt.type == REQ_ACK && 
            ack_pkt.payload.ack.seqn == current_seq) {
            //Resposta recebida
            print_timestamp();
            printf("server %s id_req %u value %u num_reqs %u total_sum %lu\n",
                   inet_ntoa(server_addr.sin_addr),
                   current_seq, value,
                   ack_pkt.payload.ack.num_reqs,
                   ack_pkt.payload.ack.total_sum);
            
            current_seq++;
            break;
            //Se ocorre o timeout ele sai do loop e reenvia a REQ
        } else if (n < 0 && errno == EAGAIN) {
            sendto(sockfd, &pkt, sizeof(pkt), 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));
        }
    }
    
    //Remove timeout
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

//Subserviço de interface
void* input_thread(void *arg) {
    char buffer[256];
    
    while (fgets(buffer, sizeof(buffer), stdin)) {
        if (!server_found) {
            fprintf(stderr, "Servidor não encontrado\n");
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
        fprintf(stderr, "%s <porta>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int port = atoi(argv[1]);
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    
    //Habilitando broadcast
    int broadcast = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    //Descobrindo o servidor
    discover_server(port);
    
    //Inicia thread de entrada
    pthread_t thread;
    pthread_create(&thread, NULL, input_thread, NULL);
    //Join esperando a thread principal terminar
    pthread_join(thread, NULL);
    
    close(sockfd);
    return 0;
}