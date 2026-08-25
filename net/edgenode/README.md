# edgenode for OpenWrt

This directory contains a small C daemon and an OpenWrt package recipe. It has no C++
runtime, full protobuf runtime, or database dependency. This package repository is the
sole source location for the OpenWrt node implementation and its node-side tests.

Implemented foundations:

- the node registers independently with up to four platforms using its 15-digit IMEI;
- an HTTPS platform base address is upgraded internally to a binary WSS
  session carrying one nanopb `Envelope` per message;
- WSS and firmware HTTPS validate both the certificate chain and hostname against
  OpenWrt's `ca-bundle`; TLS initialization and verification failures fail closed;
- every platform has isolated registration, config, reconnect, heartbeat, and outbox state;
  failed connections retry forever; a 30-second application handshake deadline, an
  enrolled-session watchdog, and a 60-second outbox ACK deadline break half-open sessions;
- config and outbox files are raw nanopb messages under
  `/tmp/edgenode/<platform_id>/`; process restarts recover them, device reboots do not;
- before every tmpfs write, the daemon preserves 15% free space by rolling the oldest
  outbox message across all platforms; active and staging config are never rolled;
- supervised acquisition workers read every second, process queued writes in that same
  cadence, and report independently at the configured interval; IPC is consumed by
  `ev_io` readiness events so device I/O never blocks the WSS event loop;
- Modbus TCP/RTU and S7 request/response codecs implement reads and writes. A successful
  command requires readback equality;
- an unresponsive S7 PLC closes the TCP socket and repeats TCP, COTP, and S7 Setup
  Communication on the next one-second cycle.
- network and serial capabilities are reported automatically; when `ttyd` is installed,
  the platform exposes an authenticated remote terminal;
- bootstrap-authorized commands can create, update, or delete UCI logical interfaces
  backed by one physical device or a bridge, using DHCP or static IPv4. The configured
  4G/WAN interface and its descendants are excluded, and an unconfirmed network change
  restores the previous UCI network configuration automatically;
- the same commands can manage additional HTTPS platforms, download verified
  firmware, and invoke `sysupgrade`.

The active-config-to-physical-endpoint binding is kept separate from the wire/session
layer. The current code provides the tested protocol codecs and scheduler that binding
uses; actual target hardware is still required before declaring a target deployable.

## Runtime observability

The node separates operational events from continuously changing state:

- the runtime event log contains lifecycle changes, configuration outcomes, transport
  changes, and failures; it is not a per-cycle activity journal;
- successful polling and telemetry enqueue operations do not create log entries;
- current delivery pressure is reported by the heartbeat `outbox_records` and
  `outbox_bytes` fields instead of repeated success messages;
- command, configuration, network, and firmware outcomes remain in their typed protocol
  results and platform task history;
- raw protocol packets are available only at `debug` level.

## Configure

Platform addresses are not compiled into the daemon. Every connection comes from a
`config platform` UCI section. A fresh install creates the default platform
`https://i.a-z.xin`; it can be edited in LuCI or with UCI, and up to four platforms can
be enabled at the same time. The daemon derives the internal upgrade path
`/edge/v1/connect` for each base address.

The default IMEI and model are empty: the init service reads IMEI from the modem and
model from `/tmp/sysinfo/model` before starting the platform client. To override either
value explicitly, use UCI and restart the service:

```sh
uci set edgenode.node.imei='your-15-digit-imei'
uci set edgenode.node.model='your-model'
uci commit edgenode
/etc/init.d/edgenode restart
```

The default platform is ordinary persistent UCI configuration:

```sh
uci set edgenode.bootstrap.url='https://i.a-z.xin'
uci commit edgenode
/etc/init.d/edgenode reload
```

Install `luci-app-edgenode` from the companion LuCI feed to add, edit, enable, disable,
order, or remove platform sections in **Services → Edge Node**. LuCI generates the
internal connection ID and never asks the user to enter it. The default ID
`00000000-0000-7000-8000-000000000001` remains the privileged network/firmware owner,
but its URL is fully editable.

New nodes enter the platform's manual approval flow by IMEI. Approval upgrades that
connection to an enrolled session. A connection that never receives approval is
closed at the application handshake deadline and retried, so a half-open or stale
pending session cannot stop reconnection.
Additional platform sections may be managed locally through LuCI/UCI or by authenticated
commands from the default platform; the node applies remote changes through
`uci set/delete` and `uci commit`.

## Build an IPK

`net/edgenode` is a complete OpenWrt package directory. Copy or link this whole
directory into the selected SDK as `package/edgenode`; do not copy only its `Makefile`:

```sh
ln -s /path/to/openwrt-dtu-packages/net/edgenode /path/to/openwrt/package/edgenode
make menuconfig
make package/edgenode/compile V=s
```

The package is self-contained apart from dependencies fetched by the OpenWrt build
system: `Makefile`, `files/`, `proto/`, and `src/` stay together.
`proto/edge.proto` is the node-side wire-contract source and must stay byte-identical
to the platform copy in `iot-engine/service/features/edge/edge.proto`.
The OpenWrt SDK uses the committed nanopb C sources when compiling.
The recipe downloads nanopb `0.4.9.1`, compiles only its three C runtime files, enables
`-Os`, LTO, function sections, and linker garbage collection, and dynamically uses
OpenWrt's mbedTLS-backed libuwsc.

The package, daemon, init service, UCI configuration, and runtime paths are all named
`edgenode`.

The nanopb C files generated from `proto/edge.proto` are committed under `generated/`.
This keeps the OpenWrt 18.06 build independent of host Python and protobuf packages.
Regenerate both files with nanopb 0.4.9.1 whenever the protocol changes.

Node-side protocol, runtime, and configuration tests are kept under `tests/` beside
the implementation instead of in the platform repository. They can be run on a host
with the exact nanopb source used for generation:

```sh
cmake -S tests -B build/tests -DNANOPB_ROOT=/path/to/nanopb-0.4.9.1
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Hardware paths, interface names, bridge mode, modem USB ID, AT port, status path, and
monitor interval are UCI settings rather than compiled constants. The TAS-682 package
defaults describe its single field serial port and Ethernet port plus the LierdaComm
modem. The service initializes the model from `/tmp/sysinfo/model`, initializes IMEI and
ICCID from the modem, and keeps registration and signal status in tmpfs without
repeatedly writing flash.

The resulting package must be cross-compiled and installed on the actual target. A host
binary is not an OpenWrt deliverable.
