// 
// tsh - A tiny shell program with job control
// 
// <William Anderson>
//

using namespace std;

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string>
#include <iostream>
#include "globals.h"
#include "jobs.h"
#include "helper-routines.h"   

//
// Needed global variable definitions
//

static char prompt[] = "tsh> ";
int verbose = 0;
int nextjid = 1;

//
// You need to implement the functions eval, builtin_cmd, do_bgfg,
// waitfg, sigchld_handler, sigstp_handler, sigint_handler
//
// The code below provides the "prototypes" for those functions
// so that earlier code can refer to them. You need to fill in the
// function bodies below.
// 

void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

//
// main - The shell's main routine 
//
int main(int argc, char **argv) 
{
  int emit_prompt = 1; // emit prompt (default)

  //
  // Redirect stderr to stdout (so that driver will get all output
  // on the pipe connected to stdout)
  //
  dup2(1, 2);

  /* Parse the command line */
  char c;
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
    case 'h':             // print help message
      usage();
      break;
    case 'v':             // emit additional diagnostic info
      verbose = 1;
      break;
    case 'p':             // don't print a prompt
      emit_prompt = 0;  // handy for automatic testing
      break;
    default:
      usage();
    }
  }

  //
  // Install the signal handlers
  //

  //
  // These are the ones you will need to implement
  //
  Signal(SIGINT,  sigint_handler);   // ctrl-c
  Signal(SIGTSTP, sigtstp_handler);  // ctrl-z
  Signal(SIGCHLD, sigchld_handler);  // Terminated or stopped child

  //
  // This one provides a clean way to kill the shell
  //
  Signal(SIGQUIT, sigquit_handler); 

  //
  // Initialize the job list
  //
  initjobs(jobs);

  //
  // Execute the shell's read/eval loop
  //
  for(;;) {
    //
    // Read command line
    //
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }

    char cmdline[MAXLINE];

    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) {
      app_error("fgets error");
    }
    //
    // End of file? (did user type ctrl-d?)
    //
    if (feof(stdin)) {
      fflush(stdout);
      exit(0);
    }

    //
    // Evaluate command line
    //
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  } 

  exit(0); //control never reaches here
}
  
/////////////////////////////////////////////////////////////////////////////
//
// eval - Evaluate the command line that the user has just typed in
// 
// If the user has requested a built-in command (quit, jobs, bg or fg)
// then execute it immediately. Otherwise, fork a child process and
// run the job in the context of the child. If the job is running in
// the foreground, wait for it to terminate and then return.  Note:
// each child process must have a unique process group ID so that our
// background children don't receive SIGINT (SIGTSTP) from the kernel
// when we type ctrl-c (ctrl-z) at the keyboard.
//
void eval(char *cmdline) 
{
  /* Parse command line */
  //
  // The 'argv' vector is filled in by the parseline
  // routine below. It provides the arguments needed
  // for the execve() routine, which you'll need to
  // use below to launch a process.
  //
  char *argv[MAXARGS];
  sigset_t mask;
  //
  // The 'bg' variable is TRUE if the job should run
  // in background mode or FALSE if it should run in FG
  //
  int bg = parseline(cmdline, argv);
  sigemptyset(&mask);   
  sigaddset(&mask, SIGCHLD);
  
  pid_t pid;
  if (argv[0] == NULL)  
    return;   /* ignore empty lines */
  
  if(!builtin_cmd(argv)) {
      //if there is an argument that exists block the current child....
      sigprocmask(SIG_BLOCK, &mask, 0);
      if((pid = fork()) < 0) {
          printf("fork(): fucking error\n");
          return;
      }
      
      if(pid == 0) {
          setpgid(0,0);
          if (execvp(argv[0], argv) < 0) {
              printf("%s: Command not found\n", argv[0]);
              exit(0);
          }
      }
      if(bg == 1) {
          addjob(jobs, pid, BG, cmdline); 
          printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
      }
      else {
          addjob(jobs, pid, FG, cmdline);
      }
      sigprocmask(SIG_UNBLOCK, &mask, 0);//unblock child	
      waitfg(pid);
  }
  
  return;
}


/////////////////////////////////////////////////////////////////////////////
//
// builtin_cmd - If the user has typed a built-in command then execute
// it immediately. The command name would be in argv[0] and
// is a C string. We've cast this to a C++ string type to simplify
// string comparisons; however, the do_bgfg routine will need 
// to use the argv array as well to look for a job number.
//
int builtin_cmd(char **argv) 
{
  string cmd(argv[0]);
  if (!strcmp("quit", argv[0])) {
      int i;
      int stop = 0;
      
      for(i = 0; i < 16; i++) {
          if (jobs[i].state == ST)
              stop = 1;
      }
      
      if (stop) {
          printf("There are stopped jobs\n");
          return 1;
      } else {
          exit(0);
      } 
  }
  
  if (!strcmp("jobs", argv[0])) {
      listjobs(jobs);
      return 1;
  }
  
  if (!strcmp("fg", argv[0]) || !strcmp("bg", argv[0])) {
      do_bgfg(argv);
      return 1;
  }
    
  if (!strcmp(argv[0],"&"))
      return 1;

  return 0;     /* not a builtin command */
}

/////////////////////////////////////////////////////////////////////////////
//
// do_bgfg - Execute the builtin bg and fg commands
//
void do_bgfg(char **argv) 
{
  struct job_t *jobp=NULL;
    
  /* Ignore command if no argument */
  if (argv[1] == NULL) {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }
    
  /* Parse the required PID or %JID arg */
  if (isdigit(argv[1][0])) {
    pid_t pid = atoi(argv[1]);
    if (!(jobp = getjobpid(jobs, pid))) {
      printf("(%d): No such process\n", pid);
      return;
    }
  }
  else if (argv[1][0] == '%') {
    int jid = atoi(&argv[1][1]);
    if (!(jobp = getjobjid(jobs, jid))) {
      printf("%s: No such job\n", argv[1]);
      return;
    }
  }	    
  else {
    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
    return;
  }

  //
  // You need to complete rest. At this point,
  // the variable 'jobp' is the job pointer
  // for the job ID specified as an argument.
  //
  // Your actions will depend on the specified command
  // so we've converted argv[0] to a string (cmd) for
  // your benefit.
  //
  string cmd(argv[0]);
  if (jobp != NULL) {
      if (jobp->state == ST) {
          if (!strcmp(argv[0],"bg")) {
              printf("[%d] (%d) %s", jobp->jid, jobp->pid, jobp->cmdline);
              jobp->state = BG;
              kill(-jobp->pid, SIGCONT);   
          } 
          if(!strcmp(argv[0],"fg"))  {
            jobp->state = FG; //changes the state to fg
            kill(-jobp->pid, SIGCONT); 
            waitfg(jobp->pid); //we only need fg process run once
          }
      }
      if(jobp->state == BG) {
          if(!strcmp(argv[0],"fg"))  {
            jobp->state = FG; //changes the state to fg 
            waitfg(jobp->pid); //we only need fg process run once
          }
      }
  }
  return;
}

/////////////////////////////////////////////////////////////////////////////
//
// waitfg - Block until process pid is no longer the foreground process
//
void waitfg(pid_t pid)
{
    struct job_t *p = getjobpid(jobs, pid);
    while(p->state == FG){
        sleep(1);
    }
    return;
}

/////////////////////////////////////////////////////////////////////////////
//
// Signal handlers
//


/////////////////////////////////////////////////////////////////////////////
//
// sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
//     a child job terminates (becomes a zombie), or stops because it
//     received a SIGSTOP or SIGTSTP signal. The handler reaps all
//     available zombie children, but doesn't wait for any other
//     currently running children to terminate.  
//
void sigchld_handler(int sig) 
{
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if(WIFEXITED(status)) {//terminated normally
            deletejob(jobs, pid);
        }
        if(WIFSIGNALED(status)) {
            printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
            deletejob(jobs,pid);
        }
        if (WIFSTOPPED(status)) {
            getjobpid(jobs, pid)->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid),(int) pid, WSTOPSIG(status));
        }   
    }
    if(pid < 0 && errno != ECHILD) {
        printf("waitpid error: %s\n", strerror(errno));
    }
    return;
}

/////////////////////////////////////////////////////////////////////////////
//
// sigint_handler - The kernel sends a SIGINT to the shell whenver the
//    user types ctrl-c at the keyboard.  Catch it and send it along
//    to the foreground job.  
//
void sigint_handler(int sig) 
{
    pid_t pid = fgpid(jobs);
    
    if(fgpid(jobs) != 0)
        kill(-pid, SIGINT);
    
    return;
}

/////////////////////////////////////////////////////////////////////////////
//
// sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
//     the user types ctrl-z at the keyboard. Catch it and suspend the
//     foreground job by sending it a SIGTSTP.  
//
void sigtstp_handler(int sig) 
{
    
    int pid = fgpid(jobs);  
    if(fgpid(jobs) != 0)
        kill(-pid, SIGSTOP);
    
    return;
}

/*********************
 * End signal handlers
 *********************/


