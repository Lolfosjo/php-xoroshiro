dnl config.m4 for xoroshiro PHP extension

PHP_ARG_ENABLE([xoroshiro],
  [whether to enable xoroshiro support],
  [AS_HELP_STRING([--enable-xoroshiro], [Enable xoroshiro support])],
  [yes])

if test "$PHP_XOROSHIRO" != "no"; then
  PHP_NEW_EXTENSION(xoroshiro, xoroshiro.c, $ext_shared)
fi
