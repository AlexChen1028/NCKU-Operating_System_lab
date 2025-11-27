#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <time.h>

#define MSG_PASSING 1
#define SHARED_MEM 2

typedef struct {
    int flag;      // 1 for message passing, 2 for shared memory
    union{
        int msqid; //for system V api. You can replace it with structure for POSIX api
        char* shm_addr;
    }storage;
} mailbox_t;


typedef struct {
    /*  TODO: 
        Message structure for wrapper
    */
    long mType;
    char msgText[1024];
} message_t;

void receive(message_t* message_ptr, mailbox_t* mailbox_ptr);
