#include "cache.h"
#include "simple-ipc.h"
#include "strbuf.h"
#include "pkt-line.h"
#include "thread-utils.h"

#ifdef SUPPORTS_SIMPLE_IPC

int ipc_server_run(const char *path, const struct ipc_server_opts *opts,
		   ipc_server_application_cb *application_cb,
		   void *application_data)
{
	struct ipc_server_data *server_data = NULL;
	int ret;

	ret = ipc_server_run_async(&server_data, path, opts,
				   application_cb, application_data);
	if (ret)
		return ret;

	ret = ipc_server_await(server_data);

	ipc_server_free(server_data);

	return ret;
}

#endif /* SUPPORTS_SIMPLE_IPC */
