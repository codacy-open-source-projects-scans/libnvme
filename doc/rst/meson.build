top_source_dir = meson.current_source_dir() + '/../../'

want_docs = get_option('docs')

if want_docs != 'false'
  want_docs_build = get_option('docs-build')
  rstdir = get_option('rstdir')
  if want_docs_build
    kernel_doc = find_program(top_source_dir + 'scripts/kernel-doc')

    rst_conf = configuration_data()
    rst_conf.set('SYSCONFDIR', sysconfdir)

    if want_docs == 'all' or want_docs == 'rst' or want_docs == 'html'
      foreach apif : api_files
        afile = files(top_source_dir + 'src/nvme/' + apif)
        subst = configure_file(
            input: afile,
            output: '@BASENAME@.subst',
            configuration: rst_conf)
        rst = custom_target(
          apif.underscorify() + '_rst',
          input: subst,
          output: '@BASENAME@.rst',
          capture: true,
          command: [kernel_doc,
                    '-rst',
                    '@INPUT@'],
          install: true,
          install_dir: rstdir)
      endforeach
    endif
  else
    # no prebuild docs
  endif
endif
