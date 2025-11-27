#include "sender.h"

// Semaphore names
#define SEM_SENDER "/sem_sender"
#define SEM_RECEIVER "/sem_receiver"

// Shared keys for System V IPC
#define MSG_KEY 1234
#define SHM_KEY 5678

void send(message_t message, mailbox_t* mailbox_ptr){
    /*  TODO: 
        1. Use flag to determine the communication method
        2. According to the communication method, send the message
    */
    
    // (1) Determine communication method by flag
    if (mailbox_ptr->flag == MSG_PASSING) {
        // Message Passing - using System V message queue
        if (msgsnd(mailbox_ptr->storage.msqid, &message, sizeof(message.msgText), 0) == -1) {
            perror("msgsnd failed");
            exit(1);
        }
        printf("Send message: %s\n", message.msgText);

    } else if (mailbox_ptr->flag == SHARED_MEM) {
        // Shared Memory - using System V shared memory
        strcpy(mailbox_ptr->storage.shm_addr, message.msgText);
        printf("Send message: %s\n", message.msgText);
    }
}

int main(int argc, char *argv[]){
    /*  TODO: 
        1) Call send(message, &mailbox) according to the flow in slide 4
        2) Measure the total sending time
        3) Get the mechanism and the input file from command line arguments
            • e.g. ./sender 1 input.txt
                    (1 for Message Passing, 2 for Shared Memory)
        4) Get the messages to be sent from the input file
        5) Print information on the console according to the output format
        6) If the message form the input file is EOF, send an exit message to the receiver.c
        7) Print the total sending time and terminate the sender.c
    */
    
    // (3) Get the mechanism and the input file from command line arguments
    // error check
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <mechanism> <input_file>\n", argv[0]);
        fprintf(stderr, "mechanism: 1 for Message Passing, 2 for Shared Memory\n");
        return 1;
    }
    
    int mechanism = atoi(argv[1]);
    char *input_file = argv[2];
    
    if (mechanism != MSG_PASSING && mechanism != SHARED_MEM) {
        fprintf(stderr, "Invalid mechanism. Use 1 or 2.\n");
        return 1;
    }
    
    // Initialize mailbox structure
    mailbox_t mailbox;
    mailbox.flag = mechanism;
    
    // Create semaphores using POSIX API
    sem_t *sem_sender = sem_open(SEM_SENDER, O_CREAT, 0666, 0);
    sem_t *sem_receiver = sem_open(SEM_RECEIVER, O_CREAT, 0666, 1);
    
    if (sem_sender == SEM_FAILED || sem_receiver == SEM_FAILED) {
        perror("sem_open failed");
        return 1;
    }
    
    // Create communication mechanism
    if (mechanism == MSG_PASSING) {
        // Create message queue using System V API
        // msqid得到queue的ID, MSG是kernel key, 0666是權限設定
        mailbox.storage.msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
        if (mailbox.storage.msqid == -1) {
            perror("msgget failed");
            sem_close(sem_sender);
            sem_close(sem_receiver);
            return 1;
        }
        printf("Message Passing\n");
        
    } else {
        // Create shared memory using System V API
        // shmget得到segment的ID, SHM是kernel key, 0666是權限設定
        int shmid = shmget(SHM_KEY, 1024, IPC_CREAT | 0666);
        if (shmid == -1) {
            perror("shmget failed");
            sem_close(sem_sender);
            sem_close(sem_receiver);
            return 1;
        }
        // Attach the shared memory segment
        // shmat得到segment的地址
        // NULL表示讓kernel自動選擇地址
        mailbox.storage.shm_addr = (char *)shmat(shmid, NULL, 0);
        if (mailbox.storage.shm_addr == (char *)-1) {
            perror("shmat failed");
            sem_close(sem_sender);
            sem_close(sem_receiver);
            return 1;
        }
        printf("Shared Memory\n");
    }
    
    // (4) Get the messages to be sent from the input file
    FILE *fp = fopen(input_file, "r");
    if (fp == NULL) {
        perror("Error opening file");
        return 1;
    }
    
    message_t message;
    message.mType = 1;
    
    struct timespec start, end;
    double total_time = 0.0;
    
    // (1) Call send(message, &mailbox) according to the flow in slide 4
    // (5) Print information on the console according to the output format
    // 從input file讀取每一行, 直到\n或EOF, 最多讀取sizeof(msgText)-1個字元
    while (fgets(message.msgText, sizeof(message.msgText), fp) != NULL) {
        // Remove trailing newline character
        size_t len = strlen(message.msgText);
        if (len > 0 && message.msgText[len - 1] == '\n') {
            message.msgText[len - 1] = '\0';
        }
        
        // Flow: Wait for receiver to be ready (wait Receiver_SEM)
        sem_wait(sem_receiver);
        
        // (2) Measure only the actual communication time
        clock_gettime(CLOCK_MONOTONIC, &start);
        send(message, &mailbox);
        clock_gettime(CLOCK_MONOTONIC, &end);
        
        // Accumulate communication time
        total_time += (end.tv_sec - start.tv_sec) + 
                      (end.tv_nsec - start.tv_nsec) * 1e-9;
        
        // Signal sender semaphore (signal Sender_SEM)
        sem_post(sem_sender);
    }
    
    // (6) If the message form the input file is EOF, send an exit message to the receiver.c
    sem_wait(sem_receiver);
    // 把msgText設為"exit"
    strcpy(message.msgText, "exit");
    
    // (2) Measure only the actual communication time
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Send exit message without printing
    if (mailbox.flag == MSG_PASSING) {
        if (msgsnd(mailbox.storage.msqid, &message, sizeof(message.msgText), 0) == -1) {
            perror("msgsnd failed");
            exit(1);
        }
    } else if (mailbox.flag == SHARED_MEM) {
        strcpy(mailbox.storage.shm_addr, message.msgText);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    total_time += (end.tv_sec - start.tv_sec) + 
                  (end.tv_nsec - start.tv_nsec) * 1e-9;
    
    sem_post(sem_sender);
    
    // (7) Print the total sending time
    printf("\033[31mEnd of input file! exit!\033[0m\n");
    printf("Total time taken in sending messages: %f seconds\n", total_time);
    
    // Cleanup
    fclose(fp);
    
    if (mechanism == SHARED_MEM) {
        shmdt(mailbox.storage.shm_addr);
    }
    
    sem_close(sem_sender);
    sem_close(sem_receiver);
    
    return 0;
}