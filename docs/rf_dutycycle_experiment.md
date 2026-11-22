# RF duty-cycle experiment

This experiment is based on MeshCore **81dbae59** and targets an attended
Heltec V3 repeater in an RF shielded lab. It adds a local-terminal traffic producer and the
[MC-Dutycycle Flipper app](../tools/flipper-dutycycle/README.md).
Commit `c03a16dc` used the original packet admission threshold. The dispatcher
now requires enough credit for the next packet's estimated airtime.

## Repeater

Build the repeater with:

```sh
pio run -e Heltec_v3_repeater
```

Use the local serial terminal. Configure the radio to match the receiver
(EU/Narrow is `869.618 MHz`, `62.5 kHz`, `SF8`, `CR8`):

```text
ver
get radio
get dutycycle
set radio 869.618,62.5,8,8
set dutycycle 10
```

The `ver` command should report `v1.18.0+rflab_piie` for this experiment build.

For a scoped test, create or select an allowed named region:

```text
region put lab
region allowf lab
```

`lab` is only an example. Any existing allowed named region with a transport
key can be used. Omit the region for an unscoped test; no region setup is needed
for that case. The wildcard region is not accepted as a named argument.
The test uses the radio, transmit power, and duty-cycle settings already
configured on the repeater. Set `dutycycle` to 10 explicitly for this experiment and confirm it with
`get dutycycle`. Record the settings and prior traffic for each run.

Arm the Flipper app, then start the producer:

```text
flood 60 lab
flood status
flood stop
```

The syntax is `flood [--continuous] <seconds> [region]`. To send unscoped traffic, use
`flood 60` instead. This uses no transport key, including when a default scope
is configured or a previous test used a named region. Status identifies it as
`<unscoped>`.

The duration must be an integer from **1 to 7,200 seconds** and limits elapsed
wall-clock time. At most one test packet is queued or transmitting. Full-size,
uniquely numbered raw-custom packets enter the normal flood transmit path.
The existing channel checks, scheduling, and duty-cycle budget control
transmission. In the default paced mode, the earliest next test packet can be
queued is one estimated packet airtime after the previous TX completes. Any
limiter wait overlaps this pause. Use `flood --continuous 60` for the original
traffic pattern. Ordinary queued packets are preserved, and one pool slot is kept
free when producing test traffic.

At the deadline, or on `flood stop`, the producer removes its queued packet.
A transmission already in progress finishes normally, so the last RF frame can
end after the nominal deadline. `flood status` distinguishes running, finishing,
and idle states and reports generated/sent/failed counts. Generated includes a
packet cancelled before transmission. Only one run can be active or finishing.
Remote requests are rejected.

At the baseline revision, raw-custom payloads are processed only on direct
routes; flood-route raw-custom frames are released without relaying. The test
therefore sends scoped or unscoped frames without being repeated by upstream
peers with that behavior. No changes to receiver/forwarding code are required.

Use the shielded setup for the two-hour experiment. The starting budget can
allow more than 10% in a rolling hour even when configured at 10%. Other repeater
traffic, including adverts, can also contribute to the RF trace and consume
budget.

## Receiver and interpretation

Initialize the submodule and build the Flipper app:

```sh
git submodule update --init tools/flipper-dutycycle
python3 tools/flipper-dutycycle/build.py
python3 tools/flipper-dutycycle/build.py test
```

The app's ignored `.venv` retains its Python environment, firmware 1.5.1 SDK,
and ARM compiler across reboots. See the app README for installation, threshold
calibration, controls, and CSV details.

1. Run a short smoke test with both devices in the shielded setup. Calibrate the
   Flipper threshold, select 60 seconds, arm it, and run `flood 60`. Confirm
   the recent graph shows packet gaps, a CSV was saved, and analysis succeeds.
2. For a fresh-budget run, reboot the repeater with the RF and 10% duty-cycle
   settings saved. Confirm the settings and avoid other traffic during the run.
   On the Flipper select `120:00` and arm. Start `flood 7200` on the local
   terminal. Keep the repeater running across both hours.
3. After the Flipper reports `CSV saved`, copy the new file from
   `apps_data/mc_dutycycle/` on the SD card. Run:

   ```sh
   python3 tools/flipper-dutycycle/analyze_capture.py /path/to/capture.csv
   ```

The capture clock starts on the first threshold crossing, while the repeater's
clock starts when the command is accepted. A final frame already transmitting
may extend past either nominal deadline. The report compares the first and
second capture hours and the busiest complete rolling 60-minute window.
The Flipper measures energy above the threshold at 869.618 MHz. Other signals
in its receive channel will contribute to the result.

Native producer tests exercise the dispatcher with a simulated radio:

```sh
pio test -e native -f test_flood_test
```

These tests cover command limits, remote rejection, deadline cancellation,
one outstanding packet, preserving other queue entries, packet airtime
admission, failed transmissions, and clock wrap. Actual RF behavior requires
validation with the attended hardware setup.
