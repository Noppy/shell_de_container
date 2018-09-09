#define  _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <linux/sched.h>
#include <sys/types.h>
#include <sys/wait.h>

#define CLONE_FLAGS CLONE_NEWUTS|CLONE_NEWPID|CLONE_NEWNS|SIGCHLD
#define STACK_SIZE (1024 * 1024)

static int childFunc()
{
    char cmd[]    = "/bin/bash";
    char *exargv[] = {"shZZZ", NULL};
    char *exenvp[] = { NULL };
    execve(cmd, exargv, exenvp);

    perror("failed execve");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    pid_t pid;
    char  *stack;
    char  *stackTop;
    int   status, ret;

    /* alloc memory for stack */
    if( (stack = malloc(STACK_SIZE)) == NULL ){
        perror("malloc stack");
        exit(EXIT_FAILURE);
    }
    stackTop = stack + STACK_SIZE;

    /* clone child process */
    pid = clone( childFunc, stackTop, CLONE_FLAGS, NULL);

    /* parent process */
    if( pid == -1 ){
        perror("failed clone");
        ret = EXIT_SUCCESS;
    }else{
        printf("child process pid=%d\n",pid);
        waitpid( -1, &status, 0 );
        if( WIFEXITED(status) ){
            ret = WEXITSTATUS(status);
        }else{
            ret = EXIT_FAILURE;
        }
    }
    exit(ret);
}
