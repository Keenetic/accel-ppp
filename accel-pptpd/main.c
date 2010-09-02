#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "triton/triton.h"

static int goto_daemon;
static char *pid_file;
static char *conf_file;

static void sigterm(int num)
{
}

#define ARG_MAX 128
static int parse_cmdline(char ***argv)
{	
	FILE *f;
	int i;
	size_t len;

	f = fopen("/proc/self/cmdline", "r");
	if (!f) {
		perror("open cmdline");
		_exit(EXIT_FAILURE);
	}

	*argv = malloc(ARG_MAX * sizeof(void *));
	memset(*argv, 0, ARG_MAX * sizeof(void *));

	for(i = 0; i < ARG_MAX; i++)
	{
		len = 0;
		if (getdelim(&(*argv)[i], &len, 0, f) < 0)
			break;
	}

	return i;
}
static void __init __main(void)
{
	int i,argc;
	char **argv;

	argc=parse_cmdline(&argv);
	
	if (argc < 2)
		goto usage;

	for(i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d"))
			goto_daemon = 1;
		else if (!strcmp(argv[i], "-p")) {
			if (i == argc - 1)
				goto usage;
			pid_file = argv[++i];
		} else if (!strcmp(argv[i], "-c")) {
			if (i == argc - 1)
				goto usage;
			conf_file = argv[++i];
		}
	}

	if (!conf_file)
		goto usage;

	if (triton_init(conf_file))
		_exit(EXIT_FAILURE);

	return;

usage:
	printf("usage: pptpd [-d] [-p <file>] -c <file>\n\
	where:\n\
		-d - daemon mode\n\
		-p - write pid to <file>\n\
		-c - config file\n");
	_exit(EXIT_FAILURE);
}
int main(int argc, char **argv)
{
	sigset_t set;

	if (goto_daemon) {
		pid_t pid = fork();
		if (pid > 0)
			_exit(EXIT_SUCCESS);
		if (pid < 0) {
			perror("fork");
			return EXIT_FAILURE;
		}
		if (setsid() < 0)
			_exit(EXIT_FAILURE);
		pid = fork();
		if (pid)
			_exit(0);
		umask(0);
		chdir("/");
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	if (pid_file) {
		FILE *f = fopen("pid_file", "w");
		if (f) {
			fprintf(f, "%i", getpid());
			fclose(f);
		}
	}

	triton_run();
	
	signal(SIGTERM, sigterm);
	sigfillset(&set);
	sigdelset(&set, SIGTERM);
	sigdelset(&set, SIGSEGV);
	sigdelset(&set, SIGILL);
	sigdelset(&set, SIGFPE);
	sigdelset(&set, SIGBUS);

	sigsuspend(&set);
	
	triton_terminate();

	return EXIT_SUCCESS;
}

