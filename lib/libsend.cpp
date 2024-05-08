#include <pthread.h>
#include <cstdlib>
#include <map>
#include <cstdint>
#include "lib.h"
#include "utils.h"
#include "protocol.h"
#include <cassert>
#include <poll.h>
#include <sys/timerfd.h>

#include <string.h>
#include <vector>
#include <cerrno>

using namespace std;

std::map<int, struct connection *> cons;

struct pollfd data_fds[MAX_CONNECTIONS];
/* Used for timers per connection */
struct pollfd timer_fds[MAX_CONNECTIONS];
int fdmax = 0;

int window_size;

int on_air; // Numarul de segmente care sunt 'pe retea'

int current_seq_no = 1;

std::vector<char *> to_be_sent[MAX_CONNECTIONS];
std::vector<char *> to_be_acked[MAX_CONNECTIONS];

int my_port;

poli_tcp_data_hdr *get_header_from_segment(char *segment) {
    return (poli_tcp_data_hdr *)segment;
}

bool can_add_to_window(int conn_id) {
    if (to_be_sent[conn_id].empty())
        return false;

    if (to_be_acked[conn_id].empty()) {
        return true;
    }

    if (int(get_header_from_segment(to_be_sent[conn_id][0])->seq_num 
        - get_header_from_segment(to_be_sent[conn_id][0])->seq_num) <= window_size)
        return true;

    return false;
}

int send_queue_first(connection *conn) {
    // DEBUG_PRINT("here line 39\n");
    if (to_be_sent[conn->conn_id].empty()) {
        return -1; // pot sa ii dau exit aici
    }
    auto curr_buff = to_be_sent[conn->conn_id][0];
    to_be_acked[conn->conn_id].push_back(curr_buff);
    int rc = 0;
    if (can_add_to_window(conn->conn_id)) {
        to_be_sent[conn->conn_id].erase(to_be_sent[conn->conn_id].begin());
        rc = sendto(conn->sockfd, curr_buff, sizeof(poli_tcp_data_hdr) +
                        get_header_from_segment(curr_buff)->len, 0,
                        (sockaddr *)&(conn->servaddr), sizeof(conn->servaddr));

        if (rc < 0) {
                DEBUG_PRINT("EROERH");
        }
        DEBUG_PRINT("%d bytes sent with frame %d\n", rc, get_header_from_segment(curr_buff)->seq_num);

        on_air++;
        DEBUG_PRINT("on_air = %d\n", on_air);
    } else {
        DEBUG_PRINT("ADDED BUT NOT SENT\n");
    }
    return rc;
}

int send_segment(int conn_id, char *buffer, int len) {
    int rc;
    // DEBUG_PRINT("IN SEND SEGMENT\n");
    sockaddr_in client_addr;
    connection *conn = cons[conn_id];
    client_addr = (conn->servaddr);
    int max_sent = std::min(len, MAX_DATA_SIZE);
    poli_tcp_data_hdr data_hdr;
    data_hdr.conn_id = conn_id;
    data_hdr.len = max_sent;
    data_hdr.protocol_id = POLI_PROTOCOL_ID;
    data_hdr.type = 3; // type -> data
    data_hdr.seq_num = current_seq_no++;
    int addr_len = sizeof(client_addr);
    // DEBUG_PRINT("IN SEND SEGMENT2\n");
    char *buff = (char *)calloc(MAX_SEGMENT_SIZE, sizeof(char));
    // memset(buff, 0, MAX_SEGMENT_SIZE);
    *(poli_tcp_data_hdr *)buff = data_hdr;
    memcpy(buff + sizeof(poli_tcp_data_hdr), buffer, max_sent);
    // DEBUG_PRINT("IN SEND SEGMENT3\n");
    // while (on_air >= window_size);
    // bool sent_now = false;
    // pthread_mutex_lock(&cons[conn_id]->con_lock);
    // DEBUG_PRINT("IN SEND SEGMENT4\n");
    to_be_sent[conn_id].push_back(buff);
    
    if (on_air < window_size) {
        // sent_now = true;
        send_queue_first(conn);
        
    } 
    
    // pthread_mutex_unlock(&cons[conn_id]->con_lock);

    return max_sent;
}

int send_data(int conn_id, char *buffer, int len)
{
    int size = 0;

    int aux_size;

    pthread_mutex_lock(&cons[conn_id]->con_lock);
    DEBUG_PRINT("In sent data. Sending %d bytes\n", len);
    /* We will write code here as to not have sync problems with sender_handler */
    do {
        
        if (len > 0) {
            aux_size = send_segment(conn_id, buffer + size, len);
            len -= aux_size;
            size += aux_size;
        } else break;
        DEBUG_PRINT("sent %d bytes\n", aux_size);
    } while (on_air < window_size);

    pthread_mutex_unlock(&cons[conn_id]->con_lock);

    return size;
}

bool ack_received(int conn_id, int seq_no, bool erase_all_smaller = false) {
    bool ret_val = false;
    for (int i = 0; i < int(to_be_acked[conn_id].size()); i++) {
        auto buff = to_be_acked[conn_id][i];
        if (!erase_all_smaller){
            if (seq_no == get_header_from_segment(buff)->seq_num) {
                DEBUG_PRINT("DELETED %d FROM TO_BE_ACKED\n", seq_no);
                free(buff);
                to_be_acked[conn_id].erase(to_be_acked[conn_id].begin() + i);
                on_air--;
                ret_val = true;
                break;
            }
        } else {
            if (seq_no >= get_header_from_segment(buff)->seq_num) {
                DEBUG_PRINT("DELETED %d FROM TO_BE_ACKED\n", get_header_from_segment(buff)->seq_num);
                free(buff);
                to_be_acked[conn_id].erase(to_be_acked[conn_id].begin() + i);
                on_air--;
                i--;
                ret_val = true;
            }
        }
    }
    return ret_val;
}

void *sender_handler(void *arg)
{
    int res = 0;
    char buf[MAX_SEGMENT_SIZE];

    while (1) {

        if (cons.size() == 0) {
            continue;
        }
        int conn_id = -1;
        do {
            res = recv_message_or_timeout(buf, MAX_SEGMENT_SIZE, &conn_id);
        } while(res == -14);


        DEBUG_PRINT("res = %d\n", res);

        /* Handle segment received from the receiver. We use this between locks
        as to not have synchronization issues with the send_data calls which are
        on the main thread */
        poli_tcp_data_hdr *data_hdr;
        connection *conn = cons[conn_id];
        int rc;

        /**
         * Am primit timeout, asa ca ar trebui sa trimit din nou pachetele care asteapta ack
        */
       pthread_mutex_lock(&cons[conn_id]->con_lock);
        if (res < 0) {
            for (int i = 0; i < int(to_be_acked[conn_id].size()); i++) {
                data_hdr = (poli_tcp_data_hdr *)to_be_acked[conn_id][i];
                
                rc = sendto(conn->sockfd, to_be_acked[conn_id][i], 
                    sizeof(data_hdr) + data_hdr->len, 0,
                    (sockaddr *)&(conn->servaddr), sizeof((conn->servaddr)));

                if (rc < 0) {
                    perror("eroare la send again\n");
                }
                DEBUG_PRINT("SENT AGAIN: frame %d\n", data_hdr->seq_num);
            }
            pthread_mutex_unlock(&cons[conn_id]->con_lock);
            continue;
        }

        

        /**
         * Am primit un ack
        */
       
        poli_tcp_ctrl_hdr ctrl_hdr = *(poli_tcp_ctrl_hdr *)buf;
        DEBUG_PRINT("ctrl_hdr_type = %d\n; seq \n", ctrl_hdr.type);
        // DEBUG_PRINT("")
        if (ctrl_hdr.type != 4  && ctrl_hdr.type != 7){
            pthread_mutex_unlock(&cons[conn_id]->con_lock);
            continue;
        }
        DEBUG_PRINT("GOT ACK FOR %d\n", ctrl_hdr.ack_num);

        DEBUG_PRINT("TO BE ACKED: ");
        for (const auto &every: to_be_acked[conn_id]) {
            DEBUG_PRINT("%d ", get_header_from_segment(every)->seq_num);
        }
        DEBUG_PRINT("\n");

        if (ack_received(conn_id, ctrl_hdr.ack_num, ctrl_hdr.type == 7)) {
            if (to_be_acked[conn_id].empty() || can_add_to_window(conn_id)) {
                while(on_air < window_size) {
                    int rc = send_queue_first(conn);
                    if (rc == -1) {
                        break;
                    }
                }
            }
        }
        
        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }
}

int setup_connection(uint32_t ip, uint16_t port)
{
    /* Implement the sender part of the Three Way Handshake. Blocks
    until the connection is established */

    struct connection *con = (struct connection *)malloc(sizeof(struct connection));
    int conn_id = fdmax + 1;
    con->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    con->max_window_seq = window_size;
    con->conn_id = conn_id;

    if (con->sockfd < 0) {
        DEBUG_PRINT("Could not setup connection");
        exit(1);
    }


    /* // This can be used to set a timer on a socket 
    
    } */
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 100000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    }
    /* We will send the SYN on 8031. Then we will receive a SYN-ACK with the connection
     * port. We can use con->sockfd for both cases, but we will need to update server_addr
     * with the port received via SYN-ACK */

    poli_tcp_ctrl_hdr ctrl_hdr;
    ctrl_hdr.protocol_id = POLI_PROTOCOL_ID;
    ctrl_hdr.conn_id = con->conn_id;
    ctrl_hdr.type = 0;
    ctrl_hdr.recv_window = window_size; // window_size included
    int rc;

    // int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serveraddr;
    int serveraddr_len = sizeof(serveraddr);

    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = port;
    serveraddr.sin_addr.s_addr = ip;
    
    for (int i1 = 0; i1 < 1; i1++) {     
        do {
            rc = sendto(con->sockfd, &ctrl_hdr, sizeof(ctrl_hdr), 0, 
                (sockaddr *)&serveraddr, serveraddr_len);
        } while (0);

        for (int i = 0; i < 1; i++) {
            DEBUG_PRINT("trying to receive something\n");
            rc = recv(con->sockfd, &ctrl_hdr, sizeof(ctrl_hdr), 0);
                        // (sockaddr *)&serveraddr,
                        // (socklen_t *) &serveraddr_len);

            if (rc >= 0)
                break;

            DEBUG_PRINT("errno = %d\n", errno);
        }
        if (rc  >= 0) {
            break;
        }
    }
    DEBUG_PRINT("done doing send and recv\n");

    if (rc < 0) {
        perror("did not received anything SYN/ACK");
        exit(1);
    }
    ctrl_hdr.ack_num += 1;
    rc = sendto(con->sockfd, &ctrl_hdr, sizeof(ctrl_hdr), 0, 
            (sockaddr *)&serveraddr, serveraddr_len);
    memcpy(&(con->servaddr), &serveraddr, serveraddr_len);

    /* Since we can have multiple connection, we want to know if data is available
       on the socket used by a given connection. We use POLL for this */
    data_fds[fdmax].fd = con->sockfd;    
    data_fds[fdmax].events = POLLIN;    
    
    /* This creates a timer and sets it to trigger every 1 sec. We use this
       to know if a timeout has happend on our connection */
    timer_fds[fdmax].fd = timerfd_create(CLOCK_REALTIME,  0);    
    timer_fds[fdmax].events = POLLIN;    
    struct itimerspec spec;     
    spec.it_value.tv_sec = 1;    
    spec.it_value.tv_nsec = 0;    
    spec.it_interval.tv_sec = 1;    
    spec.it_interval.tv_nsec = 0;    
    timerfd_settime(timer_fds[fdmax].fd, 0, &spec, NULL);    
    fdmax++;

    cons.insert({conn_id, con});

    pthread_mutex_init(&con->con_lock, NULL);
    

    DEBUG_PRINT("Connection established!");

    return conn_id;
}

void init_sender(int speed, int delay)
{
    pthread_t thread1;
    int ret = 0;

    /* Create a thread that will*/

    //speed -> Mb / s, delay -> ms => 
    int bdp = speed * delay * 1000; // (KB)
    window_size = std::max(1UL, bdp / MAX_SEGMENT_SIZE);
    // window_size = 30;
    DEBUG_PRINT("WINDOW SIZE:%d\n", window_size);
    ret = pthread_create( &thread1, NULL, sender_handler, NULL);
    assert(ret == 0);
}
