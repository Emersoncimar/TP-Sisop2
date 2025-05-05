#ifndef HEADER_H
#define HEADER_H

#include <stdint.h>
//Para utilizar estruturas com endereços de redes
#include <netinet/in.h>

//Identificando tipos de pacotes
#define DESC 1      //Descoberta
#define DESC_ACK 2  //ACK Descoberta
#define REQ 3       //Requisição
#define REQ_ACK 4   //ACK Requisião

//Valor da requisicao(payload de REQ contendo o valor a ser somado)
struct requisicao {
    uint32_t value;
};

//Resposta do servidor a uma requisição 
struct requisicao_ack {
    uint32_t seqn;      //Numero de sequendia
    uint32_t num_reqs;  //Quantidade de requisiçoes
    uint64_t total_sum; //Soma total dos valores
};

//Formato completo de um pacote que sera enviado na rede
typedef struct __packet {
    uint16_t type;      //Tipo de pacote
    uint32_t seqn;      //Numero sequencia
    union {
        struct requisicao req;
        struct requisicao_ack ack;
    } payload;
} packet;

//Armazenando informações especificas de cada cliente
typedef struct {
    struct in_addr address;
    uint32_t last_req;
    uint64_t last_sum;
} client_info;

//Estrutura de LOGS PARA AS SAIDAS DA TELA atraves de um buffer
typedef struct {
    char message[256];
} log_entry;

//Estrutura para passar PACOTE e ENDEREÇO do cliente para o pthread_create
typedef struct request_data {
    int sockfd;
    packet pkt;
    struct sockaddr_in client_addr;
} request_data;

//Timestamp no formato string
void timestamp_to_str(char *buffer, size_t size);
//Adicionar os logs no buffer
void add_log_entry(const char *message);

#endif
