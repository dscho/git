#include "cache.h"
#include "simple-ipc.h"
#include "strbuf.h"
#include "pkt-line.h"

#ifdef GIT_WINDOWS_NATIVE
static int initialize_pipe_name(const char *path, wchar_t *wpath, size_t alloc)
{
	int off = 0;
	struct strbuf realpath = STRBUF_INIT;

	if (!strbuf_realpath(&realpath, path, 0))
		return error(_("could not normalize '%s'"), path);

	off = swprintf(wpath, alloc, L"\\\\.\\pipe\\");
	if (xutftowcs(wpath + off, realpath.buf, alloc - off) < 0)
		return error(_("could not determine pipe path for '%s'"),
			     realpath.buf);

	/* Handle drive prefix */
	if (wpath[off] && wpath[off + 1] == L':') {
		wpath[off + 1] = L'_';
		off += 2;
	}

	for (; wpath[off]; off++)
		if (wpath[off] == L'/')
			wpath[off] = L'\\';

	strbuf_release(&realpath);
	return 0;
}

static int is_active(wchar_t *pipe_path)
{
	return WaitNamedPipeW(pipe_path, 1) ||
		GetLastError() != ERROR_FILE_NOT_FOUND;
}

int ipc_is_active(const char *path)
{
	wchar_t pipe_path[MAX_PATH];

	if (initialize_pipe_name(path, pipe_path, ARRAY_SIZE(pipe_path)) < 0)
		return 0;

	return is_active(pipe_path);
}

struct ipc_handle_client_data {
	struct ipc_command_listener *server;
	HANDLE pipe;
};

static int reply(void *reply_data, const char *response, size_t len)
{
	int fd = *(int *)reply_data;

	return write_packetized_from_buf(response, len, fd, 0);
}

static DWORD WINAPI ipc_handle_client(LPVOID param)
{
	struct ipc_handle_client_data *data = param;
	struct strbuf buf = STRBUF_INIT;
	HANDLE process = GetCurrentProcess(), handle;
	int ret = 0, fd;

	/*
	 * First duplicate the handle, then wrap it in a file descriptor, so
	 * that we can use pkt-line on it.
	 */
	if (!DuplicateHandle(process, data->pipe, process, &handle, 0, FALSE,
			     DUPLICATE_SAME_ACCESS)) {
		errno = err_win_to_posix(GetLastError());
		return -1;
	}

	fd = _open_osfhandle((intptr_t)handle, O_RDWR|O_BINARY);
	if (fd < 0) {
		errno = err_win_to_posix(GetLastError());
		CloseHandle(handle);
		return -1;
	}

	ret = read_packetized_to_strbuf(fd, &buf, PACKET_READ_NEVER_DIE);
	if (ret >= 0) {
		ret = data->server->handle_client(data->server,
						  buf.buf, reply, &fd);
		packet_flush_gently(fd);
		if (ret == SIMPLE_IPC_QUIT)
			data->server->active = 0;
	}
	strbuf_release(&buf);
	FlushFileBuffers(data->pipe);
	DisconnectNamedPipe(data->pipe);
	return ret;
}

int ipc_listen_for_commands(struct ipc_command_listener *server)
{
	struct ipc_handle_client_data data = { server };

	if (initialize_pipe_name(server->path, server->pipe_path,
				 ARRAY_SIZE(server->pipe_path)) < 0)
		return -1;

	if (is_active(server->pipe_path))
		return error(_("server already running at %s"), server->path);

	data.pipe = CreateNamedPipeW(server->pipe_path,
		PIPE_ACCESS_INBOUND | PIPE_ACCESS_OUTBOUND,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
		PIPE_UNLIMITED_INSTANCES, 1024, 1024, 0, NULL);
	if (data.pipe == INVALID_HANDLE_VALUE)
		return error(_("could not create pipe '%s'"),
				server->path);

	server->active = 1;
	while (server->active) {
		int ret;

		if (!ConnectNamedPipe(data.pipe, NULL) &&
		    GetLastError() != ERROR_PIPE_CONNECTED) {
			error(_("could not connect to client (%ld)"),
			      GetLastError());
			continue;
		}

		ret = ipc_handle_client(&data);
		if (ret == SIMPLE_IPC_QUIT)
			break;

		if (ret == -1)
			error("could not handle client");
	}

	CloseHandle(data.pipe);

	return 0;
}

int ipc_send_command(const char *path, const char *message, struct strbuf *answer)
{
	wchar_t wpath[MAX_PATH];
	HANDLE pipe = INVALID_HANDLE_VALUE;
	DWORD mode = PIPE_READMODE_BYTE;
	int ret = 0, fd = -1;

	trace2_region_enter("simple-ipc", "send", the_repository);
	trace2_data_string("simple-ipc", the_repository, "path", path);
	trace2_data_string("simple-ipc", the_repository, "message", message);

	if (initialize_pipe_name(path, wpath, ARRAY_SIZE(wpath)) < 0) {
		ret = -1;
		goto leave_send_command;
	}

	for (;;) {
		pipe = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
			0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetLastError() != ERROR_PIPE_BUSY) {
			ret = error(_("could not open %s (%ld)"),
				    path, GetLastError());
			goto leave_send_command;
		}

		if (!WaitNamedPipeW(wpath, 5000)) {
			ret = error(_("timed out: %s"), path);
			goto leave_send_command;
		}
	}

	if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
		ret = error(_("could not switch pipe to byte mode: %s"), path);
		goto leave_send_command;
	}

	fd = _open_osfhandle((intptr_t)pipe, O_RDWR|O_BINARY);
	if (fd < 0) {
		ret = -1;
		goto leave_send_command;
	}

	if (write_packetized_from_buf(message, strlen(message), fd, 1) < 0) {
		ret = error(_("could not send '%s' (%ld)"), message,
			    GetLastError());
		goto leave_send_command;
	}
	FlushFileBuffers(pipe);

	if (answer) {
		ret = read_packetized_to_strbuf(fd, answer,
						PACKET_READ_NEVER_DIE);
		if (ret < 0)
			error_errno(_("IPC read error"));
		else
			trace2_data_string("simple-ipc", the_repository,
					   "answer", answer->buf);
	}

leave_send_command:
	trace2_region_leave("simple-ipc", "send", the_repository);

	if (fd < 0)
		CloseHandle(pipe);
	else
		close(fd);
	return ret < 0 ? -1 : 0;
}
#elif !defined(NO_UNIX_SOCKETS)
#include "unix-socket.h"
#include "sigchain.h"

static const char *fsmonitor_listener_path;

static int is_active(const char *path)
{
	struct stat st;

	return !lstat(path, &st) && (st.st_mode & S_IFMT) == S_IFSOCK;
}

int ipc_is_active(const char *path)
{
	return is_active(path);
}

static void set_socket_blocking_flag(int fd, int make_nonblocking)
{
	int flags;

	flags = fcntl(fd, F_GETFL, NULL);

	if (flags < 0)
		die(_("fcntl failed"));

	if (make_nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) < 0)
		die(_("fcntl failed"));
}

static int reply(void *reply_data, const char *response, size_t len)
{
	int fd = *(int *)reply_data;

	return write_packetized_from_buf(response, len, fd, 0);
}

/* in ms */
#define LISTEN_TIMEOUT 50000
#define RESPONSE_TIMEOUT 1000

static void unlink_listener_path(void)
{
	if (fsmonitor_listener_path)
		unlink(fsmonitor_listener_path);
}

int ipc_listen_for_commands(struct ipc_command_listener *listener)
{
	int ret = 0, fd;

	if (is_active(listener->path))
		return error(_("server already running at %s"), listener->path);

	fd = unix_stream_listen(listener->path);
	if (fd < 0)
		return error_errno(_("could not set up socket for %s"),
				   listener->path);

	fsmonitor_listener_path = listener->path;
	atexit(unlink_listener_path);

	trace2_region_enter("simple-ipc", "listen", the_repository);
	while (1) {
		struct pollfd pollfd;
		int result, client_fd;
		struct strbuf buf = STRBUF_INIT;

		/* Wait for a request */
		pollfd.fd = fd;
		pollfd.events = POLLIN;
		result = poll(&pollfd, 1, LISTEN_TIMEOUT);
		if (result < 0) {
			if (errno == EINTR)
				/*
				 * This can lead to an overlong keepalive,
				 * but that is better than a premature exit.
				 */
				continue;
			return error_errno(_("poll() failed"));
		} else if (result == 0)
			/* timeout */
			continue;

		client_fd = accept(fd, NULL, NULL);
		if (client_fd < 0)
			/*
			 * An error here is unlikely -- it probably
			 * indicates that the connecting process has
			 * already dropped the connection.
			 */
			continue;

		/*
		 * Our connection to the client is blocking since a client
		 * can always be killed by SIGINT or similar.
		 */
		set_socket_blocking_flag(client_fd, 0);

		strbuf_reset(&buf);
		result = read_packetized_to_strbuf(client_fd, &buf,
						   PACKET_READ_NEVER_DIE);

		if (result > 0) {
			/* ensure string termination */
			ret = listener->handle_client(listener, buf.buf, reply,
						      &client_fd);
			packet_flush_gently(client_fd);
			if (ret == SIMPLE_IPC_QUIT) {
				close(client_fd);
				strbuf_release(&buf);
				break;
			}
		} else {
			/*
			 * No command from client.  Probably it's just a
			 * liveness check or client error.  Just close up.
			 */
		}
		close(client_fd);
	}

	close(fd);
	return ret == SIMPLE_IPC_QUIT ? 0 : ret;
}

int ipc_send_command(const char *path, const char *message,
		     struct strbuf *answer)
{
	int fd = unix_stream_connect(path);
	int ret = 0;

	trace2_region_enter("simple-ipc", "send", the_repository);
	trace2_data_string("simple-ipc", the_repository, "path", path);
	trace2_data_string("simple-ipc", the_repository, "message", message);

	sigchain_push(SIGPIPE, SIG_IGN);
	if (fd < 0 ||
	    write_packetized_from_buf(message, strlen(message), fd, 1) < 0)
		ret = -1;
	else if (answer) {
		if (read_packetized_to_strbuf(fd, answer,
					      PACKET_READ_NEVER_DIE) < 0)
			ret = error_errno(_("could not read packet from '%s'"),
					  path);
		else
			trace2_data_string("simple-ipc", the_repository,
					   "answer", answer->buf);
	}

	trace2_region_leave("simple-ipc", "send", the_repository);

	if (fd >= 0)
		close(fd);
	sigchain_pop(SIGPIPE);
	return ret;
}
#endif
