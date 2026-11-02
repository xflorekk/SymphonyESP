#ifndef JSON_H
#define JSON_H

/* TODO:
    parsing and writing into json format needs to be unified
    accross the client and server app, it means the commands that server handles
    and the ones handled by client are the same.
    write separate code for handling commands, that can be later used in other
    applications.
    understand the purpose of parse and write
*/

int json_parse(void);
char *json_write(void);

#endif