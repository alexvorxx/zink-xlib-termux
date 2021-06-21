html_docs = custom_target(
  'html',
  depend_files: files('conf.py'),
  command: [sphinx, '-b', 'html', '-q', meson.current_source_dir(),
            '@OUTPUT@'],
  output: 'html',
  build_by_default: get_option('html-docs').enabled(),
)

install_subdir(html_docs.full_path(),
  install_dir: join_paths(get_option('datadir'), 'doc', 'mesa'),
  exclude_files: ['.buildinfo'],
  exclude_directories: ['.doctrees'],
  strip_directory: true
)
