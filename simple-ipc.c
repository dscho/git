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

	if (answer)
		ret = read_packetized_to_strbuf(fd, answer,
						PACKET_READ_NEVER_DIE);
	trace2_data_string("simple-ipc", the_repository, "answer", answer->buf);

leave_send_command:
	trace2_region_leave("simple-ipc", "send", the_repository);

	if (fd < 0)
		CloseHandle(pipe);
	else
		close(fd);
	return ret < 0 ? -1 : 0;
}
#endif
