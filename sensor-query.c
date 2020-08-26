/* Minimal application to query sensor values and threshold states, for
 * a predefined set of sensor objects.
 */

#include <err.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <systemd/sd-bus.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

struct sensor_desc {
	const char *service;
	const char *object;
};

/* Service name and object path for each sensor to query.
 * Expand as necessary
 */
static const struct sensor_desc descs[] = {
	{
		"xyz.openbmc_project.HwmonTempSensor",
		"/xyz/openbmc_project/sensors/temperature/Temp"
	},
};

struct sensor_data {
	char	type;
	union {
		double d;
		int64_t x;
	} value;
	bool	lower_crit;
	bool	upper_crit;
	bool	lower_warn;
	bool	upper_warn;
};

/* parses a reply message (currently referencing a variant) into a
 * sensor value. Will consume the variant from the reply. */
static int parse_sensor_value(sd_bus_message *reply, struct sensor_data *data,
		const char *obj)
{
	const char *type_str = NULL;
	char c;
	int rc;

	rc = sd_bus_message_peek_type(reply, &c, &type_str);
	if (rc < 0)
		return rc;

	if (c != 'v' || strlen(type_str) != 1) {
		printf("%s: invalid sensor type %c:%s\n", obj, c, type_str);
		return -1;
	}

	data->type = type_str[0];

	if (data->type == 'd') {
		rc = sd_bus_message_read(reply, "v", "d", &data->value.d);

	} else if (data->type == 'x') {
		rc = sd_bus_message_read(reply, "v", "x", &data->value.x);

	} else {
		printf("%s: invalid type '%c', expected 'd/x'\n",
				obj, data->type);
		rc = -1;
	}

	return rc;
}

/* Query a sensor object over dbus, by performing a single GetAll method
 * on the properties interface. That provides the threhold states and
 * value in a single dbus call.
 */
static int query_sensor(sd_bus *bus, const struct sensor_desc *desc,
		struct sensor_data *sensor)
{
	sd_bus_message *reply;
	bool value_set;
	int rc;

	sensor->lower_crit = false;
	sensor->upper_crit = false;
	sensor->lower_warn = false;
	sensor->upper_warn = false;

        rc = sd_bus_call_method(bus, desc->service, desc->object,
			"org.freedesktop.DBus.Properties", "GetAll",
			NULL, &reply, "s", "");
	if (rc < 0)
		return rc;

	rc = sd_bus_message_enter_container(reply, 'a', "{sv}");
	if (rc < 0)
		return rc;

	value_set = false;
	for (;;) {
		bool *threshold_p;
		const char *prop;
		bool is_value;

		rc = sd_bus_message_enter_container(reply, 'e', "sv");
		if (rc <= 0)
			break;

		rc = sd_bus_message_read(reply, "s", &prop);
		if (rc < 0)
			break;

		threshold_p = NULL;
		is_value = false;

		if (!strcmp(prop, "Value")) {
			is_value = true;
		} else if (!strcmp(prop, "CriticalAlarmLow")) {
			threshold_p = &sensor->lower_crit;
		} else if (!strcmp(prop, "CriticalAlarmHigh")) {
			threshold_p = &sensor->upper_crit;
		} else if (!strcmp(prop, "WarningAlarmLow")) {
			threshold_p = &sensor->lower_warn;
		} else if (!strcmp(prop, "WarningAlarmHigh")) {
			threshold_p = &sensor->upper_warn;
		}

		if (is_value) {
			rc = parse_sensor_value(reply, sensor, desc->object);
			if (rc < 0)
				break;

			value_set = true;

		} else if (threshold_p) {
			int tmp;
			rc = sd_bus_message_read(reply, "v", "b", &tmp);
			*threshold_p = !!tmp;
			if (rc < 0)
				break;

		} else {
			rc = sd_bus_message_skip(reply, "v");
			if (rc < 0)
				break;
		}

		rc = sd_bus_message_exit_container(reply);
		if (rc < 0)
			break;
	}

	sd_bus_message_exit_container(reply);
	sd_bus_message_unref(reply);

	if (!value_set) {
		printf("%s: no Value property\n", desc->object);
		return -1;
	}

	return rc;
}

/* str must have enough capacity for all thresholds to be set:
 *   lc,lw,uc,uw\0 - 12 chars.
 */
static void format_thresholds(struct sensor_data *sensor, char *str)
{
	struct {
		bool *ptr;
		const char *label;
	} labels[] = {
		{ &sensor->lower_crit, "lc" },
		{ &sensor->upper_crit, "uc" },
		{ &sensor->lower_warn, "lw" },
		{ &sensor->upper_warn, "uw" },
	};
	unsigned int i;
	int n = 0;
	char *p;

	p = str;

	for (i = 0; i < ARRAY_SIZE(labels); i++) {
		if (!*labels[i].ptr)
			continue;
		if (n)
			*(p++) = ',';
		strcpy(p, labels[i].label);
		p += 2;
		n++;
	}

	if (!n)
		strcpy(p, "ok");
}

/* str assumed to be 12 bytes */
static void format_value(const struct sensor_data *sensor, char *str)
{
	const size_t str_size = 12;

	switch (sensor->type) {
	case 'd':
		snprintf(str, str_size, "%f", sensor->value.d);
		break;
	case 'x':
		snprintf(str, str_size, "%" PRId64, sensor->value.x);
		break;
	default:
		strncpy(str, "(unknown)", str_size);
	}
}

static void print_sensor(sd_bus *bus, const struct sensor_desc *desc)
{
	char threshold_str[12], value_str[12];
	struct sensor_data sensor;
	int rc;

	rc = query_sensor(bus, desc, &sensor);
	if (rc) {
		printf("%s: failed to read sensor object\n", desc->object);
		return;
	}

	format_value(&sensor, value_str);
	format_thresholds(&sensor, threshold_str);

	printf("%s: %s %s\n", desc->object, value_str, threshold_str);
}

static bool sensor_matches_type(const struct sensor_desc *desc,
		const char *type)
{
	const char *sensor_root = "/xyz/openbmc_project/sensors/";
	size_t type_len, root_len = strlen(sensor_root);
	const char *sep, *path = desc->object;

	/* if no type was specified, everything matches */
	if (!type || !strlen(type))
		return true;

	type_len = strlen(type);

	/* are we in the sensor namespace? */
	if (strncmp(path, sensor_root, root_len))
		return false;

	/* do we have another path component, of the right length? */
	sep = strchr(path + root_len, '/');
	if (!sep || (unsigned long)(sep - (path + root_len)) != type_len)
		return false;

	/* does that path component match the specified type? */
	return !strncmp(path + root_len, type, type_len);
}

int main(int argc, char **argv)
{
	const char *type;
	unsigned int i;
	sd_bus *bus;
	int rc;

	type = NULL;
	if (argc > 1)
		type = argv[1];

	rc = sd_bus_default(&bus);
	if (rc < 0)
		errx(EXIT_FAILURE, "can't connect to dbus: %s", strerror(-rc));

	for (i = 0; i < ARRAY_SIZE(descs); i++) {
		const struct sensor_desc *desc = &descs[i];

		if (!sensor_matches_type(desc, type))
			continue;

		print_sensor(bus, desc);
	}

	return EXIT_SUCCESS;
}
