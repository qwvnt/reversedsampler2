# Third-party code

| Folder | Version | License | Source |
|---|---|---|---|
| `signalsmith-stretch` | 1.4.0 | MIT | https://github.com/Signalsmith-Audio/signalsmith-stretch |
| `signalsmith-linear` | 0.6.4 | MIT | https://github.com/Signalsmith-Audio/linear |

Copies of the headers (tests, examples and web demos are left out), with one addition in each library:
`copyStateFrom()` on `DynamicSTFT` and `SignalsmithStretch` (marked "Reverse Sampler addition"), which copies the
processing state between identically configured engines so a pre-rolled note start can be cached.
Signalsmith Stretch does the time-stretching and pitch-shifting of the layers.
