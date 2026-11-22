# Forwarder ("Add to home screen") — origins and licences

* `forwarder/hbl/` — the program every shortcut runs. It is **nx-hbloader**
  (switchbrew, ISC licence, see `hbl/LICENSE.md`) with a small change: it reads
  the NRO path and argv from `sdmc:/config/vkpcnx/forwarders/<program id>.cfg`
  and exits back to HOME when the NRO quits instead of starting hbmenu.
  Built by CMake (`vkpcnx_forwarder_hbl`) and shipped inside the .nro's romfs.
* `vkpcnx/src/core/forwarder/nca_builder.cpp`, `forwarder.cpp` — builds the
  program/control/meta NCAs on the console and installs them through ncm/ns.
  The sequence and the section layout were taken over from **Sphaira**'s
  `source/owo.cpp` (<https://github.com/ITotalJustice/sphaira>, **GPL-3.0**) and
  re-written, so this part should be treated as a derivative work of a GPL-3.0
  program. vkpcnx itself is Apache-2.0; **if builds containing this code are
  redistributed, the whole binary has to comply with the GPL-3.0** (or this code
  has to be replaced by an independent implementation). The fork keeps it in
  these files only so it can be removed or swapped without touching the rest.
* The NCA header key sources are the well-known Nintendo constants every NCA
  tool uses.
