# ZMK Persistent Layers

Remembers a set of layers per endpoint, across reboots.

A mode layer — an alternate OS, a different base — is really a property of the host you are typing
on. ZMK keeps neither half of that: layer state is a plain static that every restart clears, and it
is shared by all endpoints, so switching Bluetooth profiles carries the previous host's mode along
with it.

This module records every change to the layers it owns against the endpoint that was selected at
the time, and replays that row whenever the endpoint comes back. Turn the layer on once for the
host that needs it and it follows that host from then on, including after a battery change or a
reflash.

## Usage

Add the module to `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: rafaelromao
      url-base: https://github.com/rafaelromao
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-persistent-layers
      remote: rafaelromao
      revision: main
  self:
    path: config
```

Then declare one node in your keymap:

```dts
/ {
    persistent_layers {
        compatible = "zmk,persistent-layers";
        layers = <ALT_OS>;
        save-delay-ms = <2000>;
    };
};
```

`CONFIG_ZMK_PERSISTENT_LAYERS` turns itself on when the node is present. It is built only on the
central, since that is the only side with layer state.

## Devicetree binding

| Property | Type | Default | Description |
| --- | --- | --- | --- |
| `layers` | array | required | The layers this node owns. Nothing else is read or written. |
| `save-delay-ms` | int | `2000` | How long to wait after a change before writing to flash, so a run of toggles costs one write. |

Keep `save-delay-ms` short. A reboot inside the window loses the change, which is the thing the
module exists to prevent; the write rate is a handful a day, and NVS wear-levels across the
partition, so there is little to buy by stretching it.

## What belongs here

Layers that behave as **modes**, held on by a locking toggle:

```dts
lock_on: toggle_layer_on_locking {
    compatible = "zmk,behavior-toggle-layer";
    #binding-cells = <1>;
    toggle-mode = "on";
    locking;
};
```

Restores are performed with locking, matching how such a layer is meant to be held. A momentary or
sticky layer does not belong here — it would be restored on and never released.

Neither does a layer something else already drives. A layer managed by a host listener, such as
`zmk-vim-mode`'s `managed-layers`, has an owner already, and the two would fight at connect time.

## Behaviour worth knowing

- **`ZMK_TRANSPORT_NONE` is not a host.** While nothing is connected the current state is held as
  it is, and neither recorded nor replayed. The mode does not flicker when a host drops.
- **Restores are not recorded.** Replaying a row raises `layer_state_changed` for each layer it
  moves; those are the module's own writes and are ignored.
- **Without `CONFIG_SETTINGS` you still get the per-endpoint half.** ZMK implies `SETTINGS` for BLE
  builds. A USB-only board without a settings backend keeps switching per endpoint and simply
  forgets on reboot.
- **A `settings_reset` firmware clears the table**, and every endpoint starts on the build default.

## Licence

MIT
