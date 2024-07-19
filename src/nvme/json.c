// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libnvme.
 * Copyright (c) 2021 SUSE Software Solutions
 *
 * Authors: Hannes Reinecke <hare@suse.de>
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <json.h>

#include "cleanup.h"
#include "fabrics.h"
#include "log.h"
#include "private.h"
#include "linux.h"

#define JSON_UPDATE_INT_OPTION(c, k, a, o)				\
	if (!strcmp(# a, k ) && !c->a) c->a = json_object_get_int(o);
#define JSON_UPDATE_BOOL_OPTION(c, k, a, o)				\
	if (!strcmp(# a, k ) && !c->a) c->a = json_object_get_boolean(o);

static void json_import_nvme_tls_key(nvme_ctrl_t c, const char *keyring_str,
				     const char *encoded_key)
{
	struct nvme_fabrics_config *cfg = nvme_ctrl_get_config(c);
	const char *hostnqn = nvme_host_get_hostnqn(c->s->h);
	const char *subsysnqn = nvme_ctrl_get_subsysnqn(c);
	int key_len;
	unsigned int hmac;
	long key_id;
	_cleanup_free_ unsigned char *key_data = NULL;

	if (!hostnqn || !subsysnqn) {
		nvme_msg(NULL, LOG_ERR, "Invalid NQNs (%s, %s)\n",
			 hostnqn, subsysnqn);
		return;
	}
	key_data = nvme_import_tls_key(encoded_key, &key_len, &hmac);
	if (!key_data) {
		nvme_msg(NULL, LOG_ERR, "Failed to decode TLS Key '%s'\n",
			encoded_key);
		return;
	}
	key_id = nvme_insert_tls_key_versioned(keyring_str, "psk",
					       hostnqn, subsysnqn,
					       0, hmac, key_data, key_len);
	if (key_id <= 0)
		nvme_msg(NULL, LOG_ERR, "Failed to insert TLS KEY, error %d\n",
			 errno);
	else {
		cfg->tls_key = key_id;
		cfg->tls = true;
	}
}

static void json_export_nvme_tls_key(long keyring_id, long tls_key,
				     struct json_object *obj)
{
	int key_len;
	_cleanup_free_ unsigned char *key_data = NULL;

	key_data = nvme_read_key(keyring_id, tls_key, &key_len);
	if (key_data) {
		_cleanup_free_ char *tls_str = NULL;

		tls_str = nvme_export_tls_key(key_data, key_len);
		if (tls_str)
			json_object_object_add(obj, "tls_key",
					       json_object_new_string(tls_str));
	}
}

static void json_update_attributes(nvme_ctrl_t c,
				   struct json_object *ctrl_obj)
{
	struct nvme_fabrics_config *cfg = nvme_ctrl_get_config(c);
	const char *keyring_str = NULL, *encoded_key = NULL;

	json_object_object_foreach(ctrl_obj, key_str, val_obj) {
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       nr_io_queues, val_obj);
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       nr_write_queues, val_obj);
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       nr_poll_queues, val_obj);
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       queue_size, val_obj);
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       keep_alive_tmo, val_obj);
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       reconnect_delay, val_obj);
		if (!strcmp("ctrl_loss_tmo", key_str) &&
		    cfg->ctrl_loss_tmo != NVMF_DEF_CTRL_LOSS_TMO)
			cfg->ctrl_loss_tmo = json_object_get_int(val_obj);
		JSON_UPDATE_INT_OPTION(cfg, key_str,
				       fast_io_fail_tmo, val_obj);
		if (!strcmp("tos", key_str) && cfg->tos != -1)
			cfg->tos = json_object_get_int(val_obj);
		JSON_UPDATE_BOOL_OPTION(cfg, key_str,
					duplicate_connect, val_obj);
		JSON_UPDATE_BOOL_OPTION(cfg, key_str,
					disable_sqflow, val_obj);
		JSON_UPDATE_BOOL_OPTION(cfg, key_str,
					hdr_digest, val_obj);
		JSON_UPDATE_BOOL_OPTION(cfg, key_str,
					data_digest, val_obj);
		JSON_UPDATE_BOOL_OPTION(cfg, key_str,
					tls, val_obj);
		JSON_UPDATE_BOOL_OPTION(cfg, key_str,
					concat, val_obj);
		if (!strcmp("persistent", key_str) &&
		    !nvme_ctrl_is_persistent(c))
			nvme_ctrl_set_persistent(c, true);
		if (!strcmp("discovery", key_str) &&
		    !nvme_ctrl_is_discovery_ctrl(c))
			nvme_ctrl_set_discovery_ctrl(c, true);
		/*
		 * The JSON configuration holds the keyring description
		 * which needs to be converted into the keyring serial number.
		 */
		if (!strcmp("keyring", key_str) && cfg->keyring == 0) {
			long keyring;

			keyring_str = json_object_get_string(val_obj);
			keyring = nvme_lookup_keyring(keyring_str);
			if (keyring) {
				cfg->keyring = keyring;
				nvme_set_keyring(cfg->keyring);
			}
		}
		if (!strcmp("tls_key", key_str) && cfg->tls_key == 0)
			encoded_key = json_object_get_string(val_obj);
	}

	/*
	 * We might need the keyring information from the above loop,
	 * so we can only import the TLS key once all entries are
	 * processed.
	 */
	if (encoded_key)
		json_import_nvme_tls_key(c, keyring_str, encoded_key);
}

static void json_parse_port(nvme_subsystem_t s, struct json_object *port_obj)
{
	nvme_ctrl_t c;
	struct json_object *attr_obj;
	const char *transport, *traddr = NULL;
	const char *host_traddr = NULL, *host_iface = NULL, *trsvcid = NULL;

	attr_obj = json_object_object_get(port_obj, "transport");
	if (!attr_obj)
		return;
	transport = json_object_get_string(attr_obj);
	attr_obj = json_object_object_get(port_obj, "traddr");
	if (attr_obj)
		traddr = json_object_get_string(attr_obj);
	attr_obj = json_object_object_get(port_obj, "host_traddr");
	if (attr_obj)
		host_traddr = json_object_get_string(attr_obj);
	attr_obj = json_object_object_get(port_obj, "host_iface");
	if (attr_obj)
		host_iface = json_object_get_string(attr_obj);
	attr_obj = json_object_object_get(port_obj, "trsvcid");
	if (attr_obj)
		trsvcid = json_object_get_string(attr_obj);
	c = nvme_lookup_ctrl(s, transport, traddr, host_traddr,
			     host_iface, trsvcid, NULL);
	if (!c)
		return;
	json_update_attributes(c, port_obj);
	attr_obj = json_object_object_get(port_obj, "dhchap_key");
	if (attr_obj)
		nvme_ctrl_set_dhchap_host_key(c, json_object_get_string(attr_obj));
	attr_obj = json_object_object_get(port_obj, "dhchap_ctrl_key");
	if (attr_obj)
		nvme_ctrl_set_dhchap_key(c, json_object_get_string(attr_obj));
}

static void json_parse_subsys(nvme_host_t h, struct json_object *subsys_obj)
{
	struct json_object *nqn_obj, *app_obj, *port_array;
	nvme_subsystem_t s;
	const char *nqn;
	int p;

	nqn_obj = json_object_object_get(subsys_obj, "nqn");
	if (!nqn_obj)
		return;
	nqn = json_object_get_string(nqn_obj);
	s = nvme_lookup_subsystem(h, NULL, nqn);
	if (!s)
		return;
	app_obj = json_object_object_get(subsys_obj, "application");
	if (app_obj)
		nvme_subsystem_set_application(s, json_object_get_string(app_obj));

	port_array = json_object_object_get(subsys_obj, "ports");
	if (!port_array)
		return;
	for (p = 0; p < json_object_array_length(port_array); p++) {
		struct json_object *port_obj;

		port_obj = json_object_array_get_idx(port_array, p);
		if (port_obj)
			json_parse_port(s, port_obj);
	}
}

static void json_parse_host(nvme_root_t r, struct json_object *host_obj)
{
	struct json_object *attr_obj, *subsys_array, *subsys_obj;
	nvme_host_t h;
	const char *hostnqn, *hostid = NULL;
	int s;

	attr_obj = json_object_object_get(host_obj, "hostnqn");
	if (!attr_obj)
		return;
	hostnqn = json_object_get_string(attr_obj);
	attr_obj = json_object_object_get(host_obj, "hostid");
	if (attr_obj)
		hostid = json_object_get_string(attr_obj);
	h = nvme_lookup_host(r, hostnqn, hostid);
	attr_obj = json_object_object_get(host_obj, "dhchap_key");
	if (attr_obj)
		nvme_host_set_dhchap_key(h, json_object_get_string(attr_obj));
	attr_obj = json_object_object_get(host_obj, "hostsymname");
	if (attr_obj)
		nvme_host_set_hostsymname(h, json_object_get_string(attr_obj));
	attr_obj = json_object_object_get(host_obj, "persistent_discovery_ctrl");
	if (attr_obj)
		nvme_host_set_pdc_enabled(h, json_object_get_boolean(attr_obj));
	subsys_array = json_object_object_get(host_obj, "subsystems");
	if (!subsys_array)
		return;
	for (s = 0; s < json_object_array_length(subsys_array); s++) {
		subsys_obj = json_object_array_get_idx(subsys_array, s);
		if (subsys_obj)
			json_parse_subsys(h, subsys_obj);
	}
}

static DEFINE_CLEANUP_FUNC(cleanup_tokener, json_tokener *, json_tokener_free)
#define _cleanup_tokener_ __cleanup__(cleanup_tokener)

static struct json_object *parse_json(nvme_root_t r, int fd)
{
	char buf[JSON_FILE_BUF_SIZE];
	struct json_object *obj;
	char *str = NULL;
	_cleanup_tokener_ json_tokener *tok = NULL;
	int ret;
	_cleanup_free_ void *ptr = NULL;
	int len = 0;

	while ((ret = read(fd, buf, JSON_FILE_BUF_SIZE)) > 0) {
		str = realloc(ptr, len + ret);
		if (!str)
			return NULL;
		memcpy(&str[len], buf, ret);
		len += ret;
		ptr = str;
	}

	if (ret < 0 || !len)
		return NULL;

	tok = json_tokener_new_ex(JSON_TOKENER_DEFAULT_DEPTH);
	if (!tok)
		return NULL;

	/* Enforce correctly formatted JSON */
	tok->flags = JSON_TOKENER_STRICT;

	obj = json_tokener_parse_ex(tok, str, len);
	if (!obj)
		nvme_msg(r, LOG_DEBUG, "JSON parsing failed: %s\n",
			 json_util_get_last_err());

	return obj;
}

int json_read_config(nvme_root_t r, const char *config_file)
{
	struct json_object *json_root, *host_obj;
	int fd, h;

	fd = open(config_file, O_RDONLY);
	if (fd < 0) {
		nvme_msg(r, LOG_DEBUG, "Error opening %s, %s\n",
			 config_file, strerror(errno));
		return fd;
	}
	json_root = parse_json(r, fd);
	close(fd);
	if (!json_root) {
		errno = EPROTO;
		return -1;
	}
	if (!json_object_is_type(json_root, json_type_array)) {
		nvme_msg(r, LOG_DEBUG, "Wrong format, expected array\n");
		json_object_put(json_root);
		errno = EPROTO;
		return -1;
	}
	for (h = 0; h < json_object_array_length(json_root); h++) {
		host_obj = json_object_array_get_idx(json_root, h);
		if (host_obj)
			json_parse_host(r, host_obj);
	}
	json_object_put(json_root);
	return 0;
}

#define JSON_STRING_OPTION(c, p, o)					\
	if ((c)->o && strcmp((c)->o, "none"))				\
		json_object_object_add((p), # o ,			\
				       json_object_new_string((c)->o))
#define JSON_INT_OPTION(c, p, o, d)					\
	if ((c)->o != d)						\
		json_object_object_add((p), # o ,			\
				       json_object_new_int((c)->o))
#define JSON_BOOL_OPTION(c, p, o)					\
	if ((c)->o)							\
		json_object_object_add((p), # o ,			\
				       json_object_new_boolean((c)->o))

static void json_update_port(struct json_object *ctrl_array, nvme_ctrl_t c)
{
	struct nvme_fabrics_config *cfg = nvme_ctrl_get_config(c);
	struct json_object *port_obj = json_object_new_object();
	const char *transport, *value;

	transport = nvme_ctrl_get_transport(c);
	if (!strcmp(transport, "pcie"))
		return;

	json_object_object_add(port_obj, "transport",
			       json_object_new_string(transport));
	value = nvme_ctrl_get_traddr(c);
	if (value)
		json_object_object_add(port_obj, "traddr",
				       json_object_new_string(value));
	value = nvme_ctrl_get_host_traddr(c);
	if (value)
		json_object_object_add(port_obj, "host_traddr",
				       json_object_new_string(value));
	value = nvme_ctrl_get_host_iface(c);
	if (value)
		json_object_object_add(port_obj, "host_iface",
				       json_object_new_string(value));
	value = nvme_ctrl_get_trsvcid(c);
	if (value)
		json_object_object_add(port_obj, "trsvcid",
				       json_object_new_string(value));
	value = nvme_ctrl_get_dhchap_host_key(c);
	if (value)
		json_object_object_add(port_obj, "dhchap_key",
				       json_object_new_string(value));
	value = nvme_ctrl_get_dhchap_key(c);
	if (value)
		json_object_object_add(port_obj, "dhchap_ctrl_key",
				       json_object_new_string(value));
	JSON_INT_OPTION(cfg, port_obj, nr_io_queues, 0);
	JSON_INT_OPTION(cfg, port_obj, nr_write_queues, 0);
	JSON_INT_OPTION(cfg, port_obj, nr_poll_queues, 0);
	JSON_INT_OPTION(cfg, port_obj, queue_size, 0);
	JSON_INT_OPTION(cfg, port_obj, keep_alive_tmo, 0);
	JSON_INT_OPTION(cfg, port_obj, reconnect_delay, 0);
	if (strcmp(transport, "loop")) {
		JSON_INT_OPTION(cfg, port_obj, ctrl_loss_tmo,
				NVMF_DEF_CTRL_LOSS_TMO);
		JSON_INT_OPTION(cfg, port_obj, fast_io_fail_tmo, 0);
	}
	JSON_INT_OPTION(cfg, port_obj, tos, -1);
	JSON_BOOL_OPTION(cfg, port_obj, duplicate_connect);
	JSON_BOOL_OPTION(cfg, port_obj, disable_sqflow);
	JSON_BOOL_OPTION(cfg, port_obj, hdr_digest);
	JSON_BOOL_OPTION(cfg, port_obj, data_digest);
	JSON_BOOL_OPTION(cfg, port_obj, tls);
	JSON_BOOL_OPTION(cfg, port_obj, concat);
	if (nvme_ctrl_is_persistent(c))
		json_object_object_add(port_obj, "persistent",
				       json_object_new_boolean(true));
	if (nvme_ctrl_is_discovery_ctrl(c))
		json_object_object_add(port_obj, "discovery",
				       json_object_new_boolean(true));
	/*
	 * Store the keyring description in the JSON config file.
	 */
	if (cfg->keyring) {
		_cleanup_free_ char *desc =
			nvme_describe_key_serial(cfg->keyring);

		if (desc) {
			json_object_object_add(port_obj, "keyring",
					       json_object_new_string(desc));
		}
	}
	/*
	 * Store the TLS key in PSK interchange format
	 */
	if (cfg->tls_key)
		json_export_nvme_tls_key(cfg->keyring, cfg->tls_key, port_obj);

	json_object_array_add(ctrl_array, port_obj);
}

static void json_update_subsys(struct json_object *subsys_array,
			       nvme_subsystem_t s)
{
	nvme_ctrl_t c;
	const char *subsysnqn = nvme_subsystem_get_nqn(s), *app;
	struct json_object *subsys_obj = json_object_new_object();
	struct json_object *port_array;

	/* Skip discovery subsystems as the nqn is not unique */
	if (!strcmp(subsysnqn, NVME_DISC_SUBSYS_NAME))
		return;

	json_object_object_add(subsys_obj, "nqn",
			       json_object_new_string(subsysnqn));
	app = nvme_subsystem_get_application(s);
	if (app)
		json_object_object_add(subsys_obj, "application",
				       json_object_new_string(app));
	port_array = json_object_new_array();
	nvme_subsystem_for_each_ctrl(s, c) {
		json_update_port(port_array, c);
	}
	if (json_object_array_length(port_array)) {
		json_object_object_add(subsys_obj, "ports", port_array);
		json_object_array_add(subsys_array, subsys_obj);
	} else {
		json_object_put(port_array);
		json_object_put(subsys_obj);
	}
}

int json_update_config(nvme_root_t r, const char *config_file)
{
	nvme_host_t h;
	struct json_object *json_root, *host_obj;
	struct json_object *subsys_array;
	int ret = 0;

	json_root = json_object_new_array();
	nvme_for_each_host(r, h) {
		nvme_subsystem_t s;
		const char *hostnqn, *hostid, *dhchap_key, *hostsymname;

		host_obj = json_object_new_object();
		if (!host_obj)
			continue;
		hostnqn = nvme_host_get_hostnqn(h);
		json_object_object_add(host_obj, "hostnqn",
				       json_object_new_string(hostnqn));
		hostid = nvme_host_get_hostid(h);
		if (hostid)
			json_object_object_add(host_obj, "hostid",
					       json_object_new_string(hostid));
		dhchap_key = nvme_host_get_dhchap_key(h);
		if (dhchap_key)
			json_object_object_add(host_obj, "dhchap_key",
					       json_object_new_string(dhchap_key));
		hostsymname = nvme_host_get_hostsymname(h);
		if (hostsymname)
			json_object_object_add(host_obj, "hostsymname",
					       json_object_new_string(hostsymname));
		if (h->pdc_enabled_valid)
			json_object_object_add(host_obj, "persistent_discovery_ctrl",
					       json_object_new_boolean(h->pdc_enabled));
		subsys_array = json_object_new_array();
		nvme_for_each_subsystem(h, s) {
			json_update_subsys(subsys_array, s);
		}
		if (json_object_array_length(subsys_array)) {
			json_object_object_add(host_obj, "subsystems",
					       subsys_array);
			json_object_array_add(json_root, host_obj);
		} else {
			json_object_put(subsys_array);
			json_object_put(host_obj);
		}
	}
	if (!config_file) {
		ret = json_object_to_fd(1, json_root, JSON_C_TO_STRING_PRETTY);
		printf("\n");
	} else
		ret = json_object_to_file_ext(config_file, json_root,
					      JSON_C_TO_STRING_PRETTY);
	if (ret < 0) {
		nvme_msg(r, LOG_ERR, "Failed to write to %s, %s\n",
			 config_file ? "stdout" : config_file,
			 json_util_get_last_err());
		ret = -1;
		errno = EIO;
	}
	json_object_put(json_root);

	return ret;
}

static void json_dump_ctrl(struct json_object *ctrl_array, nvme_ctrl_t c)
{
	struct nvme_fabrics_config *cfg = nvme_ctrl_get_config(c);
	struct json_object *ctrl_obj = json_object_new_object();
	const char *name, *transport, *value;

	name = nvme_ctrl_get_name(c);
	if (name && strlen(name))
		json_object_object_add(ctrl_obj, "name",
				       json_object_new_string(name));
	transport = nvme_ctrl_get_transport(c);
	json_object_object_add(ctrl_obj, "transport",
			       json_object_new_string(transport));
	value = nvme_ctrl_get_traddr(c);
	if (value)
		json_object_object_add(ctrl_obj, "traddr",
				       json_object_new_string(value));
	value = nvme_ctrl_get_host_traddr(c);
	if (value)
		json_object_object_add(ctrl_obj, "host_traddr",
				       json_object_new_string(value));
	value = nvme_ctrl_get_host_iface(c);
	if (value)
		json_object_object_add(ctrl_obj, "host_iface",
				       json_object_new_string(value));
	value = nvme_ctrl_get_trsvcid(c);
	if (value)
		json_object_object_add(ctrl_obj, "trsvcid",
				       json_object_new_string(value));
	value = nvme_ctrl_get_dhchap_host_key(c);
	if (value)
		json_object_object_add(ctrl_obj, "dhchap_key",
				       json_object_new_string(value));
	value = nvme_ctrl_get_dhchap_key(c);
	if (value)
		json_object_object_add(ctrl_obj, "dhchap_ctrl_key",
				       json_object_new_string(value));
	JSON_INT_OPTION(cfg, ctrl_obj, nr_io_queues, 0);
	JSON_INT_OPTION(cfg, ctrl_obj, nr_write_queues, 0);
	JSON_INT_OPTION(cfg, ctrl_obj, nr_poll_queues, 0);
	JSON_INT_OPTION(cfg, ctrl_obj, queue_size, 0);
	JSON_INT_OPTION(cfg, ctrl_obj, keep_alive_tmo, 0);
	JSON_INT_OPTION(cfg, ctrl_obj, reconnect_delay, 0);
	if (strcmp(transport, "loop")) {
		JSON_INT_OPTION(cfg, ctrl_obj, ctrl_loss_tmo,
				NVMF_DEF_CTRL_LOSS_TMO);
		JSON_INT_OPTION(cfg, ctrl_obj, fast_io_fail_tmo, 0);
	}
	JSON_INT_OPTION(cfg, ctrl_obj, tos, -1);
	JSON_BOOL_OPTION(cfg, ctrl_obj, duplicate_connect);
	JSON_BOOL_OPTION(cfg, ctrl_obj, disable_sqflow);
	JSON_BOOL_OPTION(cfg, ctrl_obj, hdr_digest);
	JSON_BOOL_OPTION(cfg, ctrl_obj, data_digest);
	if (!strcmp(transport, "tcp")) {
		JSON_BOOL_OPTION(cfg, ctrl_obj, tls);

		if (cfg->tls_key)
			json_export_nvme_tls_key(cfg->keyring, cfg->tls_key,
						 ctrl_obj);
	}
	JSON_BOOL_OPTION(cfg, ctrl_obj, concat);
	if (nvme_ctrl_is_persistent(c))
		json_object_object_add(ctrl_obj, "persistent",
				       json_object_new_boolean(true));
	if (nvme_ctrl_is_discovery_ctrl(c))
		json_object_object_add(ctrl_obj, "discovery",
				       json_object_new_boolean(true));
	json_object_array_add(ctrl_array, ctrl_obj);
}

static void json_dump_subsys(struct json_object *subsys_array,
			       nvme_subsystem_t s)
{
	nvme_ctrl_t c;
	struct json_object *subsys_obj = json_object_new_object();
	struct json_object *ctrl_array;

	json_object_object_add(subsys_obj, "name",
			       json_object_new_string(nvme_subsystem_get_name(s)));
	json_object_object_add(subsys_obj, "nqn",
			       json_object_new_string(nvme_subsystem_get_nqn(s)));
	ctrl_array = json_object_new_array();
	nvme_subsystem_for_each_ctrl(s, c) {
		json_dump_ctrl(ctrl_array, c);
	}
	if (json_object_array_length(ctrl_array))
		json_object_object_add(subsys_obj, "controllers", ctrl_array);
	else
		json_object_put(ctrl_array);
	json_object_array_add(subsys_array, subsys_obj);
}

int json_dump_tree(nvme_root_t r)
{
	nvme_host_t h;
	struct json_object *json_root, *host_obj;
	struct json_object *host_array, *subsys_array;
	int ret = 0;

	json_root = json_object_new_object();
	host_array = json_object_new_array();
	nvme_for_each_host(r, h) {
		nvme_subsystem_t s;
		const char *hostid, *dhchap_key;

		host_obj = json_object_new_object();
		json_object_object_add(host_obj, "hostnqn",
				       json_object_new_string(nvme_host_get_hostnqn(h)));
		hostid = nvme_host_get_hostid(h);
		if (hostid)
			json_object_object_add(host_obj, "hostid",
					       json_object_new_string(hostid));
		dhchap_key = nvme_host_get_dhchap_key(h);
		if (dhchap_key)
			json_object_object_add(host_obj, "dhchap_key",
					       json_object_new_string(dhchap_key));
		if (h->pdc_enabled_valid)
			json_object_object_add(host_obj, "persistent_discovery_ctrl",
					       json_object_new_boolean(h->pdc_enabled));
		subsys_array = json_object_new_array();
		nvme_for_each_subsystem(h, s) {
			json_dump_subsys(subsys_array, s);
		}
		if (json_object_array_length(subsys_array))
			json_object_object_add(host_obj, "subsystems",
					       subsys_array);
		else
			json_object_put(subsys_array);
		json_object_array_add(host_array, host_obj);
	}
	json_object_object_add(json_root, "hosts", host_array);

	ret = json_object_to_fd(r->log.fd, json_root, JSON_C_TO_STRING_PRETTY);
	if (ret < 0) {
		nvme_msg(r, LOG_ERR, "Failed to write, %s\n",
			 json_util_get_last_err());
		ret = -1;
		errno = EIO;
	}
	json_object_put(json_root);

	return ret;
}
