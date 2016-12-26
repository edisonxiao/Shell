#include "dsh.h"

void free_job(job_t* j);
void seize_tty(pid_t callingprocess_pgid); /* Grab control of the terminal for the calling process pgid.  */
void continue_job(job_t *j); /* resume a stopped job */
void spawn_job(job_t *j, bool fg); /* spawn a new job */
bool builtin_cmd(job_t *last_job, int argc, char **argv);  //built in functions

void append_active_job(job_t *j);
void delete_completed_jobs_from_active_jobs(void);
void print_job_status_message(job_t *j, char *status);
void try_io_redirection(process_t *p);

job_t *active_jobs = NULL;
bool jobs_added = false;

void append_active_job(job_t *j) {
    if (j == NULL) return;
    // If list is empty
    if (active_jobs == NULL) {
	active_jobs = j;
    } else {
	job_t *current = active_jobs;
	while (current->next) {
	    current = current->next;
	}
	current->next = j;
    }
}

void delete_completed_jobs_from_active_jobs(void) {
    if (active_jobs == NULL) return;

    job_t *current = active_jobs;
    while (current->next != NULL) {
	if (job_is_completed(current->next)) {
	    job_t *to_free = current->next;
	    current->next = to_free->next;
	    free_job(to_free);
	    continue;
	}
	current = current->next;
    }

    // Check the head of the list
    if (job_is_completed(active_jobs)) {
	job_t *to_free = active_jobs;
	active_jobs = active_jobs->next;
	free_job(to_free);
    }
}

void print_job_status_message(job_t *j, char *status) {
    fprintf(stderr, "%d(%s): %s\n", j->pgid, status, j->commandinfo);
}

void try_io_redirection(process_t *p) {
    if (p->ifile != NULL) {
	int fd = open(p->ifile, O_RDONLY);
	if (fd >= 0) {
	    dup2(fd, 0);
	    close(fd);
	} else {
	    perror("Failed to open file specified\n");
	}
    }
    if (p->ofile != NULL) {
	int fd = open(p->ofile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd >= 0) {
	    dup2(fd, 1);
	    close(fd);
	} else {
	    perror("Failed to write to file specified\n");
	}
    }
}

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p)
{
    if (j->pgid < 0) { /* first child: use its pid for job pgid */
	j->pgid = p->pid;
    }
    return(setpgid(p->pid,j->pgid));
}

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
void new_child(job_t *j, process_t *p, bool fg)
{
    /* establish a new process group, and put the child in
     * foreground if requested
     */

    /* Put the process into the process group and give the process
     * group the terminal, if appropriate.  This has to be done both by
     * the dsh and in the individual child processes because of
     * potential race conditions.  
     */
    p->pid = getpid();

    /* also establish child process group in child to avoid race (if parent has not done it yet). */
    set_child_pgid(j, p);

    if(fg) { // if fg is set
	seize_tty(j->pgid); // assign the terminal
	printf("Process %d is seizing the terminal\n", getpid());
    } 
	

    /* Set the handling for job control signals back to the default. */
    signal(SIGTTOU, SIG_DFL);
}

/* Spawning a process with job control. fg is true if the 
 * newly-created process is to be placed in the foreground. 
 * (This implicitly puts the calling process in the background, 
 * so watch out for tty I/O after doing this.) pgid is -1 to 
 * create a new job, in which case the returned pid is also the 
 * pgid of the new job.  Else pgid specifies an existing job's 
 * pgid: this feature is used to start the second or 
 * subsequent processes in a pipeline.
 * */

void spawn_job(job_t *j, bool fg) 
{
	pid_t pid;
	process_t *p;
	bool piping = false;
	int prev_pipe[2] = {0, 1};
	for(p = j->first_process; p; p = p->next) {
	    
	  if (builtin_cmd(j,p->argc,p->argv)){
	      return;	  
	  }

	  if (!jobs_added) {
	      append_active_job(j);
	      jobs_added = true;
	  }

	  // check if we have pipes
	  int pipeline[2];

	  if (piping || p->next != NULL) {
	      int return_code = pipe(pipeline);
	      if (return_code == -1) {		  
		  perror("Pipe failed\n");
	      }
	      piping = true;
	  }
	  
	  switch (pid = fork()) {

          case -1: /* fork failure */
            perror("fork");
            exit(EXIT_FAILURE);

          case 0: /* child process  */
	      p->pid = getpid();
	      new_child(j, p, fg);
	      try_io_redirection(p);
	      if (piping) {				
		  if (p == j->first_process) {
		      dup2(pipeline[1], 1);
		  } else if (p->next == NULL) {
		      close(pipeline[0]);
		      close(pipeline[1]);	
		      dup2(prev_pipe[0], 0);
		  } else {
		      dup2(prev_pipe[0], 0);
		      dup2(pipeline[1], 1);
		  }
	      }
	      /* fprintf(stderr, "pipeline[0] = %d\n", pipeline[0]); */
	      /* fprintf(stderr, "pipeline[1] = %d\n", pipeline[1]); */
	      /* fprintf(stderr, "prev[0] = %d\n", prev_pipe[0]); */
	      /* fprintf(stderr, "prev[1] = %d\n", prev_pipe[1]); */
	      
//	    print_job_status_message(j, "Launched");
//	    printf("pid: %d just got here! and my name is %s\n", getpid(), (p->argv)[0]);
	      execvp((p->argv)[0], p->argv);
	      perror("New child should have done an exec");
	      exit(EXIT_FAILURE);  /* NOT REACHED */
	      break;    /* NOT REACHED */

          default: /* parent */
	      /* establish child process group */	    
	      p->pid = pid;
	      set_child_pgid(j, p);
	      
	      if (!piping) {
		  int status;
		  waitpid(pid, &status, WUNTRACED);
		  if (WIFEXITED(status)) {
		      p->completed = true;
		  } else if (WIFSTOPPED(status)) {
		      p->stopped = true;
		      perror("Process stopped\n");
		  } else {
		      
		  }
	      } else {
		  prev_pipe[0] = pipeline[0];
		  prev_pipe[1] = pipeline[1];
	      }	
          }	  	 
	}


	
	 //YOUR CODE HERE?  Parent-side code for new job.
	 if (piping) { 
	     for (p = j->first_process; p; p = p->next) { 
	 	int status; 
	 	printf("SHELL STARTED WAITING ON PROCESS %d\n", p->pid); 
	 	waitpid(p->pid, &status, WUNTRACED); 
	 	printf("SHELL DONE WAITING ON PROCESS %d\n", p->pid); 
	 	if (WIFEXITED(status)) { 
	 	    p->completed = true; 
	 	} else if (WIFSTOPPED(status)) { 
	 	    p->stopped = true; 
	 	    perror("Process stopped\n"); 
	 	} else { 
	 	    //SOMETHING ELSE HAPPENED 
	 	    //NOOP FOR NOW 
	 	} 
	     } 
	 } 
	
	seize_tty(getpid()); // assign the terminal back to dsh
}



/* Sends SIGCONT signal to wake up the blocked job */
void continue_job(job_t *j) 
{
     if(kill(-j->pgid, SIGCONT) < 0)
          perror("kill(SIGCONT)");
}


/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 */
bool builtin_cmd(job_t *last_job, int argc, char **argv) 
{

	/* check whether the cmd is a built in command */

        if (!strcmp(argv[0], "quit")) {
            /* Your code here */
            exit(EXIT_SUCCESS);
	}
        else if (!strcmp("jobs", argv[0])) {
	    job_t *current = active_jobs;
	    while (current) {
		if (job_is_completed(current)) {
		    print_job_status_message(current, "Completed");
		} else if (job_is_stopped(current)) {
		    print_job_status_message(current, "Stopped");    	    
		} else {
		    print_job_status_message(current, "Running");
		}
		current = current->next;
	    }
	    delete_completed_jobs_from_active_jobs();
	    return true;
        }
	else if (!strcmp("cd", argv[0])) {
            /* Your code here */
	    return true;
        }
        else if (!strcmp("bg", argv[0])) {
            /* Your code here */
	    return true;
        }
        else if (!strcmp("fg", argv[0])) {
            /* Your code here */
	    return true;
        }
        return false;       /* not a builtin command */
}

/* Build prompt messaage */
char* promptmsg() 
{
    char prompt[50];
    /* Modify this to include pid */
    sprintf(prompt, "dsh-%d$ ", getpid());
    return prompt;
}

int main() 
{

	init_dsh();
	DEBUG("Successfully initialized\n");
	
	while(1) {
	    job_t *j = NULL;
	    if(!(j = readcmdline(promptmsg()))) {
		if (feof(stdin)) { /* End of file (ctrl-d) */
		    fflush(stdout);
		    printf("\n");
		    exit(EXIT_SUCCESS);
           	}
		continue; /* NOOP; user entered return or spaces with return */
	    }

	    /* Only for debugging purposes to show parser output; turn off in the final code */
	    // if(PRINT_INFO) print_job(j);

	    job_t *current_job = j;
	    while (current_job) {
		spawn_job(current_job, !current_job->bg);
		current_job = current_job->next;
	    }
	    jobs_added = false;
	}
}
