/* Minimal application to query sensor values and threshold states, for
 * a predefined set of sensor objects.
 */

#include <err.h>
#include <stdbool.h>
#include <stdio.h>
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
	double	value;
	bool	lower_crit;
	bool	upper_crit;
	bool	lower_warn;
	bool	upper_warn;
};

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

	for (;;) {
		bool *threshold_p;
		double *value_p;
		const char *prop;

		rc = sd_bus_message_enter_container(reply, 'e', "sv");
		if (rc <= 0)
			break;

		rc = sd_bus_message_read(reply, "s", &prop);
		if (rc < 0)
			break;

		threshold_p = NULL;
		value_p = NULL;

		if (!strcmp(prop, "Value")) {
			value_p = &sensor->value;
		} else if (!strcmp(prop, "CriticalAlarmLow")) {
			threshold_p = &sensor->lower_crit;
		} else if (!strcmp(prop, "CriticalAlarmHigh")) {
			threshold_p = &sensor->upper_crit;
		} else if (!strcmp(prop, "WarningAlarmLow")) {
			threshold_p = &sensor->lower_warn;
		} else if (!strcmp(prop, "WarningAlarmHigh")) {
			threshold_p = &sensor->upper_warn;
		}

		if (value_p) {
			const char *type = NULL;
			rc = sd_bus_message_peek_type(reply, NULL, &type);
			if (rc < 0 || strcmp(type, "d")) {
				printf("%s: invalid type '%s', expected 'd'\n",
						desc->object, type);
				rc = -1;
				break;
			}
			rc = sd_bus_message_read(reply, "v", "d", value_p);
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
		n++;
	}

	if (!n)
		strcpy(p, "ok");
}

static void print_sensor(sd_bus *bus, const struct sensor_desc *desc)
{
	struct sensor_data sensor;
	char threshold_str[12];
	int rc;

	rc = query_sensor(bus, desc, &sensor);
	if (rc) {
		printf("%s: failed to read sensor object\n", desc->object);
		return;
	}

	format_thresholds(&sensor, threshold_str);

	printf("%s: %f %s\n", desc->object, sensor.value, threshold_str);
}

int main(void)
{
	unsigned int i;
	sd_bus *bus;
	int rc;

	rc = sd_bus_default(&bus);
	if (rc < 0)
		errx(EXIT_FAILURE, "can't connect to dbus: %s", strerror(-rc));

	for (i = 0; i < ARRAY_SIZE(descs); i++)
		print_sensor(bus, &descs[i]);

	return EXIT_SUCCESS;
}
