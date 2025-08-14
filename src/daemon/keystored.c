#include "keystored.h"

int keep_running = 1;

void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        syslog(LOG_INFO, "keystored::shutting down");
        keep_running = 0;
    }
}

int daemonize(void) {
    pid_t process_id, session_id;
    int rc = 0;

    //Create process
    process_id = fork();
    if (process_id < 0)
        return -1;

    //Exit Parent process
    if (process_id > 0)
        return 0;
    
    //Set new session
    session_id = setsid();
    if (session_id < 0)
        return -2;
    
    //Change directory
    rc = chdir("/");
    if (rc < 0)
        return -3;
    
    //Close file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return 0;
}

int main(){
	int rc;
	
	//setup logs
	openlog(DAEMON_NAME, LOG_PID|LOG_CONS, LOG_DAEMON);
    
    //Daemonize the process
    rc = daemonize();
    if (rc < 0) {
        syslog(LOG_ERR, "keystored::failed to daemonize");
        return 1;
    }
    syslog(LOG_INFO, "keystored::started");

	// Handle termination signals 
	signal(SIGTERM, handle_signal);
	signal(SIGINT, handle_signal);


	while (keep_running) {
		sleep(1);
	}

	closelog();
	return 0;
}