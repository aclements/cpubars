cpubars is a simple terminal-based tool for monitoring CPU load in
real-time, especially tailored to monitoring large multicores over
SSH.

![Screenshot](/../screenshots/80.png)

cpubars uses Unicode block drawing characters to show the precise
breakdown of CPU usage for each CPU.

cpubars itself consumes almost no CPU, so it won't perturb other
processes like benchmarks, and uses the terminal efficiently (and
hence remote connections over SSH, Mosh, etc.).


Terminal support
----------------

cpubars can operate in either Unicode or ASCII mode.  In Unicode mode,
cpubars can show fine-grained divisions in each CPU usage bar.  ASCII
mode is more broadly compatible, but can only show coarse-grained
divisions.

If cpubars looks funky, pass `-a` to force it into ASCII mode.
cpubars will default to Unicode if your locale advertises Unicode
support (e.g., the `LANG` environment variable is `en_US.utf8`), but
this doesn't necessarily mean your terminal supports Unicode (or that
it supports it correctly).
