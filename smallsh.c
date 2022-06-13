// Requirement for snprintf()
#define __STDC_WANT_LIB_EXT1__  1

// Requirement for signal handling
#define _POSIX_C_SOURCE         200809L

// global values for max chars and max args that a command line can support
#define MAX_CHARS            2048
#define MAX_ARGS             512
#define MAX_CHILD_PROCESSES  20

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <stdint.h>
#include <err.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>


// Input struct to hold user input data
struct Input 
{
  char raw[MAX_CHARS+1];
  char command[MAX_CHARS+1];
  int argc;
  char* argv[MAX_ARGS+1];
  char inRedir[MAX_CHARS+1];
  char outRedir[MAX_CHARS+1];
  char inFile[MAX_CHARS+1];
  char outFile[MAX_CHARS+1];
  int isBackground;
};

// function declarations
void init_input_struct(struct Input *user_in);
char *getcwd_a(void);
void get_input(char *str_in);
void expand$$(char *str_in);
void tokenize(struct Input *user_in);
void exit_(int *running, int *bg_pids, int *bg_count);
void cd_(struct Input *user_in);
int status_(int *status);
void execute_(struct Input *user_in, struct sigaction *SIGINT_action, struct sigaction *SIGTSTP_action, int *bg_buf, int *bg_count, int *status);
void check_bg(int *bg_buf, int *bg_count, int *status);
void handle_SIGINT(int signo);
void handle_SIGTSTP(int signo);
void kill_all(int *bg_buf, int *bg_count);


// background flag toggle for signal handling
//   - 0 = Enter 'foreground mode only'
//   - 1 = Exit 'foreground mode only'
volatile sig_atomic_t TSTP_flag = 1;

// flag to check if last foreground process exited or was terminated by signal
//    - 0: exit value
//    - 1: terminated by flag
int is_SIG_flag = 0;

int
main(void) 
{
  // initialize structs: user_in, SIGINT_action, SIGTSTP_action 
  struct Input user_in;

  // SIGINT CTRL-C 
  struct sigaction SIGINT_action = {0};
  SIGINT_action.sa_handler = SIG_IGN;
  sigfillset(&SIGINT_action.sa_mask);
  SIGINT_action.sa_flags = 0;
  sigaction(SIGINT, &SIGINT_action, NULL); 

  // SIGTSTP CTRL-Z
  struct sigaction SIGTSTP_action = {0};
  SIGTSTP_action.sa_handler = handle_SIGTSTP;
  sigfillset(&SIGTSTP_action.sa_mask);
  SIGTSTP_action.sa_flags = 0;
  sigaction(SIGTSTP, &SIGTSTP_action, NULL);

  // will track the last foreground process exit value
  int status = 0;

  //background process buffer and count
  int bg_buf[MAX_CHILD_PROCESSES] = {0};
  int bg_count = 0;
  int i;
  int running = 1;

  while (running) {
    // clears all user_in buffers
    init_input_struct(&user_in);

    // check for bg processes
    check_bg(bg_buf, &bg_count, &status);

    fflush(stdout);
    get_input(user_in.raw);
    expand$$(user_in.raw);
    tokenize(&user_in);
    //for (int i = 0; i < user_in.argc; i++) printf("argv[%d] is %s\n", i, user_in.argv[i]);

    if (strcmp(user_in.command, "cd") == 0) {
      cd_(&user_in);
    } else if (strcmp(user_in.command, "status") == 0) {
      status_(&status);
    } else if (strcmp(user_in.command, "exit") == 0) {
      // clean up processes before exiting
      kill_all(bg_buf, &bg_count); 
      exit_(&running, bg_buf, &bg_count);
    } else {
      // command is non-built in so execute_()
      execute_(&user_in, &SIGINT_action, &SIGTSTP_action, bg_buf, &bg_count, &status);
    }
      // free any memory on heap
    for (i = 0; i < user_in.argc; i++) free(user_in.argv[i]);
  }
  return 0;
}



/*********************************************************
 *
 *  Kill all processes
 *
**********************************************************/
void kill_all(int *bg_buf, int *bg_count) 
{
  //printf("killing all processes, and exiting...");
  for (int i = 0; i < *bg_count; i++) 
  {
    kill(bg_buf[i], SIGTERM);
  }
}


/*********************************************************
 *
 *  SIGINT Handler
 *
**********************************************************/
void handle_SIGINT(int signo) 
{

}


/*********************************************************
 *
 *   SIGTSTP Handler
 *     - will flip the TSTP_flag to enable/disable 
 *       foreground only mode
 *
**********************************************************/
void handle_SIGTSTP(int signo) 
{
  if (TSTP_flag == 1) 
  { 
    char *message = "\nEntering foreground-only mode (& is now ignored)\n";
    write(STDOUT_FILENO, message, 50);
    TSTP_flag = 0;
  }

  else 
  {
    char *message = "\nExiting foreground-only mode\n";
    write(STDOUT_FILENO, message, 30);
    TSTP_flag = 1;
  }
}


/*********************************************************
 *  - utility function used to clear out the variables
 *    in the Input struct members
 *
**********************************************************/
void init_input_struct(struct Input *user_in) 
{
    //clear all user_input buffers
    user_in->argc = 0;
    user_in->isBackground = 0;
    memset(user_in->raw, '\0', sizeof(user_in->raw));
    memset(user_in->command, '\0', sizeof(user_in->command));
    memset(user_in->argv, '\0', sizeof(user_in->argv));
    memset(user_in->inRedir, '\0', sizeof(user_in->inRedir));
    memset(user_in->outRedir, '\0', sizeof(user_in->outRedir));
    memset(user_in->inFile, '\0', sizeof(user_in->inFile));
    memset(user_in->outFile, '\0', sizeof(user_in->outFile));
}


/*********************************************************
 *
 * Checks the list of background processes for termination
 *  and/or current statuses
 *
**********************************************************/
void
check_bg(int *bg_buf, int *bg_count, int  *status) 
{
  int child_status;
  for (int i = 0; i < *bg_count; i++) 
  {
    //printf("background pid is %d\n", bg_buf[i]);
    // check if child has terminated
    if (waitpid(bg_buf[i], &child_status, WNOHANG) > 0)
    {
      //printf("background pid is %d\n", bg_buf[i]);
      if (WIFEXITED(child_status) > 0) 
      {
        //printf("exit value %d\n", WEXITSTATUS(child_status));
        fflush(stdout);
        //bg_count -= 1;
        if (is_SIG_flag == 1) 
        {
          // fork and run ps
          int child_status;
          pid_t spawnpid = fork();
          switch(spawnpid)
          {
            case -1:
              perror("fork()\n");
              exit(1);
              break;
            case 0:
              execlp("ps", "ps", NULL);
              break;
            default:
              waitpid(spawnpid, &child_status, 0);
              printf("exit value %d\n", WEXITSTATUS(child_status));
              fflush(stdout);
              break;
          }
         }
      }
      if (WIFSIGNALED(child_status)) 
      {
        is_SIG_flag = 1;
        *status = WIFSIGNALED(child_status);
        printf("background with pid %d was terminated by signal %d\n", bg_buf[i], WTERMSIG(child_status));
        fflush(stdout);
      }
      bg_count -= 1;

    }
  }
}


/*********************************************************
 *
 * Execute the requested command 
 * Handle input/output redir, &.
 * /dev/null is default outFile and inFile if not specifed
 *  in the requested background process 
 * 
**********************************************************/
void
execute_(struct Input *user_in, struct sigaction *SIGINT_action, struct sigaction *SIGTSTP_action, int *bg_buf, int *bg_count, int *status)
{
  pid_t spawnpid;
  int child_status;
  pid_t child_pid;
  pid_t second_spawnpid;

  int targetFD = NULL;
  int sourceFD = NULL;
  int result;
  int exec_ret;
  
  // check for foreground only mode triggered by SIG_TSTP
  if (TSTP_flag == 0) user_in->isBackground = 0;
  
  spawnpid = fork();
  switch (spawnpid)
  {
    case -1:
      perror("fork() failed");
      fflush(stdout);
      exit(1);
      break;
    case 0:
      // SIGTSTP handling
      //   - Background child processes ignore received SIGTSTP 
      //   - Foreground child processes ignore received SIGTSTP 
      SIGTSTP_action->sa_handler = SIG_IGN;
      sigaction(SIGTSTP, SIGTSTP_action, NULL);

      // SIGINT Handling
      //   - Background child processes ignore received SIGINT
      //   - Foreground child processes terminate when SIGINT received
      (user_in->isBackground > 0) ? (SIGINT_action->sa_handler = SIG_IGN) : (SIGINT_action->sa_handler = SIG_DFL);
      sigaction(SIGINT, SIGINT_action, NULL);

      //pause();

      // OUTPUT REDIRECTION
      if (user_in->outRedir[0] == '>')
      {
        targetFD = open(user_in->outFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        fcntl(targetFD, F_SETFD, FD_CLOEXEC);
        if (targetFD == -1) 
        {
          perror("open() targetFD error");
          exit(1);
        }
        // redirect stdout to targetFD
        result = dup2(targetFD, 1);
        if (result == -1) 
        {
          perror("target dup2() error");
          exit(2);
        }
        // INPUT REDIRECTION
      } 
      if (user_in->inRedir[0] == '<') 
      {
        sourceFD = open(user_in->inFile, O_RDONLY);
        fcntl(sourceFD, F_SETFD, FD_CLOEXEC);

        if (sourceFD == -1) 
        {
          perror("open() sourceFD error");
          exit(2);
        }

        result = dup2(sourceFD, 0);
        if (result == -1) 
        {
          perror("source dup2() error)");
          exit(2);
        }
      }
      fflush(stdout);
      // if background and "<" specified, then default to /dev/null
      if (user_in->isBackground && user_in->inRedir[0] != '<') 
      {
        sourceFD = open("/dev/null", O_RDONLY);
        fcntl(sourceFD, F_SETFD, FD_CLOEXEC);

        if (sourceFD == -1) 
        {
          perror("open() sourceFD error");
          exit(2);
        }

        result = dup2(sourceFD, 0);
        if (result == -1) 
        {
          perror("source dup2() error)");
          exit(2);
        } 
      }
      fflush(stdout);
      // if background and no ">" specified, then default to /dev/null
      if(user_in->isBackground && user_in->outRedir[0] != '>') 
      {
        targetFD = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        fcntl(targetFD, F_SETFD, FD_CLOEXEC);

        if (targetFD == -1) 
        {
          perror("open() targetFD error");
          exit(1);
        }
        // redirect stdout to targetFD
        result = dup2(targetFD, 1);
        if (result == -1) 
        {
          perror("target dup2() error");
          exit(2);
        }
      }
      fflush(stdout);
      exec_ret = execvp(user_in->command, user_in->argv);
      perror("execvp() error");
      exit(1);
      break;
    default:
      if (user_in->isBackground > 0) 
      {
        printf("background pid is %d\n", getpid());
        fflush(stdout);
        child_pid = waitpid(spawnpid, &child_status, WNOHANG);
        bg_buf[*bg_count] = spawnpid;
        *bg_count += 1;
      
      // not a background process, parent will wait
      } else
      { 
        waitpid(spawnpid, &child_status, 0);
        *status = WEXITSTATUS(child_status);
        if (WIFEXITED(child_status) < 1) 
        {
          //status_(status);
          fflush(stdout);
        } 
        else if (WIFSIGNALED(child_status)) 
        {
          is_SIG_flag = 1;
          *status = WIFSIGNALED(child_status);
          printf("child was terminated by signal %d\n", WTERMSIG(child_status));
          fflush(stdout);
        }
      }
    // close source and target FD if created
    //if (sourceFD) close(sourceFD);
    //if (targetFD) close(targetFD);
  }
  //return;
}

int
status_(int *status) 
{
 // print out command of exit status of last foreground non-built in process
 if (is_SIG_flag == 0) 
 {
 printf("exit status %d\n", *status);
 } else 
 {
  printf("terminated by signal %d\n", *status);
 }
 is_SIG_flag = 0;
 fflush(stdout);
 return 0; 
}

void 
exit_(int *running, int *bg_pids, int *bg_count)
{
  *running = 0;
  return;
}


void
cd_(struct Input *user_in) 
{

  if (user_in->argc <= 1) 
  {
    chdir(getenv("HOME"));
  } else {
    // argv contains a path
    chdir(user_in->argv[1]);
  }

}

/*********************************************************
 *
 * the results will be stored in user_in -> argv
 * the count user_in -> argc is updated w/ each argv
 *
**********************************************************/
void
tokenize(struct Input *user_in)
{
  // make a copy to destroy during strtok() calls
  char str_copy[sizeof(user_in->raw)];
  strcpy(str_copy, user_in->raw);
  char *token;   
  token = strtok(str_copy, " ");
  // capture the command into the struct
  strcpy(user_in->command, token);
  int i = 0;
  while (token)
  { 
    // check for '<' and '>'
    if (strcmp(token, "<") == 0) 
    { 
      strcat(user_in->inRedir, token);
      token = strtok(NULL, " ");
      strcat(user_in->inFile, token);
      i++;
    } 
    else if (strcmp(token, ">") == 0) 
    { 
      strcat(user_in->outRedir, token);
      token = strtok(NULL, " ");
      strcat(user_in->outFile, token);
      token = strtok(NULL, " ");
      i++;
    } 
    else if (strcmp(token, "&") == 0) 
    {
      // check for background process request, only if '&' is the last character of the string
      char *pos = strstr(user_in->raw, token);
      if ((pos - user_in->raw) == (strlen(user_in->raw) - 1)) user_in->isBackground = WNOHANG;
      if ((pos - user_in->raw) != (strlen(user_in->raw) - 1)) 
      {
        user_in->argv[i] = calloc(MAX_CHARS, sizeof(*token));  
        strcpy(user_in->argv[i], token);
        i++;
      }
      token = strtok(NULL, " ");
      i++;
      
    } 
    else 
    { 
      if (!token) break;
      user_in->argv[i] = calloc(MAX_CHARS, sizeof(*token));    
      strcpy(user_in->argv[i], token);
      token = strtok(NULL, " ");
      i++;
    }
    user_in->argc++;
  }
}


/*********************************************************
 *
 * Request input from the user and store in user_in struct
 * specifically stores the string input in user_in -> raw
 *
*********************************************************/
void
get_input(char* user_in)
{
  char *temp;
  // get user input but ignore and as for re-input if "", "\n", and begins with "#"
  do {
  printf(": ");
  fflush(stdout);
  fgets(user_in, MAX_CHARS, stdin);
  } while (strncmp(user_in, "",1) == 0 || strncmp(user_in,"\n",1) == 0 || strncmp(user_in,"#",1) == 0);
  // remove new line character
  if (user_in[strlen(user_in) - 1] == '\n') user_in[strlen(user_in) - 1] = '\0';
}


/*********************************************************
 *
 * Replace instance of '$$' in the command. 
 *
**********************************************************/
void
expand$$(char *str_in)
{
  // create copy of string and zero out the original string
  char *str_copy = calloc(strlen(str_in), sizeof(char*));
  memset(str_copy, '\0', sizeof(str_copy));

  // get pid to use for $$ expansion
  pid_t pid = getpid();
  char *pidstr;
  int n = snprintf(NULL, 0, "%jd", pid);
  pidstr = malloc((n+1) * sizeof *pidstr);
  sprintf(pidstr, "%jd", pid);

  // copy str_copy into str_in and checking for string expansion
  char *in = str_in;
  char *out = str_copy;

  while (*in != '\0') 
  {
    if (*in == '$' && *(in+1) == '$') 
    {
      in += 2;
      strcat(out, pidstr);
      out += n;
    } else {
      *out = *in;
      in++;
      out++;
    }
  }
  strcpy(str_in, str_copy);
  free(str_copy);
  free(pidstr);
}

/*********************************************************
 *
 * allocates a string containing the CWD
 * taken from archive assignment 2
 *  * @return allocated string
 *  Use for debugging cd funciton
 *
**********************************************************/
 char *
 getcwd_a(void)
{
  char *pwd = NULL;
  for (size_t sz = 128;; sz *= 2)
    {
      pwd = realloc(pwd, sz);
      if (getcwd(pwd, sz)) break;
      if (errno != ERANGE) err(errno, "getcwd()");
    }
  return pwd;
}

