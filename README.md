mclk.lv2 - Midi Clock Generator
===============================

mclk.lv2 is MIDI Clock and MIDI realtime-message generator.
It can run freely with dedicated BPM, start/stop/continue controls
or generate MIDI Clock using host-provided musical time.

mclk.lv2 supports Transport, Song-Position and MIDI Clock and
allows to only generate a subset.

Install
-------

Compiling mclk.lv2 requires the LV2 SDK, gnu-make, and a c-compiler.

```bash
  git clone git://github.com/x42/mclk.lv2.git
  cd mclk.lv2
  make
  sudo make install PREFIX=/usr
```

Note to packagers: The Makefile honors `PREFIX` and `DESTDIR` variables as well
as `CFLAGS`, `LDFLAGS` and `OPTIMIZATIONS` (additions to `CFLAGS`), also
see the first 10 lines of the Makefile.
