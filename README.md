quicly-dgram
===

Fork of H2O/quicly, extending it with unreliable datagrams 
(see https://tools.ietf.org/html/draft-pauly-quic-datagram-03)

Including quicly based gstreamer plugins
quiclysink and quiclysrc

Including gstreamer applications for testing purposes
quicly_stream and udp_stream


The software is licensed under the MIT License.

How to build
---

```
% cmake .
% make
% make check
```

Building the software requires OpenSSL 1.0.2 or above.
If you have OpenSSL installed in a non-standard directory, you can pass the location using the `PKG_CONFIG_PATH` environment variable.

```
% PKG_CONFIG_PATH=/path/to/openssl/lib/pkgconfig cmake .
```

Usage
---
The two plugins quiclysink and quiclysrc can be used like any other gstreamer plugins.
If they are not installed, the GST_PLUGIN_PATH variable has to be set to the directory containing them (build/libgst).

The executable quicly_stream is a gstreamer application providing a pipline for streaming video over quic.
Use
```
./quicly_stream --help
```
for further options