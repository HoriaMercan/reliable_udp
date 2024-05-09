#include <pthread.h>
#include <cstdlib>
#include <map>
#include <cstdint>
#include "lib.h"
#include "utils.h"
#include "protocol.h"
#include <poll.h>
#include <cassert>
#include <sys/timerfd.h>

#include <string.h>
#include <vector>
#include <set>

using namespace std;

std::map<int, struct connection *> cons;

struct pollfd data_fds[MAX_CONNECTIONS];
/* Used for timers per connection */
struct pollfd timer_fds[MAX_CONNECTIONS];
int fdmax = 0;

int window_size;

uint8_t *received_data_buffer[MAX_CONNECTIONS];
uint32_t max_buffer_size;
uint32_t buffer_size[MAX_CONNECTIONS];

uint32_t start_size[MAX_CONNECTIONS];

bool *acked_data[MAX_CONNECTIONS];

int udp_socket;


map<int, char *>ordered_segments;

int recv_data(int conn_id, char *buffer, int len)
{
    int size = 0;

    
    // while (buffer_size[conn_id] == 0);
    
    pthread_mutex_lock(&cons[conn_id]->con_lock);
    // DEBUG_PRINT("IN RECV DATA\n");
    /* We will write code here as to not have sync problems with recv_handler */
    size = std::min(len, int(buffer_size[conn_id] - start_size[conn_id]));
    if (size == 0) {
        pthread_mutex_unlock(&cons[conn_id]->con_lock);
        return 0;
    }

    memcpy(buffer, received_data_buffer[conn_id] + start_size[conn_id], size);
    // buffer_size[conn_id] -= size;
    start_size[conn_id] += size;

    
    if (buffer_size[conn_id] >= max_buffer_size - 2 * MAX_SEGMENT_SIZE) {
        memmove(received_data_buffer[conn_id], received_data_buffer[conn_id] + start_size[conn_id], buffer_size[conn_id]);
    }

    pthread_mutex_unlock(&cons[conn_id]->con_lock);
    
    return size;
}

int acked_by_now[MAX_CONNECTIONS];
int send_ack_again(connection *conn, int current_ack) {
    DEBUG_PRINT("SENT ACK AGAIN for: %d\n", current_ack);
    poli_tcp_ctrl_hdr ctrl_hdr;
    ctrl_hdr.protocol_id = POLI_PROTOCOL_ID;
    ctrl_hdr.ack_num = current_ack;
    ctrl_hdr.conn_id = conn->conn_id;
    ctrl_hdr.type = 4;

    // if (current_ack <= acked_by_now[conn->conn_id])
        ctrl_hdr.type = 7;

    // conn->servaddr.sin_port = htons(8033 + conn->conn_id);
    int rc = sendto(conn->sockfd, &ctrl_hdr, sizeof(ctrl_hdr),
                            0, (sockaddr *)&(conn->servaddr),
                            sizeof(conn->servaddr));

    if (rc < 0) {
        perror("ERROR ON SEND\n");
        exit(-1);
    }

    return rc;
}

void *receiver_handler(void *arg)
{

    char segment[MAX_SEGMENT_SIZE];
    int res;
    DEBUG_PRINT("Starting recviver handler\n");

    while (1) {
        int conn_id = -1;
        do {
            res = recv_message_or_timeout(segment, MAX_SEGMENT_SIZE, &conn_id);
        } while(res == -14);

        pthread_mutex_lock(&cons[conn_id]->con_lock);
        connection *conn = cons[conn_id];
        if (res > 0) {
        
            DEBUG_PRINT("Not timeout\t res = %d\n", res);
            poli_tcp_data_hdr data_hdr = *(poli_tcp_data_hdr *)segment;
            DEBUG_PRINT("Segment received: %d\t. Size: %d\t. Acked by now: %d\n", data_hdr.seq_num, data_hdr.len, acked_by_now[conn_id]);

            if (data_hdr.protocol_id != POLI_PROTOCOL_ID){
                pthread_mutex_unlock(&cons[conn_id]->con_lock);
                continue;
            }

            if (data_hdr.seq_num < acked_by_now[conn_id]) {
                send_ack_again(conn, acked_by_now[conn_id] - 1);
                pthread_mutex_unlock(&cons[conn_id]->con_lock);
                continue;
            }

            if (data_hdr.seq_num >= acked_by_now[conn_id] + conn->max_window_seq) {
                pthread_mutex_unlock(&cons[conn_id]->con_lock);
                continue;
            }

            if (acked_data[conn_id][data_hdr.seq_num - acked_by_now[conn_id]]) {
                // pthread_mutex_unlock(&cons[conn_id]->con_lock);
                // send_ack_again(conn, data_hdr.seq_num);
                pthread_mutex_unlock(&cons[conn_id]->con_lock);
                continue;

            }

            DEBUG_PRINT("toate ok, pachetul este corespunzator\n");
            
            
            // buffer_size[conn->conn_id] += data_hdr.len;
            char *received_segments = (char *)malloc(sizeof(char) * res);
            memcpy(received_segments, segment, res);

            ordered_segments.insert({int(data_hdr.seq_num), received_segments});
            
            acked_data[conn_id][data_hdr.seq_num - acked_by_now[conn_id]] = true;

            int cnt = 0;
            bool *curr_acked_data = acked_data[conn_id];
            while (cnt < conn->max_window_seq && curr_acked_data[cnt]) {
                //scrie in buffer
                auto curr_buff = ordered_segments[cnt + acked_by_now[conn_id]];
                memcpy(received_data_buffer[conn_id] + buffer_size[conn_id],
                            curr_buff + sizeof(poli_tcp_ctrl_hdr), ((poli_tcp_data_hdr *)curr_buff)->len);
                
                buffer_size[conn_id] += ((poli_tcp_data_hdr *)curr_buff)->len;
                free(curr_buff);
                ordered_segments.erase(cnt + acked_by_now[conn_id]);
                cnt++;
            }


            // update acks
            for (int i = cnt; i < window_size; i++) {
                curr_acked_data[i - cnt] = curr_acked_data[i];
                        
            }
            for (int i = window_size - cnt; i < window_size; i++)
                curr_acked_data[i] = false;

            acked_by_now[conn_id] += cnt;

            send_ack_again(conn, acked_by_now[conn_id] - 1); // aici trimit ack-ul prima data

        } else {
            DEBUG_PRINT("TIMEOUT\n");

            send_ack_again(conn, acked_by_now[conn_id] - 1);
        }
        /* Handle segment received from the sender. We use this between locks
        as to not have synchronization issues with the recv_data calls which are
        on the main thread */
        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }

    
}

int wait4connect(uint32_t ip, uint16_t port)
{
    /* TODO: Implement the Three Way Handshake on the receiver part. This blocks
     * until a connection is established. */

    struct connection *con = (struct connection *)malloc(sizeof(struct connection));
    int conn_id = fdmax;
    sockaddr_in all_addr, client_addr;
    int all_addr_len = sizeof(sockaddr_in), client_addr_len = sizeof(sockaddr_in);
    int rc;

    all_addr.sin_addr.s_addr = ip;
    all_addr.sin_port = port;
    all_addr.sin_family = AF_INET;

    /* This can be used to set a timer on a socket, useful once we received a
     * SYN. You may want to disable by setting the time to 0 (tv_sec = 0,
     * tv_usec = 0)
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 100000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    } */

    /* Receive SYN on the connection socket. Create a new socket and bind it to
     * the chosen port. Send the data port number via SYN-ACK to the client */
    con->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    con->conn_id = conn_id;
    rc = bind(con->sockfd, (sockaddr *)&all_addr, all_addr_len);
    if (rc < 0) {
        DEBUG_PRINT("could not bind\n");
        exit(-1);
    }


    poli_tcp_ctrl_hdr ctrl_hdr;
    poli_tcp_ctrl_hdr recv_ack_hdr;

    // Pachetul primit trebuie sa fie de tipul connect de la un client
    do {
        // Here we receive a SYN
        rc = recvfrom(con->sockfd, &ctrl_hdr, sizeof(ctrl_hdr), 0,
                        (sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
    } while (ctrl_hdr.type != 0);

    printf("Received request\n");
    window_size = ctrl_hdr.recv_window; // window size
    con->max_window_seq = window_size + 1;

    acked_data[con->conn_id] = new bool[con->max_window_seq];
    acked_by_now[con->conn_id] = 1;
    
    for (int i = 0; i < window_size; i++)
        acked_data[conn_id][i] = false;

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 100000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    }

    ctrl_hdr.type = 1; // tipul pentru a transmite SYN/ACK la 3 way handshake
    ctrl_hdr.conn_id = conn_id;
    // DEBUG_PRINT("client connected to port: %d\n", 8033 + ctrl_hdr.conn_id);
    ctrl_hdr.ack_num = 0;

    // client_addr.sin_port = port;

    for (int i = 0; i < 3; i++) {
        rc = sendto(con->sockfd, &ctrl_hdr, sizeof(ctrl_hdr), 0,
                        (sockaddr *)&client_addr, client_addr_len);

        if (rc < 0) {
            perror("eroare la send ack");
            continue;
        }
        printf("Am trimis\n");
        for (int i = 0; i < 3; i++){
            rc = recvfrom(con->sockfd, &recv_ack_hdr, sizeof(recv_ack_hdr), 0,
                        (sockaddr *)&client_addr, (socklen_t *) &client_addr_len);
            if (rc >= 0) {
                break;
            }
        }
        if (rc >= 0)
            break;
    }

    if (rc < 0) {
        perror ("Nu s-a putut stabili o conexiune");
    }


    memcpy(&con->servaddr, &client_addr, sizeof(sockaddr_in));
    // con->servaddr.sin_port = htons(8033 + con->conn_id);
    

    pthread_mutex_init(&con->con_lock, NULL);
    pthread_mutex_lock(&con->con_lock);
    /* Since we can have multiple connection, we want to know if data is available
       on the socket used by a given connection. We use POLL for this */
    data_fds[fdmax].fd = con->sockfd;    
    data_fds[fdmax].events = POLLIN;    
    
    /* This creates a timer and sets it to trigger every 1 sec. We use this
       to know if a timeout has happend on a connection */
    timer_fds[fdmax].fd = timerfd_create(CLOCK_REALTIME,  0);    
    timer_fds[fdmax].events = POLLIN;    
    struct itimerspec spec;     
    spec.it_value.tv_sec = 1;    
    spec.it_value.tv_nsec = 0;    
    spec.it_interval.tv_sec = 1;    
    spec.it_interval.tv_nsec = 0;    
    timerfd_settime(timer_fds[fdmax].fd, 0, &spec, NULL);  
    cons.insert({conn_id, con});  
    fdmax++;    

    
    
    pthread_mutex_unlock(&con->con_lock);

    DEBUG_PRINT("Connection established!");

    return 0;
}

void init_receiver(int recv_buffer_bytes)
{
    pthread_t thread1;
    int ret = 0;

    /* TODO: Create the connection socket and bind it to 8031 */
    // sockaddr_in all_addr;
    // int all_addr_len = sizeof(sockaddr_in);
    // all_addr.sin_addr.s_addr = (INADDR_ANY);
    // all_addr.sin_port = htons(8031);
    // all_addr.sin_family = AF_INET;

    for (int i = 0; i < MAX_CONNECTIONS; i++)
        received_data_buffer[i] = (uint8_t *)calloc(recv_buffer_bytes, sizeof(uint8_t));
    max_buffer_size = recv_buffer_bytes;

    // udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    // int rc = bind(udp_socket, (sockaddr *)&all_addr, all_addr_len);
    

    ret = pthread_create( &thread1, NULL, receiver_handler, NULL);
    assert(ret == 0);
}
