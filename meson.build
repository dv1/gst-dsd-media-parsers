project('gst-dsd-media-parsers', 'cpp', default_options: ['cpp_std=c++20'], version: '1.0.0')

gstreamer_dep       = dependency('gstreamer-1.0',       required : true)
gstreamer_base_dep  = dependency('gstreamer-base-1.0',  required : true)
gstreamer_tag_dep   = dependency('gstreamer-tag-1.0',   required : true)
gstreamer_audio_dep = dependency('gstreamer-audio-1.0', required : true)

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')


configinc = include_directories('.')


conf_data = configuration_data()
conf_data.set_quoted('GST_PACKAGE_NAME', get_option('package-name'))
conf_data.set_quoted('GST_PACKAGE_ORIGIN', get_option('package-origin'))
conf_data.set_quoted('PACKAGE', 'gstdsdmediaparsers')
conf_data.set_quoted('PACKAGE_BUGREPORT', 'https://github.com/dv1/gstdsdmediaparsers')
conf_data.set_quoted('VERSION', meson.project_version())


library(
	'gstdsdmediaparsers',
	[
		'src/chunk_stack.cpp',
		'src/gstdffparse.cpp',
		'src/gstdsdchunkparse.cpp',
		'src/gstdsdmediaparse.cpp',
		'src/gstdsfparse.cpp',
		'src/mapped_buffer.cpp',
		'src/plugin.cpp'
	],
	install : true,
	install_dir: plugins_install_dir,
	include_directories: [configinc],
	dependencies : [gstreamer_dep, gstreamer_base_dep, gstreamer_tag_dep, gstreamer_audio_dep]
)


configure_file(output : 'config.h', configuration : conf_data)
