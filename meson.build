project('sensor-query', 'c',
        version: '0.1',
	meson_version: '>=0.49.0',
        default_options: [
		'warning_level=3',
		'werror=true',
		'c_std=gnu18'
        ],
)

libsystemd = dependency('libsystemd')

executable(
	'sensor-query',
	'sensor-query.c',
	dependencies: [
		libsystemd,
	],
	install: true,
)
