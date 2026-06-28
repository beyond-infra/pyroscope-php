PHP_ARG_ENABLE(pyroscope_php, whether to enable pyroscope_php support,
[  --enable-pyroscope-php          Enable Pyroscope PHP profiler])

if test "$PHP_PYROSCOPE_PHP" != "no"; then
  PHP_ADD_LIBRARY(pthread)
  PHP_CHECK_LIBRARY(curl, curl_easy_init, [
    PHP_ADD_LIBRARY(curl)
  ], [
    AC_MSG_ERROR(curl library required)
  ])
  PHP_NEW_EXTENSION(pyroscope_php, pyroscope_php.c, $ext_shared)
fi
