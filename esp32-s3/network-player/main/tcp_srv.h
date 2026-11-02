#ifndef TCP_SRV_H
#define TCP_SRV_H

#define TCP_SRV_PORT 3333

int tcp_srv_init(void);
int tcp_srv_start(void);
int tcp_srv_stop(void);

int tcp_srv_broadcast(const char *json_data);
int tcp_srv_send_error(int client_fd, const char *json_data);

#endif