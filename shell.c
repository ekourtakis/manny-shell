#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
    #define PATH_MAX 1024
#else
    #include <linux/limits.h>
#endif

#include "parse.h"

#define SHELL_CONTINUE 1
#define SHELL_BREAK 0

/* SHELL FUNCTIONS */

bool shellLoop(void); 
void printPrompt(void);
char *getCWD();
char *getInput(void);
char **parseLine(char *line);
bool builtInCalled(char **argv);
bool pipeCalled(char **argv);
bool callFromPATH(char **argv);
size_t getNumArgs(char **argv);
bool hasPipe(char **argv);
char ***splitArgv(char **argv);
void execCommand(char **argv);
char *resolvePath(char *command);
void redirect(char **argv);
void setNewIn(char *path);
void setNewOut(char *path);
void resizeArgv(char **argv, size_t index);
void handleSIGCHLD(int sig);

/* BUILT-IN FUNCTIONS */

void cd(char *path);
void help(void);
void pwd(void);
void shellWait(void);

/* GLOBAL VARS */
char **PATH_arr;
FILE *helpFile;
size_t backgroundProcesses = 0;
bool isWaiting = false;

/* MAIN */
int main(void) {
    /* set up */
    // signal handler for reaping background processes
    struct sigaction sa;
    sa.sa_handler = &handleSIGCHLD;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    // PATH
    char *_PATH = getenv("PATH");
    if (_PATH == NULL) {
        perror("PATH error");
        return EXIT_FAILURE;
    }
    char *PATH = strdup(_PATH);
    PATH_arr = parse(PATH, ":");

    // help
    helpFile = fopen("./help.txt", "r");

    // clear terminal before starting
    printf("%s", "\e[1;1H\e[2J"); 

    // enter shell
    while (shellLoop());

    /* clean up */
    fclose(helpFile);

    free(PATH);
    free(PATH_arr);

    return EXIT_SUCCESS;
}


/* SHELL FUNCTIONS */

/* prompts user for input, then parses and executes input
   everyting to handle one interaction from user */
bool shellLoop() {
    printPrompt();

    char *line = getInput();

    // let user press return arbitrary amount, check for NULL
    if (line[0] == '\n' || line == NULL) {
        free(line);
        return SHELL_CONTINUE;
    }

    char **argv = parseLine(line);
    if (argv == NULL) { // if failed
        perror("parseLine");
        free(line);
        free(argv);
        return SHELL_CONTINUE;
    }

    // execute commands
    if (strcmp(argv[0], "exit") == 0 && argv[1] == NULL) { // if call "exit" with no args
        free(line);
        free(argv);
        puts("goodbye\n");
        return SHELL_BREAK;
    } 
    else if (builtInCalled(argv)) {} // try to call built-in
    else if (pipeCalled(argv)) {} // try to call multiple commands with pipes
    else callFromPATH(argv); // try to execute a single comand

    // clean up
    free(line);
    free(argv);
    return SHELL_CONTINUE;
}

/* print shell prompt */
void printPrompt(void) {
    char *username = getenv("USER");
    char *cwd = getCWD();

    printf("\033[0;31m%s\e[0m@\e[1;34m%s\e[0m> ", username, cwd);

    free(cwd);
}

/* returns current working directory, NULL if not found 
   caller's responsibilty to free cwd
   helper function for printPrompt() and pwd() */
char *getCWD() {
    char *cwd = (char *)malloc(sizeof(char) * PATH_MAX);
    if (cwd == NULL) { 
        perror("getCWD malloc");
        free(cwd);
        return NULL;
    }

    getcwd(cwd, PATH_MAX);
    if (cwd == NULL) {
        perror("cwd");
        return NULL;
    }

    return cwd;
}

/* returns string of user's input from terminal */
char *getInput(void) {
    char *line = malloc(PATH_MAX); // freed in shellLoop()
    if (fgets(line, PATH_MAX, stdin) == NULL) {
        perror("fgets");
        free(line);
        return NULL;
    }

    return line;
}

/* tokenizes line and returns array of string */
char **parseLine(char *line) {
    line[strcspn(line, "\n")] = '\0'; // strip newline character
    return parse(line, " "); // freed in shellLoop()
}

/* checks if argv contains built-in function and calls the respective function if so
   returns 1 if built in function called, 0 otherwise */
bool builtInCalled(char **argv) {
    if (strcmp(argv[0], "cd") == 0) {
        cd(argv[1]);     
        return true;
    } 
    
    if (strcmp(argv[0], "help") == 0) {
        help();
        return true;
    } 
    
    if (strcmp(argv[0], "pwd") == 0) {
        pwd();
        return true;
    }

    if (strcmp(argv[0], "wait") == 0) {
        shellWait();
        return true;
    }   

    return false; // not built in
}

/* forks process and executes command in argv
   will check if commanded to run in background and will do so if so
   returns true if called, 0 if not found */
bool callFromPATH(char **argv) {
    bool inBackground = false;

    if (strcmp(argv[getNumArgs(argv) - 1], "&") == 0) {
        inBackground = true;
        backgroundProcesses++;
        argv[getNumArgs(argv) - 1] = NULL;
    }

    int pid = fork();
    if (pid < 0) { // failed
        perror("callFromPATH fork");
        return false; // not run
    } 
    
    if (pid == 0) // child
        execCommand(argv);
    
    // parent
    if (inBackground)
        printf("process %d in background\n", pid); // let handleSIGCHLD do its job, no need to wait
    else
        wait(NULL);
    
    return true;
}

/**returns size of argv */
size_t getNumArgs(char **argv) {
    size_t numArgs = 0;

    while (argv[numArgs] != NULL)
        numArgs++;

    return numArgs;
}

/* checks if user commanded more than one pipe and executes it
   returns true if pipes executed or false if not called or failed  */
bool pipeCalled(char **argv) {
    if (!hasPipe(argv))
        return false;

    char ***argvs = splitArgv(argv);
    if (argvs == NULL) {
        perror("splitArgv");
        return false; // fail, not run
    }

    int numArgv = 0;
    while (argvs[numArgv] != NULL) numArgv++;

    int pipefds[2 * (numArgv - 1)];  // needs numCommands - 1 pipes

    // create pipes
    for (int i = 0; i < numArgv - 1; i++) {
        if (pipe(pipefds + 2 * i) == -1) { // pipefds[0] = in of pipe1, pipefds[1] = out of pipe 1, pipefds[2] = in of pipe2, etc
            perror("pipe");
            return false; // fail, not run
        }
    }

    // fork for each command
    for (int i = 0; i < numArgv; i++) {
        int pid = fork();
        if (pid < 0) {
            perror("fork");
            return false; // fail, not run
        }

        if (pid == 0) {  // child
            if (i > 0)  // every command except first gets in piped
                dup2(pipefds[2 * (i - 1)], STDIN_FILENO);
            
            if (i < numArgv - 1)  // every command except last gets out piped
                dup2(pipefds[2 * i + 1], STDOUT_FILENO);
            

            // close all pipefds
            for (int j = 0; j < 2 * (numArgv - 1); j++)
                close(pipefds[j]);

            execCommand(argvs[i]);
        }
    }

    // parent
    // close all pipes
    for (int i = 0; i < 2 * (numArgv - 1); i++)
        close(pipefds[i]);
    

    // wait for all cmds to complete
    for (int i = 0; i < numArgv; i++)
        wait(NULL);
    
    free(argvs);

    return true;
}


/* slits argv into array of argvs for use with pipes */
char ***splitArgv(char **argv) {
    // count number of separate argvs
    int numArgvs = 1;
    for (int i = 0; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "|") == 0)
            numArgvs++;
    }

    // alloc for num of seperate commands  +1 for NULL term
    char ***argvs = malloc((numArgvs + 1) * sizeof(char **));
    if (argvs == NULL) {
        perror("malloc splitArgv");
        return NULL;
    }

    int cmdIndex = 0;  // index of current argv in the argvs array
    int argIndex = 0;  // index of current arg in the current argv

    argvs[cmdIndex] = malloc(PATH_MAX * sizeof(char *));
    if (argvs[cmdIndex] == NULL) {
        perror("malloc command array");
        return NULL;
    }

    // loop and split
    for (int i = 0; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "|") == 0) { // if end of current argv
            // null terminate current argv
            argvs[cmdIndex][argIndex] = NULL;
            cmdIndex++;  // move to next argv

            // alloc next argv
            argvs[cmdIndex] = malloc(PATH_MAX * sizeof(char *));
            if (argvs[cmdIndex] == NULL) {
                perror("malloc command array");
                return NULL;
            }

            argIndex = 0;  // reset for next argv
        } else // copy
            argvs[cmdIndex][argIndex++] = argv[i];
    }

    // null term last argv
    argvs[cmdIndex][argIndex] = NULL;

    // mark end of array of argvs
    argvs[cmdIndex + 1] = NULL;

    return argvs;
}

/* returns true if argv contains "|" otherewise false */
bool hasPipe(char **argv) {
    for (int i = 1; argv[i] != NULL; i++)
        if (strcmp(argv[i], "|") == 0) 
            return true; // found

    return false; // not found
}

/* resolves path of external command, redirects it, and executes it */
void execCommand(char **argv) {
    // path is /absolute/path/to/command or relative/path
    char *path = resolvePath(argv[0]);
    if (path == NULL) {
        perror("resolvePath");
        free(path);
        exit(EXIT_FAILURE); // not run
    }

    // check for redirection ("<" and ">") and set IO accordingly
    redirect(argv);

    // call command
    execv(path, argv);

    // past this point execv has errored out
    perror("execv");
    free(path);
    exit(EXIT_FAILURE);
}

/* returns string of resolved path if resolved, NULL otherwise */
char *resolvePath(char *command) {
    // resolve relative paths, like "./command"
    if (strchr(command, '/') != NULL) // if command contains '/'
        return strdup(command);

    char path[PATH_MAX];
    for (int i = 0; PATH_arr[i] != NULL; i++) {
        snprintf(path, sizeof(path), "%s/%s", PATH_arr[i], command);
        
        if (access(path, X_OK) == 0) // check if executable exists
            return strdup(path);
    }

    return NULL; // not found
}

/* checks for "<" and ">" in argv for redirecting inptut/output and sets it accordingly */
void redirect(char **argv) {
    for (size_t i = 1; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "<") == 0 && argv[i+1] != NULL) {
            setNewIn(argv[i+1]);
            resizeArgv(argv, i);
            i--; // readjust i from resizeArgv()
        }
        
        if (strcmp(argv[i], ">") == 0 && argv[i+1] != NULL) {
            setNewOut(argv[i+1]);
            resizeArgv(argv, i);
            i--; // readjust i from resizeArgv()
        }
    }
}

/* sets input stream to path */
void setNewIn(char *path) {
    int newin = open(path, O_RDONLY);
    if (newin < 0) {
        perror("open in");
        exit(EXIT_FAILURE);
    }

    if (dup2(newin, STDIN_FILENO) < 0) {
        perror("dup2 in");
        close(newin);
        exit(EXIT_FAILURE);
    }

    close(newin);
}

/* sets output stream to path */
void setNewOut(char *path) {
    int newout = open(path, O_CREAT | O_WRONLY, 0644);
    if (newout < 0) {
        perror("open out");
        exit(EXIT_FAILURE);
    }

    if (dup2(newout, STDOUT_FILENO) < 0) {
        perror("dup2 out");
        close(newout);
        exit(EXIT_FAILURE);
    }

    close(newout);
}

/* reduces argv's size by 2 at index
   call carefully to prevent segmentation fault */
void resizeArgv(char **argv, size_t index) {
    while (argv[index] != NULL) {
        argv[index] = argv[index + 2];
        index++;
    }
}

/* signal handler function for struct sa in main()
   handles waiting for background processes */
void handleSIGCHLD(int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (backgroundProcesses > 0) {
            backgroundProcesses--;  // one less background process to worry about
            printf("\nprocess %d finished\n", pid);
            if (!isWaiting) printPrompt();
            fflush(stdout);
        } else break;
    }
}


/* BUILT-IN FUNCTIONS */

/* changes working directory to path */
void cd(char *path) {
    if (chdir(path) != 0) {
        perror("chdir");
        return;
    }
}

/* prints help.txt to terminal */
void help(void) {
    // print file
    int c;
    while ((c = fgetc(helpFile)) != EOF)
        printf("%c", c);
    
    // reset stream
    rewind(helpFile);
}

/* prints current working directory */
void pwd(void) {
    char *cwd;
    cwd = getCWD();

    printf("%s\n", cwd);

    free(cwd);
}

/* waits for all background processes to finish before returning control to user */
void shellWait() {
    if (backgroundProcesses == 0) {
        puts("no processes to wait on");
        return;
    }

    // block handleSIGCHILD from printing prompt
    isWaiting = true;

    printf("waiting on %lu process(es)...\n", backgroundProcesses);

    while (backgroundProcesses > 0) { /* let handleSIGCHLD() do its job */}

    // done waiting
    isWaiting = false;

    puts("\ndone.");
}
