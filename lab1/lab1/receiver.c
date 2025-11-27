#include "receiver.h"

// Semaphore names
#define SEM_SENDER "/sem_sender"
#define SEM_RECEIVER "/sem_receiver"

// Shared keys for System V IPC
#define MSG_KEY 1234
#define SHM_KEY 5678

void receive(message_t* message_ptr, mailbox_t* mailbox_ptr){
    /*  TODO: 
        1. Use flag to determine the communication method
        2. According to the communication method, receive the message
    */
    
    // (1) 判斷模式 - Determine communication method by flag
    if (mailbox_ptr->flag == MSG_PASSING) {
        // Message Passing - using System V message queue
        // kernel 介入(記憶體複製兩次)
        // Fix: correct size parameter
        if (msgrcv(mailbox_ptr->storage.msqid, message_ptr, 
                   sizeof(message_t) - sizeof(long), 1, 0) == -1) {
            perror("msgrcv failed");
            exit(1);
        }
        printf("Receive message: %s\n", message_ptr->msgText);
        
    } else if (mailbox_ptr->flag == SHARED_MEM) {
        // Shared Memory - using System V shared memory
        // 記憶體複製 (直接存取)
        strcpy(message_ptr->msgText, mailbox_ptr->storage.shm_addr);
        printf("Receive message: %s\n", message_ptr->msgText);
    }
}

int main(int argc, char *argv[]){
    /*  TODO: 
        1) Call receive(&message, &mailbox) according to the flow in slide 4
        2) Measure the total receiving time
        3) Get the mechanism from command line arguments
            • e.g. ./receiver 1
        4) Print information on the console according to the output format
        5) If the exit message is received, print the total receiving time and terminate the receiver.c
    */
    
    // (3) Get the mechanism from command line arguments
    // error check
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <mechanism>\n", argv[0]);
        fprintf(stderr, "mechanism: 1 for Message Passing, 2 for Shared Memory\n");
        return 1;
    }
    
    int mechanism = atoi(argv[1]);
    
    if (mechanism != MSG_PASSING && mechanism != SHARED_MEM) {
        fprintf(stderr, "Invalid mechanism. Use 1 or 2.\n");
        return 1;
    }
    
    // Initialize mailbox structure
    mailbox_t mailbox;
    mailbox.flag = mechanism;
    
    // Open existing semaphores created by sender
    sem_t *sem_sender = sem_open(SEM_SENDER, 0);
    sem_t *sem_receiver = sem_open(SEM_RECEIVER, 0);
    
    if (sem_sender == SEM_FAILED || sem_receiver == SEM_FAILED) {
        perror("sem_open failed - make sure sender is running first");
        return 1;
    }
    
    // Get existing communication mechanism
    if (mechanism == MSG_PASSING) {
        // Get message queue using System V API
        mailbox.storage.msqid = msgget(MSG_KEY, 0666);
        if (mailbox.storage.msqid == -1) {
            perror("msgget failed - make sure sender is running first");
            sem_close(sem_sender);
            sem_close(sem_receiver);
            return 1;
        }
        printf("Message Passing\n");
        
    } else {
        // Get shared memory using System V API
        // Get the shared memory segment ID
        int shmid = shmget(SHM_KEY, 1024, 0666);
        if (shmid == -1) {
            perror("shmget failed - make sure sender is running first");
            sem_close(sem_sender);
            sem_close(sem_receiver);
            return 1;
        }
        mailbox.storage.shm_addr = (char *)shmat(shmid, NULL, 0);
        if (mailbox.storage.shm_addr == (char *)-1) {
            perror("shmat failed");
            sem_close(sem_sender);
            sem_close(sem_receiver);
            return 1;
        }
        printf("Shared Memory\n");
    }
    
    message_t message;
    struct timespec start, end;
    double total_time = 0.0;
    
    // (1) Call receive(&message, &mailbox) according to the flow in slide 4
    // (4) Print information on the console according to the output format
    while (1) {
        // Flow: Wait for sender to send message (wait Sender_SEM)
        sem_wait(sem_sender);
        
        // (2) Measure only the actual communication time
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        // Receive message without printing yet
        if (mailbox.flag == MSG_PASSING) {
            if (msgrcv(mailbox.storage.msqid, &message, 
                       sizeof(message_t) - sizeof(long), 1, 0) == -1) {
                perror("msgrcv failed");
                exit(1);
            }
        } else if (mailbox.flag == SHARED_MEM) {
            strcpy(message.msgText, mailbox.storage.shm_addr);
        }
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        
        // Accumulate communication time
        total_time += (end.tv_sec - start.tv_sec) + 
                      (end.tv_nsec - start.tv_nsec) * 1e-9;
        
        // (5) Check if exit message is received
        if (strcmp(message.msgText, "exit") == 0) {
            break;
        }
        
        // Print message only if it's not exit
        printf("receive message: %s\n", message.msgText);
        
        // Signal receiver semaphore (signal Receiver_SEM)
        sem_post(sem_receiver);
    }
    
    // (5) Print the total receiving time
    printf("\033[31mSender Exit!\033[0m\n");
    printf("Total time taken in receiving messages: %f seconds\n", total_time);
    
    // Cleanup IPC resources
    if (mechanism == MSG_PASSING) {
        msgctl(mailbox.storage.msqid, IPC_RMID, NULL);
    } else {
        int shmid = shmget(SHM_KEY, 1024, 0666);
        shmdt(mailbox.storage.shm_addr);
        shmctl(shmid, IPC_RMID, NULL);
    }
    
    // Cleanup semaphores
    sem_close(sem_sender);
    sem_close(sem_receiver);
    sem_unlink(SEM_SENDER);
    sem_unlink(SEM_RECEIVER);
    
    return 0;
}