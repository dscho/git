/*
 * test-simple-ipc.c: verify that the Inter-Process Communication works.
 */

#include "test-tool.h"
#include "cache.h"
#include "strbuf.h"
#include "simple-ipc.h"

#ifndef SUPPORTS_SIMPLE_IPC
int cmd__simple_ipc(int argc, const char **argv)
{
	die("simple IPC not available on this platform");
}
#else
static int test_handle_client(struct ipc_command_listener *listener,
			      const char *command,
			      int (*reply)(void *data,
					   const char *answer, size_t len),
			      void *reply_data)
{
	const char *answer, *unhandled = "unhandled command: ";

	if (!strcmp(command, "quit")) {
		return SIMPLE_IPC_QUIT;
	}

	if (!strcmp(command, "ping"))
		answer = "pong";
	else if (reply(reply_data, unhandled, strlen(unhandled)) < 0)
		return -1;
	else
		answer = command;

	return reply(reply_data, answer, strlen(answer));
}

int cmd__simple_ipc(int argc, const char **argv)
{
	const char *path = "ipc-test";
	int i;

	if (argc == 2 && !strcmp(argv[1], "SUPPORTS_SIMPLE_IPC"))
		return 0;

	if (argc == 2 && !strcmp(argv[1], "daemon")) {
		struct ipc_command_listener data = {
			.path = path,
			.handle_client = test_handle_client,
		};
		return !!ipc_listen_for_commands(&data);
	}

	if ((argc == 2 || argc == 3) && !strcmp(argv[1], "send")) {
		const char *command = argc > 2 ? argv[2] : "(no command)";
		struct strbuf buf = STRBUF_INIT;

		/*
		 * The test script will start the daemon in the background,
		 * meaning that it might not be fully set up by the time we are
		 * trying to send a message to it. So let's wait up to 2.5
		 * seconds, in 50ms increments, to send it.
		 */
		for (i = 0; i < 50; i++)
			if (!ipc_send_command(path, command, &buf)) {
				printf("%s\n", buf.buf);
				strbuf_release(&buf);

				return 0;
			} else if (!strcmp("quit", command) &&
				   (errno == ENOENT || errno == ECONNRESET))
				return 0;
			else if (errno != ENOENT)
				die_errno("failed to send '%s' to '%s'",
					  command, path);
			else
				sleep_millisec(50);

		die("noone home at '%s'?", path);
	}

	die("Unhandled argv[1]: '%s'", argv[1]);
}
#endif
