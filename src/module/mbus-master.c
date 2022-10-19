/* M-Bus master module for Zephyr
 *
 * Copyright (C) 2022  Addiva Elektronik AB
 *
 * Author: Joachim Wiberg <joachim.wiberg@addiva.se>
 */
#define MODULE mbus

#include <zephyr/zephyr.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#if CONFIG_CAF
#include <caf/events/module_state_event.h>
#endif
#include <stdio.h>
#include "mbus/mbus.h"

#define shell_debug if (debug) shell_print
#define NELEMS(v)   (sizeof(v) / sizeof(v[0]))

LOG_MODULE_REGISTER(MODULE);

/*
 * Work queue for processing M-Bus commands when using nRF CAF
 */
#if CONFIG_CAF
#define K_WORK_MODULE_NAME &(struct k_work_queue_config){STRINGIFY(MODULE)}

static K_THREAD_STACK_DEFINE(wq_stack, 2048);
static struct k_work_q wq;

/* Unlikely we'll use need >5 args in this module */
struct shell_cmd {
    struct k_work       work;
    int               (*cb)(const struct shell *, int, char **);
    const struct shell *sh;
    int                 argc;
    char               *argv[5];
};
#endif /* CONFIG_CAF */

/*
 * Module runtime state variables
 */
static mbus_handle *handle;
static int          debug;
static int          verbose;
static int          xml;

static int init_slaves(const struct shell *shell)
{
    if (mbus_send_ping_frame(handle, MBUS_ADDRESS_NETWORK_LAYER, 1) == -1)
	goto fail;

    if (mbus_send_ping_frame(handle, MBUS_ADDRESS_BROADCAST_NOREPLY, 1) == -1)
	goto fail;

    return 0;
fail:
    shell_error(shell, "Failed initializing M-Bus slaves (SND_NKE)");
    return -1;
}

static int secondary_select(const struct shell *shell, char *mask)
{
    shell_debug(shell, "sending secondary select for mask %s", mask);

    switch (mbus_select_secondary_address(handle, mask)) {
    case MBUS_PROBE_COLLISION:
	shell_warn(shell, "address mask [%s] matches more than one device.", mask);
	return -1;
    case MBUS_PROBE_NOTHING:
	shell_warn(shell, "address mask [%s] does not match any device.", mask);
	return -1;
    case MBUS_PROBE_ERROR:
	shell_error(shell, "failed selecting secondary address [%s].", mask);
	return -1;
    case MBUS_PROBE_SINGLE:
	shell_debug(shell, "address mask [%s] matches a single device.", mask);
	break;
    }

    return MBUS_ADDRESS_NETWORK_LAYER;
}

static int parse_addr(const struct shell *shell, char *args)
{
    int address;

    if (!args) {
	shell_print(shell, "missing required argument, address, can be primary or secondary.");
	return 1;
    }

    if (init_slaves(shell))
	return 1;

    if (mbus_is_secondary_address(args)) {
	if (secondary_select(shell, args) == -1)
	    return 1;
	address = MBUS_ADDRESS_NETWORK_LAYER;
    } else {
	address = atoi(args);
	if (address < 1 || address > 255) {
	    shell_print(shell, "invalid primary address %s.", args);
	    return 1;
	}
    }
    shell_debug(shell, "using primary address %d ...", address);

    return address;
}

static int query_device(const struct shell *shell, int argc, char *argv[])
{
    mbus_frame_data data;
    mbus_frame reply;
    char *addr_arg;
    int address;

    if (argc < 2) {
	shell_print(shell, "usage: request ADDR [ID]");
	return -1;
    }

    addr_arg = argv[1];
    address = parse_addr(shell, addr_arg);
    if (mbus_send_request_frame(handle, address) == -1) {
	shell_error(shell, "failed sending M-Bus request to %d.", address);
	return 1;
    }

    if (mbus_recv_frame(handle, &reply) != MBUS_RECV_RESULT_OK) {
	LOG_ERR("failed receiving M-Bus response from %d.", address);
	return 1;
    }

    if (argc < 3 && !verbose && !xml) {
	mbus_hex_dump("RAW:", (const char *)reply.data, reply.data_size);
	return 0;
    }

    if (mbus_frame_data_parse(&reply, &data) == -1) {
	shell_error(shell, "M-bus data parse error: %s", mbus_error_str());
	return 1;
    }

    if (argc < 3) {
	/* Dump entire response as XML */
	if (xml) {
	    char *xml_data;

	    if (!(xml_data = mbus_frame_data_xml(&data))) {
		shell_error(shell, "failed generating XML output of M-BUS response: %s", mbus_error_str());
		return 1;
	    }

	    printf("%s", xml_data);
	    free(xml_data);
	} else {
	    mbus_frame_data_print(&data);
	}

	if (data.data_var.record)
	    mbus_data_record_free(data.data_var.record);
    } else {
	int record_id;

	/* Query for a single record */
	record_id = atoi(argv[2]);

	if (data.type == MBUS_DATA_TYPE_FIXED) {
	    /* TODO: Implement this -- Not fixed in BCT --Jachim */
	}
	if (data.type == MBUS_DATA_TYPE_VARIABLE) {
	    mbus_data_record *entry;
	    mbus_record *record;
	    int i;

	    for (entry = data.data_var.record, i = 0; entry; entry = entry->next, i++) {
		shell_debug(shell, "record ID %d DIF %02x VID %02x", i,
			    entry->drh.dib.dif & MBUS_DATA_RECORD_DIF_MASK_DATA,
			    entry->drh.vib.vif & MBUS_DIB_VIF_WITHOUT_EXTENSION);
	    }

	    for (entry = data.data_var.record, i = 0; entry && i < record_id; entry = entry->next, i++)
		;

	    if (i != record_id) {
		if (data.data_var.record)
		    mbus_data_record_free(data.data_var.record);

		mbus_frame_free(reply.next);
		return 1;
	    }

	    record = mbus_parse_variable_record(entry);
	    if (record) {
		if (record->is_numeric)
		    printf("%lf", record->value.real_val);
		else
		    printf("%s", record->value.str_val.value);
		if (verbose)
		    printf(" %s\n", record->unit);
		else
		    printf("\n");
	    }

	    if (data.data_var.record)
		mbus_data_record_free(data.data_var.record);
	}
	mbus_frame_free(reply.next);
    }

    return 0;
}

static int ping_address(const struct shell *shell, mbus_frame *reply, int address)
{
    int i, rc = MBUS_RECV_RESULT_ERROR;

    memset(reply, 0, sizeof(mbus_frame));

    for (i = 0; i <= handle->max_search_retry; i++) {
	shell_debug(shell, "%d ", address);

	if (mbus_send_ping_frame(handle, address, 0) == -1) {
	    shell_warn(shell, "scan failed sending ping frame: %s", mbus_error_str());
	    return MBUS_RECV_RESULT_ERROR;
	}

	rc = mbus_recv_frame(handle, reply);
	if (rc != MBUS_RECV_RESULT_TIMEOUT)
	    return rc;
    }

    return rc;
}

static int mbus_scan_1st_address_range(const struct shell *shell)
{
    int address;
    int rc = 1;

    for (address = 0; address <= MBUS_MAX_PRIMARY_SLAVES; address++) {
	mbus_frame reply;
	int rc;

	rc = ping_address(shell, &reply, address);
	if (rc == MBUS_RECV_RESULT_TIMEOUT)
	    continue;

	if (rc == MBUS_RECV_RESULT_INVALID) {
	    mbus_purge_frames(handle);
	    shell_warn(shell, "collision at address %d.", address);
	    continue;
	}

	if (mbus_frame_type(&reply) == MBUS_FRAME_TYPE_ACK) {
	    if (mbus_purge_frames(handle)) {
		shell_warn(shell, "collision at address %d.", address);
		continue;
	    }

	    shell_print(shell, "found an M-Bus device at address %d.", address);
	    rc = 0;
	}
    }

    return rc;
}

static int scan_devices(const struct shell *shell, int argc, char *argv[])
{
    if (init_slaves(shell))
	return -1;

    return mbus_scan_1st_address_range(shell);
}

static int probe_devices(const struct shell *shell, int argc, char *argv[])
{
    char addr_mask[20] = "FFFFFFFFFFFFFFFF";

    shell_print(shell, "Probing secondary addresses ...");
    if (mbus_is_secondary_address(addr_mask) == 0) {
        shell_error(shell, "malformed secondary address mask. Must be 16 character HEX number.");
        return -1;
    }

    if (init_slaves(shell))
        return -1;

    return mbus_scan_2nd_address_range(handle, 0, addr_mask);
}

static int set_address(const struct shell *shell, int argc, char *argv[])
{
    mbus_frame reply;
    int curr, next;
    char *mask;

    mask = argv[1];
    if (!mbus_is_secondary_address(mask)) {
	curr = atoi(mask);
	if (curr < 0 || curr > 250) {
	    shell_warn(shell, "invalid secondary address [%s], also not a primary address (0-250).", argv[1]);
	    return 1;
	}
    } else {
	curr = MBUS_ADDRESS_NETWORK_LAYER;
    }

    next = atoi(argv[2]);
    if (next < 1 || next > 250) {
	shell_warn(shell, "invalid new primary address [%s], allowed 1-250.", argv[2]);
	return 1;
    }

    if (init_slaves(shell))
	return 1;

    if (mbus_send_ping_frame(handle, next, 0) == -1) {
	shell_warn(shell, "failed sending verification ping: %s", mbus_error_str());
	return 1;
    }

    if (mbus_recv_frame(handle, &reply) != MBUS_RECV_RESULT_TIMEOUT) {
	shell_warn(shell, "verification failed, primary address [%d] already in use.", next);
	return 1;
    }

    if (curr == MBUS_ADDRESS_NETWORK_LAYER) {
	if (secondary_select(shell, mask) == -1)
	    return 1;
    }

    for (int retries = 3; retries > 0; retries--) {
	if (mbus_set_primary_address(handle, curr, next) == -1) {
	    shell_warn(shell, "failed setting device [%s] primary address: %s", mask, mbus_error_str());
	    return 1;
	}

	if (mbus_recv_frame(handle, &reply) == MBUS_RECV_RESULT_TIMEOUT) {
	    if (retries > 1)
		continue;

	    shell_warn(shell, "No reply from device [%s].", mask);
	    return 1;
	}
	break;
    }

    if (mbus_frame_type(&reply) != MBUS_FRAME_TYPE_ACK) {
	shell_warn(shell, "invalid response from device [%s], exected ACK, got:", mask);
	mbus_frame_print(&reply);
	return 1;
    }

    shell_debug(shell, "primary address of device %s set to %d", mask, next);

    return 0;
}

#define ENABLED(v) v ? "enabled" : "disabled"

static int toggle_debug(const struct shell *shell, int argc, char *argv[])
{
    debug ^= 1;

    shell_print(shell, "M-Bus debugging %s.", ENABLED(debug));
    mbus_parse_set_debug(debug);
    if (debug) {
        mbus_register_send_event(handle, mbus_dump_send_event);
        mbus_register_recv_event(handle, mbus_dump_recv_event);
    } else {
        mbus_register_send_event(handle, NULL);
        mbus_register_recv_event(handle, NULL);
    }

    return 0;
}

static int toggle_verbose(const struct shell *shell, int argc, char *argv[])
{
	verbose ^= 1;
	shell_print(shell, "verbose output %s", ENABLED(verbose));
	return 0;
}

static int toggle_xml(const struct shell *shell, int argc, char *argv[])
{
	xml ^= 1;
	shell_print(shell, "XML output %s", ENABLED(xml));
	return 0;
}

#if CONFIG_CAF
#define sh_set_address    set_address
#define sh_scan_devices   scan_devices
#define sh_probe_devices  probe_devices
#define sh_query_device   query_device
#define sh_toggle_debug   toggle_debug
#define sh_toggle_verbose toggle_verbose
#define sh_toggle_xml     toggle_xml
#else

static void mbus_cmd(struct k_work *work)
{
    struct shell_cmd *cmd = CONTAINER_OF(work, struct shell_cmd, work);

    if (cmd->cb(cmd->sh, cmd->argc, cmd->argv))
        LOG_ERR("failed M-Bus command");
}

struct shell_cmd cmd = {
    .work = Z_WORK_INITIALIZER(mbus_cmd)
};

static int work_submit(const struct shell *sh, int argc, char *argv[], int (*cb)(const struct shell *, int, char **))
{
    if (argc >= NELEMS(cmd.argv)) {
        LOG_ERR("too many shell arguments to worker");
        return 1;
    }

    cmd.cb = cb;
    cmd.sh = sh;
    cmd.argc = argc;
    for (int i = 0; i < NELEMS(cmd.argv); i++)
        cmd.argv[i] = argv[i];

    return k_work_submit_to_queue(&wq, &cmd.work);
}

#define fnwrap(fn) static int _CONCAT(sh_,fn)(const struct shell *s, int c, char *v[]) { return work_submit(s, c, v, fn); }
fnwrap(set_address)
fnwrap(scan_devices)
fnwrap(probe_devices)
fnwrap(query_device)
fnwrap(toggle_debug)
fnwrap(toggle_verbose)
fnwrap(toggle_xml)
#endif /* CONFIG_CAF */

SHELL_STATIC_SUBCMD_SET_CREATE(module_shell,
    SHELL_CMD_ARG(address, NULL, "Set primary address from secondary (mask) or current primary address.\nUsage: address <MASK | ADDR> NEW_ADDR", sh_set_address, 3, 0),
    SHELL_CMD_ARG(scan,    NULL, "Primary addresses scan", sh_scan_devices, 0, 0),
    SHELL_CMD_ARG(probe,   NULL, "Secondary addresses scan", sh_probe_devices, 0, 0),
    SHELL_CMD_ARG(request, NULL, "Request data, full XML or single record.\nUsage: request <MASK | ADDR> [RECORD ID]", sh_query_device, 2, 1),
    SHELL_CMD_ARG(debug,   NULL, "Toggle debug mode", sh_toggle_debug, 0, 0),
    SHELL_CMD_ARG(verbose, NULL, "Toggle verbose output (where applicable)", sh_toggle_verbose, 0, 0),
    SHELL_CMD_ARG(xml,     NULL, "Toggle XML output", sh_toggle_xml, 0, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(MODULE, &module_shell, "M-Bus commands", NULL);

int mbus_init(void)
{
    const char *port = "mbus0";

    if ((handle = mbus_context_serial(port)) == NULL) {
        LOG_ERR("failed initializing M-Bus context: %s",  mbus_error_str());
        return 1;
    }

    if (mbus_connect(handle) == -1) {
        LOG_ERR("failed connecting to serial port %s", port);
	return 1;
    }

#if CONFIG_CAF
    /* https://docs.zephyrproject.org/1.14.0/reference/kconfig/CONFIG_SYSTEM_WORKQUEUE_PRIORITY.html */
    k_work_queue_start(&wq, wq_stack, K_THREAD_STACK_SIZEOF(wq_stack),
                       CONFIG_SYSTEM_WORKQUEUE_PRIORITY, K_WORK_MODULE_NAME);
    module_set_state(MODULE_STATE_READY);
#endif

    return 0;
}

int mbus_exit(void)
{
    if (!handle)
        return 1;

    mbus_disconnect(handle);
    mbus_context_free(handle);
    handle = NULL;

    return 0;
}

#if CONFIG_CAF
static bool handle_module_event(const struct module_state_event *evt)
{
    if (check_state(evt, MODULE_ID(main), MODULE_STATE_READY)) {
        LOG_DBG("Initializing M-Bus module ...");
        mbus_init();
        return true;
    }

    return false;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
    if (is_module_state_event(aeh))
        return handle_module_event(cast_module_state_event(aeh));

    __ASSERT_NO_MSG(false);

    return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, module_state_event);
#endif /* CONFIG_CAF */

/**
 * Local Variables:
 *  indent-tabs-mode: nil
 *  c-file-style: "stroustrup"
 * End:
 */
