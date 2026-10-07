# The demodulator's tests

`rf_demod.c` and `rf_dsp.c` are plain C, so they build and run on the Mac:

```bash
cc -O1 -g -fsanitize=address,undefined -DRF_HOST_TEST -Iapps/rf/main \
   apps/rf/main/rf_demod.c apps/rf/main/rf_dsp.c apps/rf/test/demod_test.c -o /tmp/demod_test -lm
python3 apps/rf/test/gen_iq.py am 2400000 /tmp/am.cu8 1
/tmp/demod_test am 2400000 /tmp/am.cu8 /tmp/am.wav
```

`gen_iq.py` makes I/Q as an RTL2832 gives it (cu8, noise, a DC offset), with
a neighbour in each case: broadcast FM with a 1 kHz tone and the 19 kHz pilot
and a stronger station 200 kHz away; AM 1.2 kHz off the centre (a tuner's
error) with another carrier 25 kHz away; narrow FM keyed on for the middle
half only, with another 12.5 kHz away. `demod_test` prints the strongest
audio tone and how far it stands over the rest, how often the squelch was
open, the pilot meter, and the time taken; `RF_TRACE=1` follows the squelch,
`RF_READ=n` changes the reads' size.

Measured on 2026-10-07 (the 16-bit chain), the 1 kHz tone over the rest:

| | 2.4 M | 1.92 M | 960 k | 240 k |
|---|---|---|---|---|
| Broadcast FM | 53.2 dB | 53.3 | 53.4 | (the neighbour folds in: the RTL2832 filters it in real life) |
| AM | 52.5 | 51.9 | 49.7 | 45.0 |
| Narrow FM | 38.9 | 38.7 | 38.9 | 38.4 |

The on-off keying decoders, the same way:

```bash
cc -O1 -g -fsanitize=address,undefined -DRF_HOST_TEST -Iapps/rf/main \
   apps/rf/main/rf_ook.c apps/rf/test/ook_test.c -o /tmp/ook_test -lm
python3 apps/rf/test/gen_ook.py 240000 /tmp/ook.cu8
/tmp/ook_test 240000 /tmp/ook.cu8
```

`gen_ook.py` sends an EV1527 remote (6 repeats), a PT2262 one (4), a Nexus
sensor 10 dB weaker (3) and a Prologue one (3), each off the centre by up to
80 kHz. Every repeat comes out right at 240 k and 960 k, the offsets to a few
Hz; with everything 18 dB weaker, 6 of 6 EV1527 repeats, 4 of 4 PT2262 and 3
of 3 Prologue (the Nexus, 28 dB down, is lost).

Its noise is far smaller than a real stick's, and that hid a bug until a real
remote came (2026-10-07): the floor sat at the noise's low quantiles, the
board's noise opened trains by itself and kept them open, and the remote's
repeats were cut anywhere (one 24-bit code whole in 14 trains). An I/Q
recording made on the board is the test for that: `ook_test` on two minutes
of a copier remote's two buttons, three presses each, gives 24 + 25 EV1527
repeats and no noise train. Recordings stay out of git (56 MB); make one with
the app (Save, Record the signal) or `rec_iq=1` / `rec_iq=0` over
`/api/live`.

In the simulator, `RF_IQ_FILE=/tmp/am.cu8@960000` makes such a file the
app's source, in real time and in a loop, and `RF_WAV_OUT=/tmp/out.raw` keeps
what the speaker would have played (raw 16-bit mono at the mode's rate).
