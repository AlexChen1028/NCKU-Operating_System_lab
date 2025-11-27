#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "../include/command.h"
#include "../include/builtin.h"

// ======================= requirement 2.3 =======================
/**
 * @brief 
 * Redirect command's stdin and stdout to the specified file descriptor
 * If you want to implement ( < , > ), use "in_file" and "out_file" included the cmd_node structure
 * If you want to implement ( | ), use "in" and "out" included the cmd_node structure.
 *
 * @param p cmd_node structure
 * */
void redirection(struct cmd_node *p){
    // 處理 Input Redirection (<)
    if (p->in_file) {
        int fd = open(p->in_file, O_RDONLY);
        if (fd < 0) {
            perror("redirection: open input file");
            // 注意：若在子行程中失敗，應結束行程；若在父行程(built-in)，應返回。
            // 這裡僅做基本錯誤提示。
            return; 
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("redirection: dup2 input");
        }
        close(fd);
    }
    // 處理來自 Pipe 的輸入 (如果 p->in 被設定為非標準輸入)
    else if (p->in != STDIN_FILENO) {
        if (dup2(p->in, STDIN_FILENO) < 0) {
            perror("redirection: dup2 pipe in");
        }
        close(p->in);
    }

    // 處理 Output Redirection (>)
    if (p->out_file) {
        // 權限：User可讀寫，Group/Others可讀 (0644)
        int fd = open(p->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("redirection: open output file");
            return;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("redirection: dup2 output");
        }
        close(fd);
    } 
    // 處理輸出到 Pipe (如果 p->out 被設定為非標準輸出)
    else if (p->out != STDOUT_FILENO) {
        if (dup2(p->out, STDOUT_FILENO) < 0) {
            perror("redirection: dup2 pipe out");
        }
        close(p->out);
    }
}
// ===============================================================

// ======================= requirement 2.2 =======================
/**
 * @brief 
 * Execute external command
 * The external command is mainly divided into the following two steps:
 * 1. Call "fork()" to create child process
 * 2. Call "execvp()" to execute the corresponding executable file
 * @param p cmd_node structure
 * @return int 
 * Return execution status
 */
int spawn_proc(struct cmd_node *p)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("spawn_proc: fork");
        return 1;
    } else if (pid == 0) {
        // Child process
        redirection(p); // 處理重導向
        
        execvp(p->args[0], p->args);
        
        // 如果 execvp 返回，表示發生錯誤
        perror("spawn_proc: execvp");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        int status;
        waitpid(pid, &status, 0); // 等待子行程結束
        return 1;
    }
}
// ===============================================================


// ======================= requirement 2.4 =======================
/**
 * @brief 
 * Use "pipe()" to create a communication bridge between processes
 * Call "spawn_proc()" in order according to the number of cmd_node
 * @param cmd Command structure  
 * @return int
 * Return execution status 
 */
int fork_cmd_node(struct cmd *cmd)
{
    struct cmd_node *p = cmd->head;
    int pipe_fd[2];
    int input_fd = STDIN_FILENO; // 初始輸入為標準輸入

    while (p != NULL) {
        // 如果還有下一個指令，建立 pipe
        if (p->next != NULL) {
            if (pipe(pipe_fd) < 0) {
                perror("fork_cmd_node: pipe");
                return 1;
            }
            p->out = pipe_fd[1]; // 當前指令輸出到 pipe 寫入端
        } else {
            p->out = STDOUT_FILENO; // 最後一個指令輸出到標準輸出
        }

        // 設定當前指令的輸入 (來自上一個 pipe 或標準輸入)
        p->in = input_fd;

        // Fork 行程
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork_cmd_node: fork");
            return 1;
        } else if (pid == 0) {
            // Child process
            
            // 如果當前有下一個指令，子行程不需要讀取端，應關閉避免 hang
            if (p->next != NULL) {
                close(pipe_fd[0]);
            }

            // 執行重導向 (處理 p->in, p->out 以及檔案重導向)
            redirection(p);

            execvp(p->args[0], p->args);
            perror("fork_cmd_node: execvp");
            exit(EXIT_FAILURE);
        }

        // Parent process
        
        // 關閉父行程中不再需要的 pipe 寫入端 (子行程已經繼承並在使用)
        if (p->next != NULL) {
            close(pipe_fd[1]);
        }

        // 關閉上一個指令的輸入端 (如果不是標準輸入)
        if (input_fd != STDIN_FILENO) {
            close(input_fd);
        }

        // 將當前 pipe 的讀取端設為下一個指令的輸入
        if (p->next != NULL) {
            input_fd = pipe_fd[0];
        }

        p = p->next;
    }

    // 等待所有子行程結束
    while (wait(NULL) > 0);

	return 1;
}
// ===============================================================


void shell()
{
	while (1) {
		printf(">>> $ ");
		char *buffer = read_line();
		if (buffer == NULL)
			continue;

		struct cmd *cmd = split_line(buffer);
		
		int status = -1;
		// only a single command
		struct cmd_node *temp = cmd->head;
		
		if(temp->next == NULL){
			status = searchBuiltInCommand(temp);
			if (status != -1){
				int in = dup(STDIN_FILENO), out = dup(STDOUT_FILENO);
				if( in == -1 | out == -1)
					perror("dup");
				redirection(temp);
				status = execBuiltInCommand(status,temp);

				// recover shell stdin and stdout
				if (temp->in_file)  dup2(in, 0);
				if (temp->out_file){
					dup2(out, 1);
				}
				close(in);
				close(out);
			}
			else{
				//external command
				status = spawn_proc(cmd->head);
			}
		}
		// There are multiple commands ( | )
		else{
			
			status = fork_cmd_node(cmd);
		}
		// free space
		while (cmd->head) {
			
			struct cmd_node *temp = cmd->head;
      		cmd->head = cmd->head->next;
			free(temp->args);
   	    	free(temp);
   		}
		free(cmd);
		free(buffer);
		
		if (status == 0)
			break;
	}
}