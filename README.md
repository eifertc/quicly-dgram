quicly-dgram
===

Fork of H2O/quicly, extending it with unreliable datagrams 
(see https://tools.ietf.org/html/draft-pauly-quic-datagram-05)

Including variations of the datagram implementation for real time streaming experiments.

Including quicly based gstreamer plugins
quiclysink and quiclysrc

Including gstreamer application for testing purposes
quicly_stream


The software is licensed under the MIT License.

How to build
---

```
% mkdir build && cd build
% cmake ..
% make
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

The executable quicly_stream is a gstreamer application providing a pipline for streaming RTP video over Quic or UDP with various options for congestion control.

Use
```
./quicly_stream --help
```
for further options

The use of Rmcat scream congestion control requires a seperate repository found here: https://github.com/Banaschar/scream
